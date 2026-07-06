#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "common.h"
#include "bus_state.h"
#include "bus_mqtt.h"
#include "beacon_config.h"
#include "notifications.h"
#include "ignition.h"

static const char *TAG = "BUS_STATE";

#define EXIT_GRACE_S        CONFIG_EXIT_GRACE_SECONDS
#define CO2_BASELINE_STALE_S 90

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bus_state_t s_state = BUS_STATE_BOOT;
static bool s_ignition_on = false;
/* Override layer above ignition_get(). Set by bus_state_force_empty(); cleared
 * when ignition_get() reports a value different from the override value AND
 * it's debounced-stable (i.e. a real source edge). This fixes a latent bug
 * where force_empty set s_ignition_on directly and the next GPIO poll would
 * silently overwrite it, leaving bus_state_ignition_on() reporting the real
 * GPIO value while the override-intended state was lost. */
static bool s_override_active = false;
static bool s_override_value = false;
/* Previous poll's debounced source value, for real-edge detection when an
 * override is active. The override is cleared when the source's debounced
 * value *changes* (s_last_raw -> raw), not merely when it differs from the
 * override value — this honors "override lasts until next ignition edge". */
static bool s_last_raw = false;
/* Previous poll's effective ignition value (override-applied). Kept at file
 * scope so bus_state_force_empty() can reset it after installing an override,
 * preventing the poll loop from re-triggering a transition force_empty
 * already handled via enter_armed()/enter_state(ACTIVE). */
static bool s_prev_effective = false;
static bool s_distress_active = false;
static bool s_distress_acknowledged = false;
static int64_t s_exit_grace_started = 0;
static uint32_t s_armed_generation = 0;
static uint16_t s_co2_baseline = 0;
static char s_distress_id[48] = {0};
static int64_t s_fault_started_at = 0;
static bool s_fault_reported = false;

