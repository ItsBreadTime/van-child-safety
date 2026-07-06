#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "common.h"
#include "bus_state.h"
#include "notifications.h"
#include "bus_mqtt.h"
#include "secrets.h"

static const char *TAG = "NOTIF";

#define INITIAL_DELAY_S  CONFIG_DISTRESS_INITIAL_DELAY_S
#define REPEAT_INTERVAL_S CONFIG_DISTRESS_REPEAT_INTERVAL_S

typedef enum {
    NOTIF_IDLE,
    NOTIF_TRIGGERED,
    NOTIF_ACTIVE,
} notif_state_t;

typedef struct {
    notif_state_t state;
    int64_t trigger_time;
    int64_t last_send;
    distress_event_t event;
} notif_tracker_t;

static const char *trigger_names[] = {"beacon_near", "co2_rise", "presence_detected"};

/* Tracker is module-scope so reconnect and ack-during-pending paths can reach it. */
static notif_tracker_t s_tracker = {0};

static bool event_has_better_identity(const distress_event_t *current,
                                      const distress_event_t *candidate)
{
    if (!current || !candidate) return false;
    if (current->type == DISTRESS_BEACON_NEAR) return false;
    return candidate->type == DISTRESS_BEACON_NEAR && candidate->beacon_name[0] != '\0';
}

static bool event_has_better_trigger(const distress_event_t *current,
                                     const distress_event_t *candidate)
{
    if (!current || !candidate) return false;
    if (event_has_better_identity(current, candidate)) return true;
    return current->type == DISTRESS_PRESENCE_DETECTED &&
           candidate->type == DISTRESS_CO2_RISE;
}

/* Distress queue ack flush: when an ack arrives during the pending window
 * (notification task has the event but hasn't broadcast yet), the tracker is
 * reset to IDLE and a distress_clear event is emitted without ever sending. */
void notification_handle_ack(void)
{
    bool had_pending = (s_tracker.state == NOTIF_TRIGGERED);
    if (s_tracker.state != NOTIF_IDLE) {
        ESP_LOGW(TAG, "Distress acked (state was %d); silencing", s_tracker.state);
    }
    s_tracker.state = NOTIF_IDLE;
    s_tracker.last_send = 0;
    if (had_pending) {
        /* Operator cancelled before broadcast. Emit distress_clear so the
         * retained MQTT state and the relay both see this incident as closed. */
        bus_state_clear_distress();
    }
}

void notification_republish_distress(void)
{
    if (s_tracker.state != NOTIF_ACTIVE && s_tracker.state != NOTIF_TRIGGERED) return;
    if (bus_state_distress_acknowledged()) return;
    mqtt_publish_distress(&s_tracker.event);
    ESP_LOGW(TAG, "Distress re-published on MQTT reconnect: %s",
             trigger_names[s_tracker.event.type]);
}

static void send_discord(const char *message)
{
#if !CONFIG_ENABLE_LEGACY_DISCORD_WEBHOOK
    (void)message;
    return;
#else
    if (DISCORD_WEBHOOK_URL[0] == '\0') return;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "content", message);
    char *json = cJSON_PrintUnformatted(root);

    esp_http_client_config_t config = {
        .url = DISCORD_WEBHOOK_URL,
        .method = HTTP_METHOD_POST,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        free(json);
        cJSON_Delete(root);
        return;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json, strlen(json));

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "Discord: %d", status);
    } else {
        ESP_LOGE(TAG, "Discord failed: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
    free(json);
    cJSON_Delete(root);
#endif
}

static void build_message(const distress_event_t *evt, char *buf, int buf_len)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm_buf;
    gmtime_r(&tv.tv_sec, &tm_buf);
    char timebuf[32];
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);

    float lat = 0, lon = 0;
    if (xSemaphoreTake(g_telemetry_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        lat = g_telemetry.latitude;
        lon = g_telemetry.longitude;
        xSemaphoreGive(g_telemetry_mutex);
    }

    switch (evt->type) {
    case DISTRESS_BEACON_NEAR:
        snprintf(buf, buf_len,
                 "\xf0\x9f\x9a\xa8 **CHILD IN VAN ALERT**\n"
                 "Trigger: Beacon '%s' detected near bus\n"
                 "GPS: %.6f, %.6f\nTime: %s",
                 evt->beacon_name, lat, lon, timebuf);
        break;
    case DISTRESS_CO2_RISE:
        snprintf(buf, buf_len,
                 "\xf0\x9f\x9a\xa8 **CHILD IN VAN ALERT**\n"
                 "Trigger: CO2 rising +%dppm (bus empty)\n"
                 "GPS: %.6f, %.6f\nTime: %s",
                 evt->co2_delta, lat, lon, timebuf);
        break;
    case DISTRESS_PRESENCE_DETECTED:
        snprintf(buf, buf_len,
                 "\xf0\x9f\x9a\xa8 **CHILD IN VAN ALERT**\n"
                 "Trigger: Presence detected (bus empty)\n"
                 "GPS: %.6f, %.6f\nTime: %s",
                 lat, lon, timebuf);
        break;
    }
}

