#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_defs.h"
#include "esp_gap_ble_api.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "common.h"
#include "bus_state.h"
#include "ble_scanner.h"
#include "beacon_config.h"
#include "bus_mqtt.h"

static const char *TAG = "BLE";

#define SCAN_INTERVAL_MS   CONFIG_BLE_SCAN_INTERVAL_MS
#define SCAN_WINDOW_MS      CONFIG_BLE_SCAN_WINDOW_MS
#define SCAN_INTERVAL_UNITS ((SCAN_INTERVAL_MS * 1000) / 625)
#define SCAN_WINDOW_UNITS   ((SCAN_WINDOW_MS * 1000) / 625)
#define ABSENT_TIMEOUT_S    CONFIG_BLE_BEACON_ABSENT_TIMEOUT_S
#define NEAR_SUSTAIN_S      CONFIG_BLE_BEACON_NEAR_SUSTAIN_S
#define FAR_HYSTERESIS      CONFIG_BLE_BEACON_FAR_HYSTERESIS_DB
#define MIN_NEAR_RSSI       CONFIG_BLE_BEACON_MIN_NEAR_RSSI

typedef struct {
    uint8_t mac[6];
    int rssi;
    uint8_t adv_data[62];
    uint8_t adv_data_len;
} ble_scan_result_t;

static QueueHandle_t s_result_queue = NULL;
static int s_scan_count = 0;
static int s_adv_count = 0;
static int64_t s_last_ble_log = 0;
static int64_t s_last_scan_result_us = 0;
static volatile uint32_t s_scan_queue_drops = 0;

static void process_device(const uint8_t *mac, int rssi, const uint8_t *adv_data, uint8_t adv_len)
{
    beacon_entry_t *entry = beacon_config_find_by_mac(mac);
    if (!entry) {
        int pos = 0;
        while (pos + 1 < adv_len) {
            uint8_t len = adv_data[pos];
            if (len == 0) break;
            if (pos + 1 + len > adv_len) break;
            uint8_t type = adv_data[pos + 1];
            if (type == 0xFF && len >= 26) {
                const uint8_t *data = &adv_data[pos + 2];
                uint16_t company_id = data[0] | (data[1] << 8);
                uint16_t beacon_type = data[2] | (data[3] << 8);
                if (company_id == 0x004C && beacon_type == 0x1502) {
                    const uint8_t *uuid = &data[4];
                    uint16_t major = (data[20] << 8) | data[21];
                    uint16_t minor = (data[22] << 8) | data[23];
                    entry = beacon_config_find_by_ibeacon(uuid, major, minor);
                    if (entry) break;
                }
            }
            pos += len + 1;
        }
    }

    if (!entry) return;

    int64_t now = esp_timer_get_time();
    entry->rssi = rssi;
    entry->last_seen = now;

    beacon_state_t old_state = entry->state;
    beacon_state_t new_state = entry->state;
    int effective_threshold = beacon_config_effective_rssi_threshold(entry);

    /* Hysteresis: hold previous state inside the band
     * (effective_threshold - FAR_HYSTERESIS <= rssi < effective_threshold).
     * The effective threshold clamps each roster threshold to
     * CONFIG_BLE_BEACON_MIN_NEAR_RSSI so a permissive per-child value cannot
     * make distant beacons count as inside the bus. Only transition FAR ->
     * NEAR-candidate when rssi >= effective_threshold (then accumulate
     * sustain), and only NEAR -> FAR when rssi drops below
     * effective_threshold - hysteresis.
     * This prevents near/far churn when a beacon hovers around threshold
     * (e.g. child seated behind a seat) — the previous implementation reset
     * near_since on every dip into the band, defeating the sustain timer. */
    if (rssi < effective_threshold - FAR_HYSTERESIS) {
        /* Strongly below threshold + hysteresis: definitely far. */
        new_state = BEACON_FAR;
        entry->near_since = 0;
    } else if (rssi >= effective_threshold) {
        /* At/above threshold: begin or continue near sustain. */
        if (entry->near_since == 0) {
            entry->near_since = now;
        }
        int64_t elapsed_s = (now - entry->near_since) / 1000000;
        if (elapsed_s >= NEAR_SUSTAIN_S) {
            new_state = BEACON_NEAR;
        }
    } else {
        /* Hysteresis band: hold previous state. If currently NEAR, keep
         * the sustain counter intact so a brief dip doesn't drop us to FAR.
         * If currently FAR (or ABSENT), the reading is still below the near
         * threshold, so reset the candidate timer: near sustain means
         * continuously at/above threshold, not "mostly close enough". */
        if (old_state == BEACON_NEAR) {
            new_state = BEACON_NEAR;
        } else {
            new_state = BEACON_FAR;
            entry->near_since = 0;
        }
    }

    if (new_state != old_state) {
        entry->state = new_state;

        const char *state_str = "absent";
        if (new_state == BEACON_NEAR) state_str = "near";
        else if (new_state == BEACON_FAR) state_str = "far";

        mqtt_publish_beacon_state(entry->name, state_str, rssi);
        ESP_LOGI(TAG, "%s: %s -> %s (RSSI=%d, threshold=%d, configured=%d)",
                 entry->name,
                 old_state == BEACON_NEAR ? "near" : old_state == BEACON_FAR ? "far" : "absent",
                 state_str, rssi, effective_threshold, entry->rssi_threshold);

        if (new_state == BEACON_NEAR && bus_state_is_armed()) {
            distress_event_t evt = {
                .type = DISTRESS_BEACON_NEAR,
                .timestamp = now,
            };
            strncpy(evt.beacon_name, entry->name, sizeof(evt.beacon_name) - 1);
            if (xQueueSend(g_distress_queue, &evt, 0) != pdTRUE) {
                ESP_LOGW(TAG, "Distress queue full, dropping beacon_near event");
            }
        }
    }
}