static void set_telemetry_state_locked(void)
{
    if (xSemaphoreTake(g_telemetry_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        g_telemetry.ignition_on = s_ignition_on;
        g_telemetry.bus_armed = (s_state == BUS_STATE_ARMED ||
                                s_state == BUS_STATE_DISTRESS_PENDING ||
                                s_state == BUS_STATE_DISTRESS_ACTIVE ||
                                s_state == BUS_STATE_ACKED);
        strncpy(g_telemetry.bus_state, bus_state_name(s_state),
                sizeof(g_telemetry.bus_state) - 1);
        g_telemetry.bus_state[sizeof(g_telemetry.bus_state) - 1] = '\0';
        g_telemetry.co2_baseline = s_co2_baseline;
        xSemaphoreGive(g_telemetry_mutex);
    }
}

const char *bus_state_name(bus_state_t state)
{
    switch (state) {
    case BUS_STATE_BOOT: return "boot";
    case BUS_STATE_ACTIVE: return "active";
    case BUS_STATE_EXIT_GRACE: return "exit_grace";
    case BUS_STATE_ARMED: return "armed";
    case BUS_STATE_DISTRESS_PENDING: return "distress_pending";
    case BUS_STATE_DISTRESS_ACTIVE: return "distress_active";
    case BUS_STATE_ACKED: return "acked";
    case BUS_STATE_DISARMED: return "disarmed";
    case BUS_STATE_FAULT: return "fault";
    default: return "unknown";
    }
}

bus_state_t bus_state_get(void)
{
    bus_state_t state;
    portENTER_CRITICAL(&s_lock);
    state = s_state;
    portEXIT_CRITICAL(&s_lock);
    return state;
}

bool bus_state_is_armed(void)
{
    bus_state_t state = bus_state_get();
    return state == BUS_STATE_ARMED || state == BUS_STATE_DISTRESS_PENDING ||
           state == BUS_STATE_DISTRESS_ACTIVE || state == BUS_STATE_ACKED;
}

bool bus_state_allows_attendance(void)
{
    bus_state_t state = bus_state_get();
    return state == BUS_STATE_ACTIVE || state == BUS_STATE_EXIT_GRACE;
}

bool bus_state_ignition_on(void)
{
    bool ignition;
    portENTER_CRITICAL(&s_lock);
    ignition = s_ignition_on;
    portEXIT_CRITICAL(&s_lock);
    return ignition;
}

bool bus_state_distress_active(void)
{
    bool active;
    portENTER_CRITICAL(&s_lock);
    active = s_distress_active;
    portEXIT_CRITICAL(&s_lock);
    return active;
}

bool bus_state_distress_acknowledged(void)
{
    bool acked;
    portENTER_CRITICAL(&s_lock);
    acked = s_distress_acknowledged;
    portEXIT_CRITICAL(&s_lock);
    return acked;
}

int bus_state_exit_grace_remaining_s(void)
{
    int remaining = 0;
    portENTER_CRITICAL(&s_lock);
    if (s_state == BUS_STATE_EXIT_GRACE && s_exit_grace_started > 0) {
        int64_t elapsed = (esp_timer_get_time() - s_exit_grace_started) / 1000000;
        remaining = EXIT_GRACE_S - (int)elapsed;
        if (remaining < 0) remaining = 0;
    }
    portEXIT_CRITICAL(&s_lock);
    return remaining;
}

uint32_t bus_state_armed_generation(void)
{
    uint32_t generation;
    portENTER_CRITICAL(&s_lock);
    generation = s_armed_generation;
    portEXIT_CRITICAL(&s_lock);
    return generation;
}

static void generate_distress_id_locked(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm_buf;
    gmtime_r(&tv.tv_sec, &tm_buf);
    strftime(s_distress_id, sizeof(s_distress_id), "distress-%Y%m%d-%H%M%S", &tm_buf);
}

const char *bus_state_distress_id(void)
{
    const char *id;
    portENTER_CRITICAL(&s_lock);
    id = s_distress_id[0] ? s_distress_id : NULL;
    portEXIT_CRITICAL(&s_lock);
    return id;
}

void bus_state_set_co2_baseline(uint16_t co2)
{
    portENTER_CRITICAL(&s_lock);
    s_co2_baseline = co2;
    portEXIT_CRITICAL(&s_lock);
    set_telemetry_state_locked();
}

uint16_t bus_state_co2_baseline(void)
{
    uint16_t baseline;
    portENTER_CRITICAL(&s_lock);
    baseline = s_co2_baseline;
    portEXIT_CRITICAL(&s_lock);
    return baseline;
}

static void publish_simple_event(const char *event)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return;
    cJSON_AddStringToObject(root, "event", event);
    cJSON_AddStringToObject(root, "state", bus_state_name(bus_state_get()));
    mqtt_publish_event_json(root);
    cJSON_Delete(root);
}

static void publish_sensor_fault_event(const char *sensor, const char *detail, int duration_s)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return;
    cJSON_AddStringToObject(root, "event", "sensor_fault");
    cJSON_AddStringToObject(root, "state", bus_state_name(bus_state_get()));
    cJSON_AddStringToObject(root, "sensor", sensor ? sensor : "unknown");
    cJSON_AddStringToObject(root, "detail", detail ? detail : "unknown");
    cJSON_AddNumberToObject(root, "duration_s", duration_s);
    mqtt_publish_event_json(root);
    cJSON_Delete(root);
}

