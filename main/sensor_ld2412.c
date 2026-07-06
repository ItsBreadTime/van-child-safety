#include <string.h>
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "common.h"
#include "bus_state.h"
#include "sensor_ld2412.h"

static const char *TAG = "LD2412";

static int s_frame_count = 0;
static int s_byte_count = 0;
static int64_t s_last_log = 0;

typedef enum {
    ST_IDLE, ST_H1, ST_H2, ST_H3,
    ST_LEN1, ST_LEN2, ST_CMD1, ST_CMD2,
    ST_DATA, ST_E1, ST_E2, ST_E3, ST_E4,
} parser_state_t;

static int s_state = ST_IDLE;
static uint8_t s_data[64];
static int s_data_idx = 0;
static uint16_t s_data_len = 0;
static uint16_t s_cmd = 0;
static uint8_t s_std_type = 0;
static int64_t s_target_since = 0;
static uint32_t s_reported_generation = 0;
static uint32_t s_seen_armed_generation = 0;
static int64_t s_target_dropped_at = 0;  /* 0 = not currently in drop-debounce */
static bool s_target_was_present_at_arm = false;

#define PRESENCE_SUSTAIN_S CONFIG_PRESENCE_SUSTAIN_S
#define PRESENCE_SUSTAIN_ARMED_EXISTING_S CONFIG_PRESENCE_SUSTAIN_ARMED_EXISTING_S
#define PRESENCE_TARGET_DROP_DEBOUNCE_MS CONFIG_PRESENCE_TARGET_DROP_DEBOUNCE_MS
#define PRESENCE_STILL_DISTANCE_MAX_CM CONFIG_PRESENCE_STILL_DISTANCE_MAX_CM
#define PRESENCE_STILL_ENERGY_MIN CONFIG_PRESENCE_STILL_ENERGY_MIN

/* Loaded at task start from Kconfig so the compiler can't fold the gate
 * comparisons into dead-code-in-calendar (which it would for the
 * PRESENCE_STILL_ENERGY_MIN=0 default, generating a -Wtype-limits warning
 * for the uint8_t < 0 check). */
static int s_sustain_s = PRESENCE_SUSTAIN_S;
static int s_sustain_armed_existing_s = PRESENCE_SUSTAIN_ARMED_EXISTING_S;
static int s_drop_debounce_ms = PRESENCE_TARGET_DROP_DEBOUNCE_MS;
static int s_still_distance_max_cm = PRESENCE_STILL_DISTANCE_MAX_CM;
static int s_still_energy_min = PRESENCE_STILL_ENERGY_MIN;

static void evaluate_presence_trigger(bool has_target, uint16_t still_distance,
                                       uint8_t still_energy);

#define ST_F4_HDR  50
#define ST_F4_F3   51
#define ST_F4_F2   52
#define ST_F4_F1   53
#define ST_SD_LENL 54
#define ST_SD_LENH 55
#define ST_SD_TYPE 56
#define ST_SD_AA   57
#define ST_SD_DATA 58
#define ST_SD_F8   59
#define ST_SD_F7   60
#define ST_SD_F6   61
#define ST_SD_F5   62