static void check_absent(void)
{
    int64_t now = esp_timer_get_time();
    int64_t timeout_us = ABSENT_TIMEOUT_S * 1000000LL;

    for (int i = 0; i < beacon_config_count(); i++) {
        beacon_entry_t *entry = beacon_config_get(i);
        if (!entry || !entry->active) continue;

        if (entry->state != BEACON_ABSENT && (now - entry->last_seen) > timeout_us) {
            beacon_state_t old_state = entry->state;
            entry->state = BEACON_ABSENT;
            entry->near_since = 0;
            mqtt_publish_beacon_state(entry->name, "absent", 0);
            ESP_LOGI(TAG, "%s: %s -> absent (timeout)", entry->name,
                     old_state == BEACON_NEAR ? "near" : "far");

            /* Advisory: while armed, a beacon that was near going absent is
             * ambiguous - either the child left (good) or the beacon battery
             * died (bad - the child could still be inside but is now invisible
             * to BLE). Emit bus/event so the relay can warn; CO2/LD2412 are
             * still the backstop signals. */
            if (bus_state_is_armed() && old_state == BEACON_NEAR) {
                cJSON *evt = cJSON_CreateObject();
                if (evt) {
                    cJSON_AddStringToObject(evt, "event", "beacon_silent_while_armed");
                    cJSON_AddStringToObject(evt, "beacon_id", entry->name);
                    mqtt_publish_event_json(evt);
                    cJSON_Delete(evt);
                }
            }
        }
    }
}

static void gap_callback(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
        esp_ble_gap_start_scanning(0);
        break;

    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
        if (param->scan_start_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "Scan started");
        }
        break;

    case ESP_GAP_BLE_SCAN_RESULT_EVT:
        if (param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
            ble_scan_result_t result;
            memcpy(result.mac, param->scan_rst.bda, 6);
            result.rssi = param->scan_rst.rssi;
            result.adv_data_len = param->scan_rst.adv_data_len;
            if (result.adv_data_len > 62) result.adv_data_len = 62;
            memcpy(result.adv_data, param->scan_rst.ble_adv, result.adv_data_len);
            if (xQueueSend(s_result_queue, &result, 0) != pdTRUE) {
                s_scan_queue_drops++;
            }
        }
        break;

    default:
        break;
    }
}

void ble_scan_task(void *pvParameters)
{
    s_result_queue = xQueueCreate(50, sizeof(ble_scan_result_t));

    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());
    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_callback));

    esp_ble_scan_params_t scan_params = {
        .scan_type = BLE_SCAN_TYPE_ACTIVE,
        .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
        .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
        .scan_interval = SCAN_INTERVAL_UNITS,
        .scan_window = SCAN_WINDOW_UNITS,
        .scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE,
    };
    ESP_ERROR_CHECK(esp_ble_gap_set_scan_params(&scan_params));

    ESP_LOGI(TAG, "Init (interval=%dms, window=%dms, min_near_rssi=%d)",
             SCAN_INTERVAL_MS, SCAN_WINDOW_MS, MIN_NEAR_RSSI);

    ble_scan_result_t result;
    int64_t last_absent_check = 0;
    s_last_ble_log = esp_timer_get_time();

    while (1) {
        if (xQueueReceive(s_result_queue, &result, pdMS_TO_TICKS(1000)) == pdTRUE) {
            s_scan_count++;
            s_adv_count++;
            s_last_scan_result_us = esp_timer_get_time();
            beacon_config_lock();
            process_device(result.mac, result.rssi, result.adv_data, result.adv_data_len);
            beacon_config_unlock();
        }

        int64_t now = esp_timer_get_time();
        if (now - s_last_ble_log > 15000000) {
            uint32_t drops = s_scan_queue_drops;
            ESP_LOGI(TAG, "stats: %d scans, %d adv in 15s (queue_drops=%lu)",
                     s_scan_count, s_adv_count, (unsigned long)drops);
            s_scan_count = 0;
            s_adv_count = 0;
            s_last_ble_log = now;
        }
        if (now - last_absent_check > 5000000) {
            beacon_config_lock();
            check_absent();
            beacon_config_unlock();
            last_absent_check = now;
        }

        /* Sensor health: track last scan result arrival. If the BLE controller
         * dies or the stack is wedged, no scan results arrive for 90s ->
         * STALE. Without this, mqtt_publish_bus_state would report ble_status=ok
         * forever even when the scanner has effectively stopped. */
        sensor_status_t ble_status = SENSOR_STATUS_OK;
        int64_t last_scan_us = s_last_scan_result_us;
        if (now - last_scan_us > 90LL * 1000000LL) {
            ble_status = SENSOR_STATUS_STALE;
        }
        if (xSemaphoreTake(g_telemetry_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            g_telemetry.ble_last_scan_us = last_scan_us;
            g_telemetry.ble_scan_drop_count = s_scan_queue_drops;
            if (g_telemetry.ble_status != ble_status) {
                g_telemetry.ble_status = ble_status;
            }
            xSemaphoreGive(g_telemetry_mutex);
        }
    }
}