static void publish_bus_clear_if_clear(void)
{
    bool target = false;
    bool any_near = beacon_config_any_near();

    if (xSemaphoreTake(g_telemetry_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        target = g_telemetry.ld2412_target;
        xSemaphoreGive(g_telemetry_mutex);
    }

    if (!target && !any_near) {
        cJSON *root = cJSON_CreateObject();
        if (!root) return;
        cJSON_AddStringToObject(root, "event", "bus_clear");
        cJSON_AddBoolToObject(root, "ld2412_target", false);
        cJSON_AddArrayToObject(root, "beacons_near");
        mqtt_publish_event_json(root);
        cJSON_Delete(root);
    }
}

static void enter_state(bus_state_t state)
{
    bus_state_t old_state;
    portENTER_CRITICAL(&s_lock);
    old_state = s_state;
    if (old_state == state) {
        portEXIT_CRITICAL(&s_lock);
        return;
    }

    s_state = state;
    if (state == BUS_STATE_ACTIVE || state == BUS_STATE_DISARMED) {
        s_distress_active = false;
        s_distress_acknowledged = false;
        s_distress_id[0] = '\0';
        g_bus_empty = false;
    } else if (state == BUS_STATE_ARMED || state == BUS_STATE_DISTRESS_PENDING ||
               state == BUS_STATE_DISTRESS_ACTIVE || state == BUS_STATE_ACKED) {
        g_bus_empty = true;
    }
    portEXIT_CRITICAL(&s_lock);

    ESP_LOGI(TAG, "State: %s -> %s", bus_state_name(old_state), bus_state_name(state));
    set_telemetry_state_locked();
    bus_state_publish();

    if (state == BUS_STATE_ACTIVE) {
        mqtt_clear_distress();
        publish_simple_event("ignition_on");
    } else if (state == BUS_STATE_DISARMED) {
        mqtt_clear_distress();
    } else if (state == BUS_STATE_EXIT_GRACE) {
        publish_simple_event("ignition_off");
    } else if (state == BUS_STATE_ARMED) {
        publish_simple_event("armed");
        beacon_config_queue_near_distress_events();
        publish_bus_clear_if_clear();
    }
}

static void enter_armed(void)
{
    uint16_t co2 = 0;
    bool co2_fresh = false;
    int64_t now = esp_timer_get_time();
    if (xSemaphoreTake(g_telemetry_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        int64_t age_us = g_telemetry.co2_last_update_us > 0
            ? (now - g_telemetry.co2_last_update_us) : -1;
        co2_fresh = g_telemetry.co2 > 0 &&
                    age_us >= 0 &&
                    age_us <= (int64_t)CO2_BASELINE_STALE_S * 1000000LL &&
                    g_telemetry.co2_status == SENSOR_STATUS_OK;
        co2 = co2_fresh ? g_telemetry.co2 : 0;
        g_telemetry.co2_baseline = co2;
        g_telemetry.co2_delta_since_armed = 0;
        xSemaphoreGive(g_telemetry_mutex);
    }
    if (!co2_fresh) {
        ESP_LOGW(TAG, "Entering armed without fresh CO2 baseline; tracker will baseline on next reading");
    }

    portENTER_CRITICAL(&s_lock);
    s_armed_generation++;
    s_co2_baseline = co2;
    portEXIT_CRITICAL(&s_lock);

    enter_state(BUS_STATE_ARMED);
}

bool bus_state_mark_distress_pending(void)
{
    portENTER_CRITICAL(&s_lock);
    if (s_state != BUS_STATE_ARMED || s_ignition_on) {
        bus_state_t state = s_state;
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGI(TAG, "Ignoring pending distress transition from state=%s",
                 bus_state_name(state));
        return false;
    }
    s_state = BUS_STATE_DISTRESS_PENDING;
    g_bus_empty = true;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "State: armed -> distress_pending");
    set_telemetry_state_locked();
    bus_state_publish();
    return true;
}

bool bus_state_mark_distress_active(void)
{
    portENTER_CRITICAL(&s_lock);
    if (s_state != BUS_STATE_DISTRESS_PENDING || s_ignition_on) {
        bus_state_t state = s_state;
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGI(TAG, "Ignoring active distress transition from state=%s",
                 bus_state_name(state));
        return false;
    }
    s_state = BUS_STATE_DISTRESS_ACTIVE;
    s_distress_active = true;
    s_distress_acknowledged = false;
    if (s_distress_id[0] == '\0') generate_distress_id_locked();
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "State: distress_pending -> distress_active");
    set_telemetry_state_locked();
    bus_state_publish();
    return true;
}