void notification_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Init (initial=%ds, repeat=%ds)", INITIAL_DELAY_S, REPEAT_INTERVAL_S);

    distress_event_t evt;
    while (1) {
        if (xQueueReceive(g_distress_queue, &evt, pdMS_TO_TICKS(1000)) == pdTRUE) {
            if (s_tracker.state == NOTIF_IDLE && bus_state_is_armed()) {
                s_tracker.state = NOTIF_TRIGGERED;
                s_tracker.trigger_time = esp_timer_get_time();
                s_tracker.last_send = 0;
                s_tracker.event = evt;
                if (bus_state_mark_distress_pending()) {
                    ESP_LOGI(TAG, "Triggered: %s", trigger_names[evt.type]);
                } else {
                    /* State changed (ignition/disarm) between the armed check
                     * and transition. Fail safe without reviving an alarm in
                     * the new state. */
                    s_tracker.state = NOTIF_IDLE;
                }
            } else {
                if (s_tracker.state != NOTIF_IDLE &&
                    bus_state_is_armed() &&
                    event_has_better_trigger(&s_tracker.event, &evt)) {
                    ESP_LOGW(TAG, "Updating active distress evidence: %s -> %s",
                             trigger_names[s_tracker.event.type],
                             trigger_names[evt.type]);
                    s_tracker.event = evt;
                    if (s_tracker.state == NOTIF_ACTIVE &&
                        !bus_state_distress_acknowledged()) {
                        mqtt_publish_distress(&s_tracker.event);
                    }
                } else {
                    ESP_LOGI(TAG, "Dropping distress event (tracker busy or not armed): %s",
                             trigger_names[evt.type]);
                }
            }
        }

        int64_t now = esp_timer_get_time();

        bus_state_t current_state = bus_state_get();
        if (current_state == BUS_STATE_ACTIVE || current_state == BUS_STATE_ARMED ||
            current_state == BUS_STATE_DISARMED) {
            if (s_tracker.state != NOTIF_IDLE && !bus_state_distress_active()) {
                /* Distress was cleared (either by ack-during-pending or by
                 * distress_clear). Reset to idle. */
                s_tracker.state = NOTIF_IDLE;
            }
        }

        if (bus_state_distress_acknowledged()) {
            /* Once acked, stop repeating until clear. The tracker stays
             * non-IDLE so the acked state is recoverable; only `distress_clear`
             * or ignition-on returns us to IDLE via the state transition above
             * (CLEAR/ACTIVE both reset distress_active=false). */
            continue;
        }

        if (s_tracker.state == NOTIF_TRIGGERED) {
            int64_t elapsed_s = (now - s_tracker.trigger_time) / 1000000;
            if (elapsed_s >= INITIAL_DELAY_S) {
                if (!bus_state_mark_distress_active()) {
                    s_tracker.state = NOTIF_IDLE;
                    continue;
                }
                s_tracker.state = NOTIF_ACTIVE;
                s_tracker.last_send = now;

                char msg[512];
                build_message(&s_tracker.event, msg, sizeof(msg));
                send_discord(msg);
                mqtt_publish_distress(&s_tracker.event);
                ESP_LOGW(TAG, "Distress sent: %s", trigger_names[s_tracker.event.type]);
            }
        } else if (s_tracker.state == NOTIF_ACTIVE) {
            int64_t since_last_s = (now - s_tracker.last_send) / 1000000;
            if (since_last_s >= REPEAT_INTERVAL_S) {
                s_tracker.last_send = now;

                char msg[512];
                build_message(&s_tracker.event, msg, sizeof(msg));
                send_discord(msg);
                mqtt_publish_distress(&s_tracker.event);
                ESP_LOGW(TAG, "Distress repeat: %s", trigger_names[s_tracker.event.type]);
            }
        }
    }
}
