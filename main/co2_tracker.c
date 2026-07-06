#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "common.h"
#include "bus_state.h"
#include "co2_tracker.h"

static const char *TAG = "CO2TRK";

#define WINDOW_SIZE CONFIG_CO2_WINDOW_READINGS
#define RISE_THRESHOLD CONFIG_CO2_RISE_THRESHOLD_PPM
#define CO2_STALE_S 90
#define REQUIRE_DELTA_3MIN_RISING CONFIG_CO2_REQUIRE_DELTA_3MIN_RISING

static uint16_t s_buffer[WINDOW_SIZE];
static int s_idx = 0;
static int s_count = 0;
static uint32_t s_seen_armed_generation = 0;
static int s_consecutive_rise = 0;
static bool s_alerted_this_armed = false;

void co2_tracker_task(void *pvParameters)
{
    uint16_t co2;
    xQueuePeek(g_co2_queue, &co2, portMAX_DELAY);

    ESP_LOGI(TAG, "Init (window=%d, threshold=%dppm)", WINDOW_SIZE, RISE_THRESHOLD);

    while (1) {
        if (xQueueReceive(g_co2_queue, &co2, portMAX_DELAY) != pdTRUE) continue;

        uint32_t armed_generation = bus_state_armed_generation();
        if (armed_generation != s_seen_armed_generation) {
            s_seen_armed_generation = armed_generation;
            s_consecutive_rise = 0;
            s_alerted_this_armed = false;
            ESP_LOGI(TAG, "CO2 armed generation reset; baseline remains %d ppm",
                     bus_state_co2_baseline());
        }

        s_buffer[s_idx] = co2;
        s_idx = (s_idx + 1) % WINDOW_SIZE;
        if (s_count < WINDOW_SIZE) s_count++;

        if (bus_state_is_armed() && bus_state_co2_baseline() == 0 && co2 > 0) {
            bus_state_set_co2_baseline(co2);
            if (xSemaphoreTake(g_telemetry_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                g_telemetry.co2_delta_since_armed = 0;
                xSemaphoreGive(g_telemetry_mutex);
            }
            s_consecutive_rise = 0;
            ESP_LOGI(TAG, "CO2 baseline initialized after arming at %d ppm", co2);
        }

        if (s_count >= 2) {
            int oldest_idx = (s_idx - s_count + WINDOW_SIZE) % WINDOW_SIZE;
            int32_t delta_raw = (int32_t)co2 - (int32_t)s_buffer[oldest_idx];
            int16_t delta = (delta_raw > 32767) ? 32767 : (delta_raw < -32768) ? -32768 : (int16_t)delta_raw;

            if (xSemaphoreTake(g_telemetry_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                g_telemetry.co2_delta_3min = delta;
                int32_t armed_delta = (int32_t)co2 - (int32_t)bus_state_co2_baseline();
                if (armed_delta > 32767) armed_delta = 32767;
                if (armed_delta < -32768) armed_delta = -32768;
                g_telemetry.co2_delta_since_armed = (int16_t)armed_delta;
                xSemaphoreGive(g_telemetry_mutex);
            }

            int32_t since_armed = (int32_t)co2 - (int32_t)bus_state_co2_baseline();

            bool stale = false;
            int16_t delta_3min_snapshot = 0;
            if (xSemaphoreTake(g_telemetry_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                int64_t age_us = g_telemetry.co2_last_update_us > 0
                    ? (esp_timer_get_time() - g_telemetry.co2_last_update_us) : -1;
                stale = (age_us < 0) || (age_us > (int64_t)CO2_STALE_S * 1000000LL);
                delta_3min_snapshot = g_telemetry.co2_delta_3min;
                xSemaphoreGive(g_telemetry_mutex);
            }

            /* Trigger requires: armed + since-armed delta >= threshold +
             * not stale + (optionally) 3-min rolling delta also positive.
             * The 3-min gate filters out sensor noise: SCD4x low-power
             * readings have ~+/-50 ppm noise; requiring the 3-min trend
             * to agree with the since-armed trend cuts false positives from
             * a single noisy reading that drifts above threshold. */
            bool trigger_condition = bus_state_is_armed() && since_armed >= RISE_THRESHOLD && !stale;
            if (trigger_condition && REQUIRE_DELTA_3MIN_RISING && delta_3min_snapshot <= 0) {
                trigger_condition = false;
            }
            if (trigger_condition) {
                s_consecutive_rise++;
                if (!s_alerted_this_armed && s_consecutive_rise >= 2) {
                    distress_event_t evt = {
                        .type = DISTRESS_CO2_RISE,
                        .co2_delta = (uint16_t)since_armed,
                        .timestamp = esp_timer_get_time(),
                    };
                    if (xQueueSend(g_distress_queue, &evt, 0) != pdTRUE) {
                        ESP_LOGW(TAG, "Distress queue full, dropping CO2 rise event");
                    }
                    s_alerted_this_armed = true;
                    ESP_LOGW(TAG, "CO2 rising +%ldppm since armed (delta_3min=%d)",
                             (long)since_armed, delta_3min_snapshot);
                }
            } else {
                s_consecutive_rise = 0;
                if (!bus_state_is_armed()) s_alerted_this_armed = false;
                if (stale && bus_state_is_armed()) {
                    ESP_LOGW(TAG, "CO2 reading stale >%ds, ignoring trigger", CO2_STALE_S);
                }
            }

            if (xSemaphoreTake(g_telemetry_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                g_telemetry.co2_consecutive_rise_count = (uint8_t)s_consecutive_rise;
                xSemaphoreGive(g_telemetry_mutex);
            }
        }
    }
}