static void process_frame_eng(uint16_t cmd, const uint8_t *data, int len)
{
    if ((cmd == 0xAA02 || cmd == 0x0101) && len >= 9) {
        bool has_target = data[0] != 0;
        bool has_moving = (data[0] == 1 || data[0] == 3);
        bool has_still = (data[0] == 2 || data[0] == 3);
        uint16_t moving_dist = data[1] | (data[2] << 8);
        uint8_t moving_energy = data[3];
        uint16_t still_dist = data[4] | (data[5] << 8);
        uint8_t still_energy = data[6];
        uint16_t detect_dist = 0;
        if (len >= 9) detect_dist = data[7] | (data[8] << 8);

        if (xSemaphoreTake(g_telemetry_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            g_telemetry.ld2412_target = has_target;
            g_telemetry.ld2412_moving = has_moving;
            g_telemetry.ld2412_still = has_still;
            g_telemetry.moving_distance = moving_dist;
            g_telemetry.still_distance = still_dist;
            g_telemetry.moving_energy = moving_energy;
            g_telemetry.still_energy = still_energy;
            g_telemetry.detection_distance = detect_dist;
            g_telemetry.ld2412_last_frame_us = esp_timer_get_time();
            xSemaphoreGive(g_telemetry_mutex);
        }

        s_frame_count++;
        evaluate_presence_trigger(has_target, still_dist, still_energy);
    }
}

static void evaluate_presence_trigger(bool has_target, uint16_t still_distance,
                                       uint8_t still_energy)
{
    int64_t now = esp_timer_get_time();
    uint32_t generation = bus_state_armed_generation();

    /* Track arm-time snapshot: when the armed generation changes, record
     * whether the target was already present at that moment. If it was,
     * require a longer sustain to reject the driver's residual signature. */
    if (generation != s_seen_armed_generation) {
        s_seen_armed_generation = generation;
        s_reported_generation = 0;
        s_target_was_present_at_arm = has_target;
        if (has_target) {
            s_target_since = now;
            s_target_dropped_at = 0;
        }
        /* Measure dwell from the armed boundary for targets already present
         * at arming. Otherwise a driver/radar residual that existed before
         * arming can satisfy the sustain timer immediately. */
    }

    if (has_target) {
        /* Clear any in-progress drop debounce; target is back. */
        s_target_dropped_at = 0;
        if (s_target_since == 0) s_target_since = now;
    } else {
        /* Debounce: real frames can briefly report has_target=0 during a
         * still/moving handoff. Require a sustained drop before clearing
         * s_target_since, so a one-frame blip doesn't reset the sustain
         * timer and force a fresh 5s before re-triggering. */
        if (s_target_dropped_at == 0) {
            s_target_dropped_at = now;
            return;
        }
        if ((now - s_target_dropped_at) < (int64_t)s_drop_debounce_ms * 1000LL) {
            return;
        }
        s_target_since = 0;
        s_target_dropped_at = 0;
        s_reported_generation = 0;
        if (xSemaphoreTake(g_telemetry_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            g_telemetry.ld2412_target_since_us = 0;
            xSemaphoreGive(g_telemetry_mutex);
        }
        return;
    }

    if (xSemaphoreTake(g_telemetry_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        g_telemetry.ld2412_target_since_us = s_target_since;
        xSemaphoreGive(g_telemetry_mutex);
    }

    if (!bus_state_is_armed() || generation == 0 || s_reported_generation == generation) return;

    /* Distance gate: a child seat is usually within ~1.5m of the radar.
     * A still target beyond the configured max distance is likely a far
     * reflection (dashboard, roof) and shouldn't trigger. */
    if (s_still_distance_max_cm < 9999 && (int)still_distance > s_still_distance_max_cm) {
        return;
    }
    /* Energy gate: ignore near-zero energy still targets (radar noise floor). */
    if (s_still_energy_min > 0 && (int)still_energy < s_still_energy_min) {
        return;
    }

    int64_t target_s = (now - s_target_since) / 1000000;
    /* Extended sustain for targets already present at arm time (residual
     * driver presence typically clears within ~10s; a child remains). */
    int required_s = s_target_was_present_at_arm ? s_sustain_armed_existing_s
                                                 : s_sustain_s;
    if (target_s >= required_s) {
        distress_event_t evt = {
            .type = DISTRESS_PRESENCE_DETECTED,
            .timestamp = now,
        };
        if (xQueueSend(g_distress_queue, &evt, 0) != pdTRUE) {
            ESP_LOGW(TAG, "Distress queue full, dropping presence_detected event");
        } else {
            s_reported_generation = generation;
            ESP_LOGW(TAG, "Presence sustained %llds while armed (arm_existent=%d still_dist=%d energy=%d)",
                     (long long)target_s, s_target_was_present_at_arm, still_distance, still_energy);
        }
    }
}

static void process_frame_std(const uint8_t *data, int len)
{
    if (len < 7) return;

    bool has_target = data[0] != 0;
    bool has_moving = (data[0] == 1 || data[0] == 3);
    bool has_still = (data[0] == 2 || data[0] == 3);
    uint16_t moving_dist = data[1] | (data[2] << 8);
    uint8_t moving_energy = data[3];
    uint16_t still_dist = data[4] | (data[5] << 8);
    uint8_t still_energy = data[6];
    uint16_t detect_dist = 0;
    if (len >= 9) detect_dist = data[7] | (data[8] << 8);

    if (xSemaphoreTake(g_telemetry_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        g_telemetry.ld2412_target = has_target;
        g_telemetry.ld2412_moving = has_moving;
        g_telemetry.ld2412_still = has_still;
        g_telemetry.moving_distance = moving_dist;
        g_telemetry.still_distance = still_dist;
        g_telemetry.moving_energy = moving_energy;
        g_telemetry.still_energy = still_energy;
        g_telemetry.detection_distance = detect_dist;
        g_telemetry.ld2412_last_frame_us = esp_timer_get_time();
        xSemaphoreGive(g_telemetry_mutex);
    }

    s_frame_count++;
    evaluate_presence_trigger(has_target, still_dist, still_energy);
}

static void process_byte(uint8_t b)
{
    switch (s_state) {
    case ST_IDLE:
        if (b == 0xF4) { s_state = ST_F4_HDR; break; }
        if (b == 0xFD) s_state = ST_H1;
        break;
    case ST_H1:
        s_state = (b == 0xFC) ? ST_H2 : ST_IDLE;
        break;
    case ST_H2:
        s_state = (b == 0xFB) ? ST_H3 : ST_IDLE;
        break;
    case ST_H3:
        s_state = (b == 0xFA) ? ST_LEN1 : ST_IDLE;
        break;
    case ST_LEN1:
        s_data_len = b;
        s_state = ST_LEN2;
        break;
    case ST_LEN2:
        s_data_len |= (b << 8);
        s_data_idx = 0;
        if (s_data_len < 2 || s_data_len > 66) {
            s_state = ST_IDLE;
        } else {
            s_state = ST_CMD1;
        }
        break;
    case ST_CMD1:
        s_cmd = b;
        s_state = ST_CMD2;
        break;
    case ST_CMD2:
        s_cmd |= (b << 8);
        s_state = (s_data_len > 2) ? ST_DATA : ST_E1;
        break;
    case ST_DATA:
        if (s_data_idx < (int)sizeof(s_data)) s_data[s_data_idx] = b;
        s_data_idx++;
        if (s_data_idx >= (int)(s_data_len - 2)) s_state = ST_E1;
        break;
    case ST_E1:
        s_state = (b == 0x04) ? ST_E2 : ST_IDLE;
        break;
    case ST_E2:
        s_state = (b == 0x03) ? ST_E3 : ST_IDLE;
        break;
    case ST_E3:
        s_state = (b == 0x02) ? ST_E4 : ST_IDLE;
        break;
    case ST_E4:
        if (b == 0x01) {
            process_frame_eng(s_cmd, s_data, s_data_idx);
        }
        s_state = ST_IDLE;
        break;

    /* Standard target data frame:
     * F4 F3 F2 F1 [lenL] [lenH] [type] [0xAA] [data...] [0x55] [0x00] [F8 F7 F6 F5]
     * Length covers: type, 0xAA, data..., 0x55, 0x00 (excludes header and footer)
     */
    case ST_F4_HDR: /* F4 seen */
        s_state = (b == 0xF3) ? ST_F4_F3 : ST_IDLE;
        break;
    case ST_F4_F3: /* F4 F3 seen */
        s_state = (b == 0xF2) ? ST_F4_F2 : ST_IDLE;
        break;
    case ST_F4_F2: /* F4 F3 F2 seen */
        s_state = (b == 0xF1) ? ST_F4_F1 : ST_IDLE;
        break;
    case ST_F4_F1: /* F4 F3 F2 F1 header confirmed */
        s_data_len = b;
        s_state = ST_SD_LENH;
        break;
    case ST_SD_LENH: /* length high byte */
        s_data_len |= (b << 8);
        s_data_idx = 0;
        if (s_data_len < 4 || s_data_len > 60) {
            s_state = ST_IDLE;
        } else {
            s_state = ST_SD_TYPE;
        }
        break;
    case ST_SD_TYPE: /* data type: 0x01=engineering, 0x02=normal */
        s_std_type = b;
        s_state = ST_SD_AA;
        break;
    case ST_SD_AA: /* validate 0xAA marker */
        if (b == 0xAA) {
            s_state = ST_SD_DATA;
        } else {
            s_state = ST_IDLE;
        }
        break;
    case ST_SD_DATA: /* accumulate data payload including 0x55 0x00 suffix */
        if (s_data_idx < (int)sizeof(s_data)) {
            s_data[s_data_idx] = b;
        }
        s_data_idx++;
        /* remaining bytes = s_data_len - 2 (type+AA already consumed) */
        if (s_data_idx >= (int)(s_data_len - 2)) {
            s_state = ST_SD_F8;
        }
        break;
    case ST_SD_F8: /* footer F8 */
        s_state = (b == 0xF8) ? ST_SD_F7 : ST_IDLE;
        break;
    case ST_SD_F7:
        s_state = (b == 0xF7) ? ST_SD_F6 : ST_IDLE;
        break;
    case ST_SD_F6:
        s_state = (b == 0xF6) ? ST_SD_F5 : ST_IDLE;
        break;
    case ST_SD_F5: /* footer complete */
        if (b == 0xF5) {
            int payload = s_data_len - 4;
            if (payload >= 7) {
                process_frame_std(s_data, payload);
            }
            if (s_std_type == 0x01 && s_data_idx >= (37 + 1)) {
                if (xSemaphoreTake(g_telemetry_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                    g_telemetry.ld2412_light = s_data[37];
                    xSemaphoreGive(g_telemetry_mutex);
                }
            }
        }
        s_state = ST_IDLE;
        break;

    default:
        s_state = ST_IDLE;
        break;
    }
}

void ld2412_task(void *pvParameters)
{
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_1, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_1, 7, 8, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_1, 1024, 0, 0, NULL, 0));

    ESP_LOGI(TAG, "Init (TX=7, RX=8, 115200)");

    uint8_t buf[64];
    int64_t last_no_uart_log = 0;
    s_last_log = esp_timer_get_time();
    while (1) {
        int len = uart_read_bytes(UART_NUM_1, buf, sizeof(buf), pdMS_TO_TICKS(100));
        if (len > 0) {
            s_byte_count += len;
            for (int i = 0; i < len; i++) {
                process_byte(buf[i]);
            }
        }
        int64_t now = esp_timer_get_time();
        if (now - s_last_log > 15000000) {
            ESP_LOGI(TAG, "stats: %d frames, %d bytes in 15s", s_frame_count, s_byte_count);
            if (s_byte_count == 0) {
                ESP_LOGW(TAG, "No UART data received — check wiring (TX=7 LD2412-RX, RX=8 LD2412-TX, 115200 baud)");
            }
            s_frame_count = 0;
            s_byte_count = 0;
            s_last_log = now;
        }
        /* Sensor health: NO_UART if zero bytes for >5s, STALE if zero frames
         * for >90s, OK otherwise. Don't spam logs; only log the transition. */
        sensor_status_t new_status = SENSOR_STATUS_OK;
        int64_t last_frame_us = 0;
        if (xSemaphoreTake(g_telemetry_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            last_frame_us = g_telemetry.ld2412_last_frame_us;
            xSemaphoreGive(g_telemetry_mutex);
        }
        if (s_byte_count == 0 && last_frame_us == 0) {
            /* Never seen any data at all. */
            new_status = SENSOR_STATUS_NO_UART;
            if (now - last_no_uart_log > 60000000) {
                ESP_LOGE(TAG, "LD2412 UART has received zero bytes since boot");
                last_no_uart_log = now;
            }
        } else if (last_frame_us > 0 && (now - last_frame_us) > 90LL * 1000000LL) {
            new_status = SENSOR_STATUS_STALE;
        }
        if (xSemaphoreTake(g_telemetry_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            if (g_telemetry.ld2412_status != new_status) {
                g_telemetry.ld2412_status = new_status;
                ESP_LOGW(TAG, "ld2412_status -> %d", new_status);
            }
            xSemaphoreGive(g_telemetry_mutex);
        }
    }
}