void bus_state_ack_distress(void)
{
    bool was_pending = false;
    bool was_active = false;
    portENTER_CRITICAL(&s_lock);
    if (s_state == BUS_STATE_DISTRESS_PENDING) {
        was_pending = true;
    } else if (s_distress_active) {
        was_active = true;
        s_distress_acknowledged = true;
        s_state = BUS_STATE_ACKED;
    }
    portEXIT_CRITICAL(&s_lock);

    if (was_pending) {
        /* Ack during pending window: cancel the alarm before broadcast.
         * notification_handle_ack() resets the tracker and emits distress_clear,
         * which clears retained MQTT distress and the relay's active id. */
        ESP_LOGW(TAG, "Distress acked during PENDING window: cancelling alarm");
        notification_handle_ack();
        return;
    }
    if (!was_active) {
        ESP_LOGI(TAG, "distress_ack received but no distress active");
        return;
    }

    set_telemetry_state_locked();
    bus_state_publish();
    publish_simple_event("distress_ack");
}

void bus_state_publish_distress_repeat(void)
{
    /* Re-publish the current distress snapshot. The notification task owns
     * the event; this delegates to notification_republish_distress which is
     * safe to call from any task. */
    notification_republish_distress();
}

void bus_state_clear_distress(void)
{
    bool ignition = false;
    bus_state_t old_state;
    bool should_transition = false;

    portENTER_CRITICAL(&s_lock);
    old_state = s_state;
    ignition = s_ignition_on;
    should_transition = old_state == BUS_STATE_DISTRESS_PENDING ||
                        old_state == BUS_STATE_DISTRESS_ACTIVE ||
                        old_state == BUS_STATE_ACKED;
    s_distress_active = false;
    s_distress_acknowledged = false;
    s_distress_id[0] = '\0';
    portEXIT_CRITICAL(&s_lock);

    mqtt_clear_distress();
    if (should_transition) {
        if (ignition) {
            enter_state(BUS_STATE_ACTIVE);
        } else {
            enter_state(BUS_STATE_ARMED);
        }
    } else {
        ESP_LOGI(TAG, "distress_clear received while state=%s; cleared retained distress only",
                 bus_state_name(old_state));
        set_telemetry_state_locked();
        bus_state_publish();
    }
    publish_simple_event("distress_clear");
}

const char *bus_state_disarm_result_name(bus_disarm_result_t result)
{
    switch (result) {
    case BUS_DISARM_OK: return "disarmed";
    case BUS_DISARM_ALREADY: return "already_disarmed";
    case BUS_DISARM_REJECT_PENDING: return "distress_pending_ack_required";
    case BUS_DISARM_REJECT_UNACKNOWLEDGED: return "distress_ack_required";
    case BUS_DISARM_REJECT_STATE: return "state_not_disarmable";
    default: return "unknown";
    }
}

bus_disarm_result_t bus_state_disarm(void)
{
    bus_state_t old_state;
    portENTER_CRITICAL(&s_lock);
    old_state = s_state;
    if (old_state == BUS_STATE_DISARMED || s_ignition_on) {
        portEXIT_CRITICAL(&s_lock);
        return BUS_DISARM_ALREADY;
    }
    if (old_state == BUS_STATE_DISTRESS_PENDING) {
        portEXIT_CRITICAL(&s_lock);
        return BUS_DISARM_REJECT_PENDING;
    }
    if (old_state == BUS_STATE_DISTRESS_ACTIVE ||
        (s_distress_active && !s_distress_acknowledged)) {
        portEXIT_CRITICAL(&s_lock);
        return BUS_DISARM_REJECT_UNACKNOWLEDGED;
    }
    if (old_state != BUS_STATE_EXIT_GRACE && old_state != BUS_STATE_ARMED &&
        old_state != BUS_STATE_ACKED && old_state != BUS_STATE_ACTIVE) {
        portEXIT_CRITICAL(&s_lock);
        return BUS_DISARM_REJECT_STATE;
    }

    /* State + physical ignition are checked and updated under one lock. If an
     * ignition edge wins the race, this returns ALREADY and ACTIVE remains
     * authoritative. If disarm wins, the GPIO task's subsequent edge moves
     * the system back to ACTIVE. */
    s_state = BUS_STATE_DISARMED;
    s_distress_active = false;
    s_distress_acknowledged = false;
    s_distress_id[0] = '\0';
    g_bus_empty = false;
    portEXIT_CRITICAL(&s_lock);

    ESP_LOGI(TAG, "State: %s -> disarmed", bus_state_name(old_state));
    set_telemetry_state_locked();
    bus_state_publish();
    mqtt_clear_distress();
    publish_simple_event("disarmed");
    ESP_LOGW(TAG, "Operator disarmed after cabin-clear confirmation");
    return BUS_DISARM_OK;
}

void bus_state_force_empty(bool empty)
{
    ESP_LOGW(TAG, "Manual bus_empty override: %d", empty);
    /* Snapshot the current source value so the poll loop's edge detection
     * starts from here: the override persists until the source's debounced
     * value actually transitions from this snapshot, not merely differs from
     * the override value. */
    bool raw_now = ignition_get();
    portENTER_CRITICAL(&s_lock);
    s_override_active = true;
    s_override_value = !empty;  /* empty=true -> override ignition OFF */
    s_last_raw = raw_now;
    s_prev_effective = !empty;  /* keep poll loop from re-transitioning */
    s_ignition_on = !empty;
    if (empty) {
        s_exit_grace_started = esp_timer_get_time() - (EXIT_GRACE_S * 1000000LL);
    }
    portEXIT_CRITICAL(&s_lock);

    if (empty) {
        enter_armed();
    } else {
        enter_state(BUS_STATE_ACTIVE);
    }
}

bool bus_state_override_active(void)
{
    bool active;
    portENTER_CRITICAL(&s_lock);
    active = s_override_active;
    portEXIT_CRITICAL(&s_lock);
    return active;
}

void bus_state_publish(void)
{
    mqtt_publish_bus_state();
}

void bus_state_task(void *pvParameters)
{
    /* Wait for the ignition source to be ready (ESPNOW waits for WiFi inside
     * ignition_init; GPIO returns immediately). */
    ignition_init();

    bool stable = ignition_get();
    s_last_raw = stable;
    s_prev_effective = stable;
    int64_t fault_check_at = 0;

    portENTER_CRITICAL(&s_lock);
    s_ignition_on = stable;
    if (stable) {
        s_state = BUS_STATE_ACTIVE;
        g_bus_empty = false;
    } else {
        s_state = BUS_STATE_EXIT_GRACE;
        s_exit_grace_started = esp_timer_get_time();
    }
    portEXIT_CRITICAL(&s_lock);

    set_telemetry_state_locked();
    bus_state_publish();
    ESP_LOGI(TAG, "Init ignition_on=%d state=%s (source=%s)",
             stable, bus_state_name(bus_state_get()), ignition_source_name());

    while (1) {
        int64_t now = esp_timer_get_time();

        /* Source-agnostic ignition polling. The override (bus_empty_on/off)
         * shadows the real value until cleared by a real source edge. */
        bool raw = ignition_get();
        bool effective;

        portENTER_CRITICAL(&s_lock);
        bool override_active = s_override_active;
        bool override_value = s_override_value;
        bool last_raw = s_last_raw;
        portEXIT_CRITICAL(&s_lock);

        if (override_active) {
            effective = override_value;
            /* Clear on a REAL edge: the source's debounced value changed since
             * the override was installed. Comparing raw != last_raw detects a
             * transition across polls, whereas raw != override_value would
             * instantly clear an override set while the source disagreed. */
            if (raw != last_raw) {
                ESP_LOGI(TAG, "Override cleared by real source edge (raw=%d last_raw=%d)",
                         raw, last_raw);
                portENTER_CRITICAL(&s_lock);
                s_override_active = false;
                portEXIT_CRITICAL(&s_lock);
                effective = raw;
            }
        } else {
            effective = raw;
        }

        /* Update s_last_raw for next iteration's edge detection. */
        portENTER_CRITICAL(&s_lock);
        s_last_raw = raw;
        portEXIT_CRITICAL(&s_lock);

        if (effective != s_prev_effective) {
            portENTER_CRITICAL(&s_lock);
            s_ignition_on = effective;
            portEXIT_CRITICAL(&s_lock);

            if (effective) {
                enter_state(BUS_STATE_ACTIVE);
            } else {
                portENTER_CRITICAL(&s_lock);
                s_exit_grace_started = now;
                portEXIT_CRITICAL(&s_lock);
                enter_state(BUS_STATE_EXIT_GRACE);
            }
            s_prev_effective = effective;
        } else {
            /* Keep s_ignition_on in sync even when unchanged (covers the gap
             * when override took over from a fresh source value). */
            portENTER_CRITICAL(&s_lock);
            s_ignition_on = effective;
            portEXIT_CRITICAL(&s_lock);
        }

        bus_state_t state = bus_state_get();
        if (state == BUS_STATE_EXIT_GRACE) {
            int64_t started;
            portENTER_CRITICAL(&s_lock);
            started = s_exit_grace_started;
            portEXIT_CRITICAL(&s_lock);
            if (started > 0 && (now - started) >= EXIT_GRACE_S * 1000000LL) {
                enter_armed();
            }
        }

        /* Sensor-fault watchdog: while armed, if a critical-sensor status
         * (LD2412 no_uart/error, CO2 error) persists for >120s, emit a
         * bus/event so the relay can warn. Distress detection stays active via
         * the remaining healthy signals (a dead radar doesn't suppress a
         * beacon_near trigger). */
        if (bus_state_is_armed() && now - fault_check_at > 30000000LL) {
            fault_check_at = now;
            bool critical_fault = false;
            const char *fault_sensor = NULL;
            const char *fault_detail = NULL;
            if (xSemaphoreTake(g_telemetry_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                if (g_telemetry.ld2412_status == SENSOR_STATUS_NO_UART ||
                    g_telemetry.ld2412_status == SENSOR_STATUS_ERROR) {
                    critical_fault = true;
                    fault_sensor = "ld2412";
                    fault_detail = g_telemetry.ld2412_status == SENSOR_STATUS_NO_UART ? "no_uart" : "error";
                }
                if (g_telemetry.co2_status == SENSOR_STATUS_ERROR) {
                    critical_fault = true;
                    if (!fault_sensor) {
                        fault_sensor = "scd4x";
                        fault_detail = "error";
                    }
                }
                xSemaphoreGive(g_telemetry_mutex);
            }
            if (critical_fault && state == BUS_STATE_ARMED) {
                if (s_fault_started_at == 0) {
                    s_fault_started_at = now;
                    s_fault_reported = false;
                    ESP_LOGE(TAG, "Critical sensor fault started while armed: %s/%s",
                             fault_sensor ? fault_sensor : "unknown",
                             fault_detail ? fault_detail : "unknown");
                } else {
                    int duration_s = (int)((now - s_fault_started_at) / 1000000LL);
                    if (!s_fault_reported && duration_s >= 120) {
                        ESP_LOGE(TAG, "Critical sensor fault persisted %ds while armed: %s/%s",
                                 duration_s,
                                 fault_sensor ? fault_sensor : "unknown",
                                 fault_detail ? fault_detail : "unknown");
                        publish_sensor_fault_event(fault_sensor, fault_detail, duration_s);
                        s_fault_reported = true;
                    }
                }
                /* Don't actually transition state to FAULT yet; FAULT would
                 * stop the armed-path checks in the sensor tasks (they gate
                 * on is_armed() which includes ARMED/PENDING/ACTIVE/ACKED
                 * but not FAULT). Emit the event + log only for now so
                 * detection continues on the surviving signals. */
            } else {
                s_fault_started_at = 0;
                s_fault_reported = false;
            }
        } else if (!bus_state_is_armed()) {
            s_fault_started_at = 0;
            s_fault_reported = false;
        }

        set_telemetry_state_locked();
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}
