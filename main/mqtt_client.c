#include <string.h>
#include <time.h>
#include <math.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <mqtt_client.h>
#include "esp_ota_ops.h"
#include "cJSON.h"
#include "common.h"
#include "bus_mqtt.h"
#include "bus_state.h"
#include "beacon_config.h"
#include "secrets.h"
#include "ignition.h"

static const char *TAG = "MQTT";

static esp_mqtt_client_handle_t s_client = NULL;

/* Publish helper that logs failures. For safety-critical topics (distress)
 * the return code is critical: a silently-dropped emergency publish is a
 * child-safety incident. Returns the message id (>0) on success. */
int mqtt_publish_checked(const char *topic, const char *data, int qos, bool retain)
{
    if (!s_client || !(xEventGroupGetBits(g_event_group) & MQTT_CONNECTED_BIT)) return -1;
    int msg_id = esp_mqtt_client_publish(s_client, topic, data, 0, qos, retain ? 1 : 0);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "publish FAILED topic=%s qos=%d retain=%d (len=%d)",
                 topic, qos, retain ? 1 : 0, data ? (int)strlen(data) : 0);
    }
    return msg_id;
}

void mqtt_publish_clear_retained(const char *topic)
{
    mqtt_publish_checked(topic, "", 1, true);
}

static void utc_timestamp(char *out, size_t out_len)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm_buf;
    gmtime_r(&tv.tv_sec, &tm_buf);
    strftime(out, out_len, "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
}

static void beacon_name_to_safe(const char *name, char *out, size_t out_len)
{
    strncpy(out, name, out_len - 1);
    out[out_len - 1] = '\0';
    for (int i = 0; out[i]; i++) {
        if (out[i] == '/' || out[i] == '+' || out[i] == '#') {
            out[i] = '_';
        }
    }
}

static void publish_sensor_discovery_on_topic(const char *name, const char *unique_id,
                                              const char *state_topic,
                                              const char *device_class, const char *unit,
                                              const char *value_template)
{
    char topic[256];
    snprintf(topic, sizeof(topic), "%s/sensor/%s/config", HA_DISCOVERY_PREFIX, unique_id);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", name);
    /* object_id makes entity_id deterministic: `sensor.<unique_id>`. Without it
     * HA derives entity_id from the name slug, which varies across HA versions
     * and makes dashboard references unreliable. */
    cJSON_AddStringToObject(root, "obj_id", unique_id);
    cJSON_AddStringToObject(root, "stat_t", state_topic);
    cJSON_AddStringToObject(root, "val_tpl", value_template);
    if (device_class) cJSON_AddStringToObject(root, "dev_cla", device_class);
    if (unit) cJSON_AddStringToObject(root, "unit_of_meas", unit);
    cJSON_AddStringToObject(root, "stat_cla", "measurement");
    cJSON_AddStringToObject(root, "uniq_id", unique_id);
    cJSON_AddStringToObject(root, "avty_t", MQTT_BASE_TOPIC "/status");
    cJSON_AddStringToObject(root, "pl_avail", "online");
    cJSON_AddStringToObject(root, "pl_not_avail", "offline");

    cJSON *dev = cJSON_AddObjectToObject(root, "dev");
    cJSON *ids = cJSON_AddArrayToObject(dev, "ids");
    cJSON_AddItemToArray(ids, cJSON_CreateString(DEVICE_ID));
    cJSON_AddStringToObject(dev, "name", DEVICE_NAME);
    cJSON_AddStringToObject(dev, "mf", "Van Child Safety");
    cJSON_AddStringToObject(dev, "mdl", "BusSensor v2");

    char *json = cJSON_PrintUnformatted(root);
    mqtt_publish_checked(topic, json, 1, true);
    free(json);
    cJSON_Delete(root);
}

static void publish_sensor_discovery(const char *name, const char *unique_id,
                                     const char *device_class, const char *unit,
                                     const char *value_template)
{
    publish_sensor_discovery_on_topic(name, unique_id, MQTT_BASE_TOPIC "/telemetry",
                                      device_class, unit, value_template);
}

static void publish_binary_sensor_discovery(const char *name, const char *unique_id,
                                            const char *device_class,
                                            const char *value_template)
{
    char topic[256];
    snprintf(topic, sizeof(topic), "%s/binary_sensor/%s/config", HA_DISCOVERY_PREFIX, unique_id);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", name);
    cJSON_AddStringToObject(root, "obj_id", unique_id);
    cJSON_AddStringToObject(root, "stat_t", MQTT_BASE_TOPIC "/telemetry");
    cJSON_AddStringToObject(root, "val_tpl", value_template);
    if (device_class) cJSON_AddStringToObject(root, "dev_cla", device_class);
    cJSON_AddStringToObject(root, "pl_on", "true");
    cJSON_AddStringToObject(root, "pl_off", "false");
    cJSON_AddStringToObject(root, "uniq_id", unique_id);
    cJSON_AddStringToObject(root, "avty_t", MQTT_BASE_TOPIC "/status");
    cJSON_AddStringToObject(root, "pl_avail", "online");
    cJSON_AddStringToObject(root, "pl_not_avail", "offline");

    cJSON *dev = cJSON_AddObjectToObject(root, "dev");
    cJSON *ids = cJSON_AddArrayToObject(dev, "ids");
    cJSON_AddItemToArray(ids, cJSON_CreateString(DEVICE_ID));

    char *json = cJSON_PrintUnformatted(root);
    mqtt_publish_checked(topic, json, 1, true);
    free(json);
    cJSON_Delete(root);
}

static void publish_text_sensor_discovery(const char *name, const char *unique_id,
                                          const char *state_topic,
                                          const char *value_template)
{
    char topic[256];
    snprintf(topic, sizeof(topic), "%s/sensor/%s/config", HA_DISCOVERY_PREFIX, unique_id);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", name);
    cJSON_AddStringToObject(root, "obj_id", unique_id);
    cJSON_AddStringToObject(root, "stat_t", state_topic);
    if (value_template) cJSON_AddStringToObject(root, "val_tpl", value_template);
    cJSON_AddStringToObject(root, "uniq_id", unique_id);
    cJSON_AddStringToObject(root, "avty_t", MQTT_BASE_TOPIC "/status");
    cJSON_AddStringToObject(root, "pl_avail", "online");
    cJSON_AddStringToObject(root, "pl_not_avail", "offline");
    cJSON *dev = cJSON_AddObjectToObject(root, "dev");
    cJSON *ids = cJSON_AddArrayToObject(dev, "ids");
    cJSON_AddItemToArray(ids, cJSON_CreateString(DEVICE_ID));

    char *json = cJSON_PrintUnformatted(root);
    mqtt_publish_checked(topic, json, 1, true);
    free(json);
    cJSON_Delete(root);
}

static void publish_device_tracker_discovery(void)
{
    char topic[256];
    snprintf(topic, sizeof(topic), "%s/device_tracker/%s_location/config",
             HA_DISCOVERY_PREFIX, DEVICE_ID);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", "Bus Location");
    cJSON_AddStringToObject(root, "obj_id", DEVICE_ID "_location");
    cJSON_AddStringToObject(root, "stat_t", MQTT_BASE_TOPIC "/location/state");
    cJSON_AddStringToObject(root, "state_topic", MQTT_BASE_TOPIC "/location/state");
    cJSON_AddStringToObject(root, "json_attr_t", MQTT_BASE_TOPIC "/location/attributes");
    cJSON_AddStringToObject(root, "json_attributes_topic", MQTT_BASE_TOPIC "/location/attributes");
    cJSON_AddStringToObject(root, "src_type", "gps");
    cJSON_AddStringToObject(root, "source_type", "gps");
    cJSON_AddStringToObject(root, "uniq_id", DEVICE_ID "_location");
    cJSON_AddStringToObject(root, "avty_t", MQTT_BASE_TOPIC "/status");
    cJSON_AddStringToObject(root, "pl_avail", "online");
    cJSON_AddStringToObject(root, "pl_not_avail", "offline");
    cJSON *dev = cJSON_AddObjectToObject(root, "dev");
    cJSON *ids = cJSON_AddArrayToObject(dev, "ids");
    cJSON_AddItemToArray(ids, cJSON_CreateString(DEVICE_ID));

    char *json = cJSON_PrintUnformatted(root);
    mqtt_publish_checked(topic, json, 1, true);
    free(json);
    cJSON_Delete(root);
}

static void publish_button_discovery(const char *name, const char *unique_id,
                                     const char *payload_press)
{
    char topic[256];
    snprintf(topic, sizeof(topic), "%s/button/%s/config", HA_DISCOVERY_PREFIX, unique_id);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", name);
    cJSON_AddStringToObject(root, "obj_id", unique_id);
    cJSON_AddStringToObject(root, "cmd_t", MQTT_BASE_TOPIC "/command");
    cJSON_AddStringToObject(root, "pl_prs", payload_press);
    cJSON_AddStringToObject(root, "uniq_id", unique_id);
    cJSON_AddStringToObject(root, "avty_t", MQTT_BASE_TOPIC "/status");
    cJSON_AddStringToObject(root, "pl_avail", "online");
    cJSON_AddStringToObject(root, "pl_not_avail", "offline");

    cJSON *dev = cJSON_AddObjectToObject(root, "dev");
    cJSON *ids = cJSON_AddArrayToObject(dev, "ids");
    cJSON_AddItemToArray(ids, cJSON_CreateString(DEVICE_ID));

    char *json = cJSON_PrintUnformatted(root);
    mqtt_publish_checked(topic, json, 1, true);
    free(json);
    cJSON_Delete(root);
}

static void publish_switch_discovery(const char *name, const char *unique_id,
                                     const char *value_template,
                                     const char *payload_on, const char *payload_off)
{
    char topic[256];
    snprintf(topic, sizeof(topic), "%s/switch/%s/config", HA_DISCOVERY_PREFIX, unique_id);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", name);
    cJSON_AddStringToObject(root, "obj_id", unique_id);
    cJSON_AddStringToObject(root, "stat_t", MQTT_BASE_TOPIC "/telemetry");
    cJSON_AddStringToObject(root, "val_tpl", value_template);
    cJSON_AddStringToObject(root, "cmd_t", MQTT_BASE_TOPIC "/command");
    cJSON_AddStringToObject(root, "pl_on", payload_on);
    cJSON_AddStringToObject(root, "pl_off", payload_off);
    cJSON_AddStringToObject(root, "stat_on", "true");
    cJSON_AddStringToObject(root, "stat_off", "false");
    cJSON_AddStringToObject(root, "uniq_id", unique_id);
    cJSON_AddStringToObject(root, "avty_t", MQTT_BASE_TOPIC "/status");
    cJSON_AddStringToObject(root, "pl_avail", "online");
    cJSON_AddStringToObject(root, "pl_not_avail", "offline");

    cJSON *dev = cJSON_AddObjectToObject(root, "dev");
    cJSON *ids = cJSON_AddArrayToObject(dev, "ids");
    cJSON_AddItemToArray(ids, cJSON_CreateString(DEVICE_ID));

    char *json = cJSON_PrintUnformatted(root);
    mqtt_publish_checked(topic, json, 1, true);
    free(json);
    cJSON_Delete(root);
}

static void publish_beacon_discovery_for(const beacon_entry_t *entry)
{
    if (!entry || !entry->active) return;

    char safe[32];
    beacon_name_to_safe(entry->name, safe, sizeof(safe));

    char state_topic[160];
    snprintf(state_topic, sizeof(state_topic), MQTT_BASE_TOPIC "/beacon/%s/state", safe);
    char uid_base[80];
    snprintf(uid_base, sizeof(uid_base), DEVICE_ID "_beacon_%s", safe);

    /* Proximity state sensor (near / far / absent) */
    {
        char topic[256];
        snprintf(topic, sizeof(topic), "%s/sensor/%s_state/config", HA_DISCOVERY_PREFIX, uid_base);
        cJSON *root = cJSON_CreateObject();
        char disp_name[64];
        snprintf(disp_name, sizeof(disp_name), "Beacon: %s",
                 entry->person_name[0] ? entry->person_name : entry->name);
        cJSON_AddStringToObject(root, "name", disp_name);
        /* obj_id = `<DEVICE_ID>_beacon_<id>_state` so entity_id is deterministic
         * on every HA version. Without obj_id HA derives entity_id from the
         * name slug, which would mangle Unicode/whitespace in person_name. */
        char obj_id[96];
        snprintf(obj_id, sizeof(obj_id), "%s_state", uid_base);
        cJSON_AddStringToObject(root, "obj_id", obj_id);
        cJSON_AddStringToObject(root, "stat_t", state_topic);
        cJSON_AddStringToObject(root, "val_tpl", "{{ value_json.state }}");
        cJSON_AddStringToObject(root, "uniq_id", uid_base);
        cJSON_AddStringToObject(root, "avty_t", MQTT_BASE_TOPIC "/status");
        cJSON_AddStringToObject(root, "pl_avail", "online");
        cJSON_AddStringToObject(root, "pl_not_avail", "offline");
        cJSON *dev = cJSON_AddObjectToObject(root, "dev");
        cJSON *ids = cJSON_AddArrayToObject(dev, "ids");
        cJSON_AddItemToArray(ids, cJSON_CreateString(DEVICE_ID));
        char *json = cJSON_PrintUnformatted(root);
        mqtt_publish_checked(topic, json, 1, true);
        free(json);
        cJSON_Delete(root);
    }

    /* RSSI sensor (signal strength, dBm) */
    {
        char topic[256], uid[96], obj_id[96];
        snprintf(topic, sizeof(topic), "%s/sensor/%s_rssi/config", HA_DISCOVERY_PREFIX, uid_base);
        snprintf(uid, sizeof(uid), "%s_rssi", uid_base);
        snprintf(obj_id, sizeof(obj_id), "%s_rssi", uid_base);
        cJSON *root = cJSON_CreateObject();
        char disp_name[64];
        snprintf(disp_name, sizeof(disp_name), "Beacon: %s RSSI",
                 entry->person_name[0] ? entry->person_name : entry->name);
        cJSON_AddStringToObject(root, "name", disp_name);
        cJSON_AddStringToObject(root, "obj_id", obj_id);
        cJSON_AddStringToObject(root, "stat_t", state_topic);
        cJSON_AddStringToObject(root, "val_tpl", "{{ value_json.rssi }}");
        cJSON_AddStringToObject(root, "dev_cla", "signal_strength");
        cJSON_AddStringToObject(root, "unit_of_meas", "dBm");
        cJSON_AddStringToObject(root, "stat_cla", "measurement");
        cJSON_AddStringToObject(root, "uniq_id", uid);
        cJSON_AddStringToObject(root, "avty_t", MQTT_BASE_TOPIC "/status");
        cJSON_AddStringToObject(root, "pl_avail", "online");
        cJSON_AddStringToObject(root, "pl_not_avail", "offline");
        cJSON *dev = cJSON_AddObjectToObject(root, "dev");
        cJSON *ids = cJSON_AddArrayToObject(dev, "ids");
        cJSON_AddItemToArray(ids, cJSON_CreateString(DEVICE_ID));
        char *json = cJSON_PrintUnformatted(root);
        mqtt_publish_checked(topic, json, 1, true);
        free(json);
        cJSON_Delete(root);
    }
}

static void publish_all_discovery(void)
{
    publish_sensor_discovery("Temperature", DEVICE_ID "_temperature", "temperature", "\u00b0C",
                             "{{ value_json.temperature }}");
    publish_sensor_discovery("Humidity", DEVICE_ID "_humidity", "humidity", "%",
                             "{{ value_json.humidity }}");
    publish_sensor_discovery("CO2", DEVICE_ID "_co2", "carbon_dioxide", "ppm",
                             "{{ value_json.co2 }}");
    publish_sensor_discovery("Moving Distance", DEVICE_ID "_moving_distance", NULL, "cm",
                             "{{ value_json.moving_distance }}");
    publish_sensor_discovery("Still Distance", DEVICE_ID "_still_distance", NULL, "cm",
                             "{{ value_json.still_distance }}");
    publish_sensor_discovery("Move Energy", DEVICE_ID "_moving_energy", NULL, NULL,
                             "{{ value_json.moving_energy }}");
    publish_sensor_discovery("Still Energy", DEVICE_ID "_still_energy", NULL, NULL,
                             "{{ value_json.still_energy }}");
    publish_sensor_discovery("Detection Distance", DEVICE_ID "_detection_distance", NULL, "cm",
                             "{{ value_json.detection_distance }}");
publish_sensor_discovery("Light", DEVICE_ID "_ld2412_light", NULL, "raw",
                              "{{ value_json.ld2412_light }}");
    publish_sensor_discovery("Latitude", DEVICE_ID "_latitude", NULL, "\u00b0",
                             "{{ value_json.latitude }}");
    publish_sensor_discovery("Longitude", DEVICE_ID "_longitude", NULL, "\u00b0",
                             "{{ value_json.longitude }}");
    publish_sensor_discovery("Course", DEVICE_ID "_course", NULL, "\u00b0",
                             "{{ value_json.course }}");
    publish_sensor_discovery("Altitude", DEVICE_ID "_altitude", NULL, "m",
                             "{{ value_json.altitude }}");
    publish_sensor_discovery("Speed", DEVICE_ID "_speed", NULL, "km/h",
                             "{{ value_json.speed }}");
    publish_sensor_discovery("Satellites", DEVICE_ID "_satellites", NULL, NULL,
                             "{{ value_json.satellites }}");
    publish_sensor_discovery("CO2 Delta 3min", DEVICE_ID "_co2_delta_3min", NULL, "ppm",
                             "{{ value_json.co2_delta_3min }}");
    publish_sensor_discovery("CO2 Delta Since Armed", DEVICE_ID "_co2_delta_since_armed", NULL, "ppm",
                             "{{ value_json.co2_delta_since_armed }}");
    publish_sensor_discovery("CO2 Armed Baseline", DEVICE_ID "_co2_baseline", "carbon_dioxide", "ppm",
                             "{{ value_json.co2_baseline }}");
    publish_sensor_discovery("WiFi RSSI", DEVICE_ID "_wifi_rssi", "signal_strength", "dBm",
                             "{{ value_json.wifi_rssi }}");

    publish_binary_sensor_discovery("Presence", DEVICE_ID "_ld2412_target", "occupancy",
                                    "{{ value_json.ld2412_target | lower }}");
    publish_binary_sensor_discovery("Moving Target", DEVICE_ID "_ld2412_moving", NULL,
                                    "{{ value_json.ld2412_moving | lower }}");
    publish_binary_sensor_discovery("Still Target", DEVICE_ID "_ld2412_still", NULL,
                                    "{{ value_json.ld2412_still | lower }}");
    publish_binary_sensor_discovery("GPS Valid", DEVICE_ID "_gps_valid", NULL,
                                    "{{ value_json.gps_valid | lower }}");
    publish_binary_sensor_discovery("Ignition", DEVICE_ID "_ignition_on", NULL,
                                    "{{ value_json.ignition_on | lower }}");
    publish_binary_sensor_discovery("Armed", DEVICE_ID "_bus_armed", NULL,
                                    "{{ value_json.bus_armed | lower }}");

    /* Ignition source health + ESPNOW remote diagnostics. These read from
     * bus/state (retained) so they appear as soon as the firmware publishes. */
    publish_text_sensor_discovery("Ignition Source", DEVICE_ID "_ignition_source",
                                  MQTT_BASE_TOPIC "/state",
                                  "{{ value_json.ignition_source }}");
    publish_text_sensor_discovery("Ignition Source Status", DEVICE_ID "_ignition_source_status",
                                  MQTT_BASE_TOPIC "/state",
                                  "{{ value_json.ignition_source_status }}");
    publish_sensor_discovery_on_topic("Ignition Remote Heartbeat Age",
                                      DEVICE_ID "_ignition_remote_heartbeat_age_s",
                                      MQTT_BASE_TOPIC "/state",
                                      "duration", "s",
                                      "{{ value_json.ignition_espnow.last_heartbeat_age_s }}");
    publish_sensor_discovery_on_topic("Ignition Remote Battery",
                                      DEVICE_ID "_ignition_remote_battery_mv",
                                      MQTT_BASE_TOPIC "/state",
                                      "voltage", "mV",
                                      "{{ value_json.ignition_espnow.remote_battery_mv }}");

    publish_text_sensor_discovery("Bus State", DEVICE_ID "_bus_state",
                                  MQTT_BASE_TOPIC "/state", "{{ value_json.state }}");
    publish_text_sensor_discovery("Distress", DEVICE_ID "_distress",
                                  MQTT_BASE_TOPIC "/distress/state",
                                  "{{ value_json.state if value_json is defined and value_json.state is defined else 'clear' }}");
    publish_text_sensor_discovery("Beacon Config Result", DEVICE_ID "_beacon_config_result",
                                  MQTT_BASE_TOPIC "/beacon/config/result", "{{ value_json.message | default(value) }}");

    publish_button_discovery("Restart", DEVICE_ID "_restart", "restart");
    publish_button_discovery("Acknowledge Distress", DEVICE_ID "_distress_ack", "distress_ack");
    publish_button_discovery("Clear Distress", DEVICE_ID "_distress_clear", "distress_clear");
    publish_button_discovery("Test Distress", DEVICE_ID "_distress_test", "distress_test");

    publish_switch_discovery("Bus Empty", DEVICE_ID "_bus_empty",
                             "{{ value_json.bus_empty | lower }}",
                             "bus_empty_on", "bus_empty_off");

    publish_device_tracker_discovery();

    int beacon_count = 0;
    beacon_config_lock();
    for (int i = 0; i < beacon_config_count(); i++) {
        beacon_entry_t *entry = beacon_config_get(i);
        if (entry && entry->active) {
            publish_beacon_discovery_for(entry);
            beacon_count++;
        }
    }
    beacon_config_unlock();

    /* Base discovery count: 17 sensors + 6 binary sensors + 3 text sensors +
     * 4 buttons + 1 switch + 1 device tracker = 32 base, + 2 text sensors
     * (ignition source, ignition source status) + 2 sensors (remote heartbeat
     * age, remote battery) = 36 base. Each active beacon adds 2. */
    int total = 36 + beacon_count * 2;
    ESP_LOGI(TAG, "Published %d discovery messages (%d beacons x2), retained", total, beacon_count);
}

static void publish_telemetry(void)
{
    if (xSemaphoreTake(g_telemetry_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;

    float lat = g_telemetry.latitude;
    float lon = g_telemetry.longitude;
    float speed = g_telemetry.speed;
    float course = g_telemetry.course;
    bool gps_valid = g_telemetry.gps_valid;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "temperature", round(g_telemetry.temperature * 100.0) / 100.0);
    cJSON_AddNumberToObject(root, "humidity", round(g_telemetry.humidity * 10.0) / 10.0);
    cJSON_AddNumberToObject(root, "co2", g_telemetry.co2);
    cJSON_AddNumberToObject(root, "co2_baseline", g_telemetry.co2_baseline);
    cJSON_AddNumberToObject(root, "co2_delta_since_armed", g_telemetry.co2_delta_since_armed);
    cJSON_AddBoolToObject(root, "ld2412_target", g_telemetry.ld2412_target);
    cJSON_AddBoolToObject(root, "ld2412_moving", g_telemetry.ld2412_moving);
    cJSON_AddBoolToObject(root, "ld2412_still", g_telemetry.ld2412_still);
    cJSON_AddNumberToObject(root, "moving_distance", g_telemetry.moving_distance);
    cJSON_AddNumberToObject(root, "still_distance", g_telemetry.still_distance);
    cJSON_AddNumberToObject(root, "moving_energy", g_telemetry.moving_energy);
    cJSON_AddNumberToObject(root, "still_energy", g_telemetry.still_energy);
    cJSON_AddNumberToObject(root, "detection_distance", g_telemetry.detection_distance);
    cJSON_AddNumberToObject(root, "ld2412_light", g_telemetry.ld2412_light);
    cJSON_AddNumberToObject(root, "latitude", g_telemetry.latitude);
    cJSON_AddNumberToObject(root, "longitude", g_telemetry.longitude);
    cJSON_AddNumberToObject(root, "course", g_telemetry.course);
    cJSON_AddNumberToObject(root, "altitude", g_telemetry.altitude);
    cJSON_AddNumberToObject(root, "speed", g_telemetry.speed);
    cJSON_AddNumberToObject(root, "satellites", g_telemetry.satellites);
    cJSON_AddBoolToObject(root, "gps_valid", g_telemetry.gps_valid);
    cJSON_AddNumberToObject(root, "co2_delta_3min", g_telemetry.co2_delta_3min);
    cJSON_AddNumberToObject(root, "wifi_rssi", g_telemetry.wifi_rssi);
    cJSON_AddBoolToObject(root, "bus_empty", g_bus_empty);
    cJSON_AddBoolToObject(root, "ignition_on", g_telemetry.ignition_on);
    cJSON_AddBoolToObject(root, "bus_armed", g_telemetry.bus_armed);
    cJSON_AddStringToObject(root, "bus_state", g_telemetry.bus_state);
    cJSON_AddNumberToObject(root, "ble_scan_drops", g_telemetry.ble_scan_drop_count);

    beacon_config_add_telemetry(root);

    xSemaphoreGive(g_telemetry_mutex);

    char *json = cJSON_PrintUnformatted(root);
    mqtt_publish_checked(MQTT_BASE_TOPIC "/telemetry", json, 1, false);
    free(json);
    cJSON_Delete(root);

    cJSON *attrs = cJSON_CreateObject();
    if (gps_valid) {
        mqtt_publish_checked(MQTT_BASE_TOPIC "/location/state", "gps", 1, true);
        cJSON_AddNumberToObject(attrs, "latitude", lat);
        cJSON_AddNumberToObject(attrs, "longitude", lon);
        cJSON_AddNumberToObject(attrs, "speed", speed);
        cJSON_AddNumberToObject(attrs, "course", course);
        cJSON_AddBoolToObject(attrs, "gps_valid", true);
    } else {
        mqtt_publish_checked(MQTT_BASE_TOPIC "/location/state", "not_home", 1, true);
        cJSON_AddBoolToObject(attrs, "gps_valid", false);
    }
    char *attr_json = cJSON_PrintUnformatted(attrs);
    if (attr_json) {
        mqtt_publish_checked(MQTT_BASE_TOPIC "/location/attributes", attr_json, 1, true);
        free(attr_json);
    }
    cJSON_Delete(attrs);
}

static void handle_command(const char *data, int data_len)
{
    /* Trim leading/trailing whitespace/newlines. HA sometimes appends a
     * trailing newline; exact-length memcmp would silently drop the command. */
    while (data_len > 0 && (data[0] == ' ' || data[0] == '\t' ||
                            data[0] == '\r' || data[0] == '\n')) {
        data++;
        data_len--;
    }
    while (data_len > 0 && (data[data_len - 1] == ' ' || data[data_len - 1] == '\t' ||
                            data[data_len - 1] == '\r' || data[data_len - 1] == '\n')) {
        data_len--;
    }

    /* Structured commands carry correlation and confirmation data. Safety-
     * sensitive actions intentionally do not accept a bare text alias. */
    if (data_len > 0 && data[0] == '{') {
        cJSON *root = cJSON_ParseWithLength(data, data_len);
        const cJSON *request = root ? cJSON_GetObjectItemCaseSensitive(root, "request_id") : NULL;
        const cJSON *action = root ? cJSON_GetObjectItemCaseSensitive(root, "action") : NULL;
        const cJSON *confirm = root ? cJSON_GetObjectItemCaseSensitive(root, "confirm_cabin_clear") : NULL;
        const cJSON *source = root ? cJSON_GetObjectItemCaseSensitive(root, "source") : NULL;
        const char *request_id = cJSON_IsString(request) && request->valuestring
                               ? request->valuestring : "unknown";
        const char *action_name = cJSON_IsString(action) && action->valuestring
                                ? action->valuestring : "unknown";
        const char *source_name = cJSON_IsString(source) && source->valuestring
                                ? source->valuestring : "unknown";

        if (!root) {
            mqtt_publish_command_result("unknown", "unknown", false,
                                        "invalid_json", "unknown");
        } else if (!cJSON_IsString(request) || request_id[0] == '\0') {
            mqtt_publish_command_result("unknown", action_name, false,
                                        "request_id_required", source_name);
        } else if (!cJSON_IsString(source) || source_name[0] == '\0') {
            mqtt_publish_command_result(request_id, action_name, false,
                                        "source_required", "unknown");
        } else if (strlen(request_id) > 63 || strlen(action_name) > 31 ||
                   strlen(source_name) > 63) {
            mqtt_publish_command_result("unknown", action_name, false,
                                        "metadata_too_long", "unknown");
        } else if (strcmp(action_name, "disarm") != 0) {
            mqtt_publish_command_result(request_id, action_name, false,
                                        "unsupported_action", source_name);
        } else if (!cJSON_IsTrue(confirm)) {
            mqtt_publish_command_result(request_id, action_name, false,
                                        "cabin_clear_confirmation_required", source_name);
        } else {
            bus_disarm_result_t result = bus_state_disarm();
            bool ok = result == BUS_DISARM_OK || result == BUS_DISARM_ALREADY;
            mqtt_publish_command_result(request_id, action_name, ok,
                                        bus_state_disarm_result_name(result), source_name);
        }
        cJSON_Delete(root);
        return;
    }

    if (data_len == 6 && memcmp(data, "disarm", 6) == 0) {
        mqtt_publish_command_result("unknown", "disarm", false,
                                    "structured_confirmation_required", "legacy");
    } else if (data_len == 7 && memcmp(data, "restart", 7) == 0) {
        ESP_LOGI(TAG, "Restart command");
        esp_restart();
    } else if (data_len == 12 && memcmp(data, "bus_empty_on", 12) == 0) {
        bus_state_force_empty(true);
    } else if (data_len == 13 && memcmp(data, "bus_empty_off", 13) == 0) {
        bus_state_force_empty(false);
    } else if (data_len == 12 && memcmp(data, "distress_ack", 12) == 0) {
        bus_state_ack_distress();
    } else if (data_len == 14 && memcmp(data, "distress_clear", 14) == 0) {
        bus_state_clear_distress();
    } else if (data_len == 13 && memcmp(data, "distress_test", 13) == 0) {
        distress_event_t evt = {
            .type = DISTRESS_PRESENCE_DETECTED,
            .timestamp = esp_timer_get_time(),
        };
        if (xQueueSend(g_distress_queue, &evt, 0) != pdTRUE) {
            ESP_LOGW(TAG, "distress_test: queue full");
        }
    } else if (data_len > 0) {
        ESP_LOGW(TAG, "Unrecognized bus/command payload: %.*s", data_len, data);
    }
}

static void mqtt_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch (event->event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Connected");
        xEventGroupSetBits(g_event_group, MQTT_CONNECTED_BIT);
        /* Only mark the OTA slot valid when we actually booted from one.
         * Calling this from the factory partition logs "Running firmware is
         * factory" and, worse, internally runs a SHA over the whole partition
         * which overflows the MQTT task's stack and panics. The factory
         * partition needs no rollback confirmation. */
        const esp_partition_t *running = esp_ota_get_running_partition();
        if (running && running->type == ESP_PARTITION_TYPE_APP &&
            running->subtype != ESP_PARTITION_SUBTYPE_APP_FACTORY) {
            esp_ota_mark_app_valid_cancel_rollback();
        }
        esp_mqtt_client_publish(s_client, MQTT_BASE_TOPIC "/status", "online", 0, 1, true);
        esp_mqtt_client_subscribe(s_client, MQTT_BASE_TOPIC "/command", 1);
        esp_mqtt_client_subscribe(s_client, MQTT_BASE_TOPIC "/beacon/config/set", 1);
        publish_all_discovery();
        bus_state_publish();
        mqtt_publish_beacon_states();
        beacon_config_publish_list();
        /* Reconcile retained distress on every reconnect. If firmware has an
         * active/acked incident, re-publish it; otherwise clear any stale
         * retained emergency left by a previous boot/crash. Without the clear
         * path, Home Assistant can show sensor.bus_distress=active while
         * bus/state is freshly active with distress_active=false. */
        if (bus_state_distress_active()) {
            bus_state_publish_distress_repeat();
        } else {
            mqtt_clear_distress();
        }
        break;

    case MQTT_EVENT_DATA: {
        char topic[256];
        int tlen = event->topic_len < 255 ? event->topic_len : 255;
        memcpy(topic, event->topic, tlen);
        topic[tlen] = '\0';

        if (strcmp(topic, MQTT_BASE_TOPIC "/command") == 0) {
            if (event->retain) {
                ESP_LOGW(TAG, "Ignoring retained command payload on %s", topic);
                break;
            }
            handle_command(event->data, event->data_len);
        } else if (strcmp(topic, MQTT_BASE_TOPIC "/beacon/config/set") == 0) {
            if (event->retain) {
                ESP_LOGW(TAG, "Ignoring retained beacon config command on %s", topic);
                break;
            }
            beacon_config_handle_mqtt(event->data, event->data_len);
        } else {
            ESP_LOGI(TAG, "RX topic=%.*s data=%.*s", tlen, topic,
                     event->data_len < 64 ? event->data_len : 64,
                     (const char *)event->data);
        }
        break;
    }

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Disconnected");
        xEventGroupClearBits(g_event_group, MQTT_CONNECTED_BIT);
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "Error");
        break;

    default:
        break;
    }
}

void mqtt_task(void *pvParameters)
{
    /* Wait for WireGuard before bringing up MQTT (broker is on the WG net). */
    xEventGroupWaitBits(g_event_group, WG_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    /* Wait briefly for NTP so initial publishes carry real timestamps
     * (not 1970-01-01). Don't block forever: if NTP is busted we still want
     * bus/status and bus/state to publish; the relay treats malformed
     * timestamps as opaque strings in dedupe keys. */
    xEventGroupWaitBits(g_event_group, NTP_SYNCED_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(15000));

    ESP_LOGI(TAG, "Initializing MQTT (ntp_synced=%d)",
             !!(xEventGroupGetBits(g_event_group) & NTP_SYNCED_BIT));

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
        .credentials.username = MQTT_USERNAME,
        .credentials.authentication.password = MQTT_PASSWORD,
        .session.last_will = {
            .topic = MQTT_BASE_TOPIC "/status",
            .msg = "offline",
            .msg_len = 7,
            .qos = 1,
            .retain = 1,
        },
    };

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!s_client) {
        ESP_LOGE(TAG, "Failed to initialize MQTT client");
        vTaskDelete(NULL);
        return;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_client);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        if (xEventGroupGetBits(g_event_group) & MQTT_CONNECTED_BIT) {
            publish_telemetry();
        }
    }
}

void mqtt_publish_beacon_state(const char *name, const char *state, int rssi)
{
    if (!s_client || !(xEventGroupGetBits(g_event_group) & MQTT_CONNECTED_BIT)) return;

    char safe_name[32];
    beacon_name_to_safe(name, safe_name, sizeof(safe_name));

    char topic[256];
    snprintf(topic, sizeof(topic), MQTT_BASE_TOPIC "/beacon/%s/state", safe_name);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "id", name);
    cJSON_AddStringToObject(root, "name", name);
    cJSON_AddStringToObject(root, "state", state);
    cJSON_AddNumberToObject(root, "rssi", rssi);
    char timebuf[32];
    utc_timestamp(timebuf, sizeof(timebuf));
    cJSON_AddStringToObject(root, "timestamp", timebuf);

    /* Use a snapshot under the beacon mutex instead of holding a raw pointer.
     * A concurrent remove_beacon would memmove the table and invalidate a
     * raw pointer; this was a real use-after-free read during distress
     * publishes. The snapshot also lets us read last_seen safely. */
    beacon_entry_t snapshot;
    if (beacon_config_snapshot_by_id(name, &snapshot)) {
        cJSON_AddStringToObject(root, "person_name", snapshot.person_name);
        cJSON_AddNumberToObject(root, "rssi_threshold", snapshot.rssi_threshold);
        cJSON_AddNumberToObject(root, "effective_rssi_threshold",
                                beacon_config_effective_rssi_threshold(&snapshot));
        cJSON_AddBoolToObject(root, "active", snapshot.active);
        cJSON_AddStringToObject(root, "discord_channel_id", snapshot.discord_channel_id);
        cJSON_AddStringToObject(root, "home_zone_entity", snapshot.home_zone_entity);
        int64_t age_s = snapshot.last_seen > 0 ? (esp_timer_get_time() - snapshot.last_seen) / 1000000 : -1;
        cJSON_AddNumberToObject(root, "last_seen_age_s", age_s);
    }

    char *json = cJSON_PrintUnformatted(root);
    mqtt_publish_checked(topic, json, 1, true);
    free(json);
    cJSON_Delete(root);
}

void mqtt_publish_beacon_discovery(const char *name)
{
    if (!s_client || !(xEventGroupGetBits(g_event_group) & MQTT_CONNECTED_BIT)) return;

    beacon_config_lock();
    for (int i = 0; i < beacon_config_count(); i++) {
        beacon_entry_t *entry = beacon_config_get(i);
        if (entry && entry->active && strcmp(entry->name, name) == 0) {
            publish_beacon_discovery_for(entry);
            /* publish current state so HA sees it immediately */
            const char *st = entry->state == BEACON_NEAR ? "near"
                           : entry->state == BEACON_FAR ? "far" : "absent";
            mqtt_publish_beacon_state(entry->name, st, entry->rssi);
            break;
        }
    }
    beacon_config_unlock();
}

void mqtt_publish_beacon_states(void)
{
    if (!s_client || !(xEventGroupGetBits(g_event_group) & MQTT_CONNECTED_BIT)) return;

    beacon_config_lock();
    for (int i = 0; i < beacon_config_count(); i++) {
        beacon_entry_t *entry = beacon_config_get(i);
        if (!entry || !entry->active) continue;
        const char *st = entry->state == BEACON_NEAR ? "near"
                       : entry->state == BEACON_FAR ? "far" : "absent";
        mqtt_publish_beacon_state(entry->name, st, entry->rssi);
    }
    beacon_config_unlock();
}

void mqtt_publish_distress(const distress_event_t *event)
{
    if (!s_client || !(xEventGroupGetBits(g_event_group) & MQTT_CONNECTED_BIT)) return;

    cJSON *root = cJSON_CreateObject();
    char timebuf[32];
    utc_timestamp(timebuf, sizeof(timebuf));

    const char *stable_id = bus_state_distress_id();
    if (!stable_id || !stable_id[0]) {
        /* Distress id must be stable across re-publishes so the relay can
         * dedupe. mark_distress_active() always populates the id before
         * publishing; reaching here means a logic error. Fail closed: publish
         * with a fixed sentinel rather than a per-call timestamp that would
         * break relay dedupe. */
        ESP_LOGE(TAG, "mqtt_publish_distress: no stable distress id set");
        stable_id = "distress-uninitialized";
    }
    cJSON_AddStringToObject(root, "id", stable_id);
    cJSON_AddStringToObject(root, "severity", "emergency");
    cJSON_AddStringToObject(root, "state", "active");
    cJSON_AddStringToObject(root, "bus_state", bus_state_name(bus_state_get()));

    beacon_entry_t beacon_snapshot;
    bool have_beacon = false;
    switch (event->type) {
    case DISTRESS_BEACON_NEAR:
        cJSON_AddStringToObject(root, "trigger", "beacon_near");
        cJSON_AddStringToObject(root, "detail", event->beacon_name);
        /* Snapshot the beacon under the lock; do NOT hold a raw pointer across
         * the cJSON tree build (a concurrent remove could invalidate it). */
        have_beacon = beacon_config_snapshot_by_id(event->beacon_name, &beacon_snapshot);
        if (have_beacon) {
            cJSON *person = cJSON_AddObjectToObject(root, "person");
            cJSON_AddStringToObject(person, "id", beacon_snapshot.name);
            cJSON_AddStringToObject(person, "person_name", beacon_snapshot.person_name);
            cJSON_AddStringToObject(person, "discord_channel_id", beacon_snapshot.discord_channel_id);
            cJSON_AddStringToObject(person, "home_zone_entity", beacon_snapshot.home_zone_entity);
        }
        break;
    case DISTRESS_CO2_RISE:
        cJSON_AddStringToObject(root, "trigger", "co2_rise");
        cJSON_AddStringToObject(root, "detail", "co2_rise");
        cJSON_AddNumberToObject(root, "co2_delta", event->co2_delta);
        break;
    case DISTRESS_PRESENCE_DETECTED:
        cJSON_AddStringToObject(root, "trigger", "presence_detected");
        cJSON_AddStringToObject(root, "detail", "presence_detected");
        break;
    }

    cJSON_AddStringToObject(root, "timestamp", timebuf);

    if (xSemaphoreTake(g_telemetry_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        int64_t now = esp_timer_get_time();
        cJSON *evidence = cJSON_AddObjectToObject(root, "evidence");
        cJSON_AddNumberToObject(evidence, "co2", g_telemetry.co2);
        cJSON_AddNumberToObject(evidence, "co2_baseline", g_telemetry.co2_baseline);
        cJSON_AddNumberToObject(evidence, "co2_delta_since_armed", g_telemetry.co2_delta_since_armed);
        cJSON_AddNumberToObject(evidence, "co2_consecutive_rise_count", g_telemetry.co2_consecutive_rise_count);
        cJSON_AddBoolToObject(evidence, "ld2412_target", g_telemetry.ld2412_target);
        cJSON_AddBoolToObject(evidence, "ld2412_moving", g_telemetry.ld2412_moving);
        cJSON_AddBoolToObject(evidence, "ld2412_still", g_telemetry.ld2412_still);
        cJSON_AddNumberToObject(evidence, "moving_distance", g_telemetry.moving_distance);
        cJSON_AddNumberToObject(evidence, "still_distance", g_telemetry.still_distance);
        cJSON_AddNumberToObject(evidence, "moving_energy", g_telemetry.moving_energy);
        cJSON_AddNumberToObject(evidence, "still_energy", g_telemetry.still_energy);
        if (g_telemetry.ld2412_target_since_us > 0) {
            cJSON_AddNumberToObject(evidence, "ld2412_target_duration_s",
                                    (double)(now - g_telemetry.ld2412_target_since_us) / 1000000.0);
        } else {
            cJSON_AddNumberToObject(evidence, "ld2412_target_duration_s", 0);
        }
        if (g_telemetry.ld2412_last_frame_us > 0) {
            cJSON_AddNumberToObject(evidence, "ld2412_frame_age_s",
                                    (double)(now - g_telemetry.ld2412_last_frame_us) / 1000000.0);
        } else {
            cJSON_AddNumberToObject(evidence, "ld2412_frame_age_s", -1);
        }

        if (have_beacon) {
            const char *bs = beacon_snapshot.state == BEACON_NEAR ? "near"
                           : beacon_snapshot.state == BEACON_FAR ? "far" : "absent";
            cJSON_AddStringToObject(evidence, "beacon_state", bs);
            cJSON_AddNumberToObject(evidence, "rssi", beacon_snapshot.rssi);
        }

        cJSON *gps = cJSON_AddObjectToObject(root, "gps");
        cJSON_AddBoolToObject(gps, "valid", g_telemetry.gps_valid);
        cJSON_AddNumberToObject(gps, "latitude", g_telemetry.latitude);
        cJSON_AddNumberToObject(gps, "longitude", g_telemetry.longitude);
        xSemaphoreGive(g_telemetry_mutex);
    }

    char *json = cJSON_PrintUnformatted(root);
    if (json) {
        int msg_id = mqtt_publish_checked(MQTT_BASE_TOPIC "/distress/state", json, 2, true);
        if (msg_id < 0) {
            ESP_LOGE(TAG, "EMERGENCY distress publish failed (topic=%s len=%d)",
                     MQTT_BASE_TOPIC "/distress/state", (int)strlen(json));
        } else {
            ESP_LOGW(TAG, "Distress published (trigger=%d msg_id=%d)", event->type, msg_id);
        }
        free(json);
    }
    cJSON_Delete(root);
}

void mqtt_clear_distress(void)
{
    if (!s_client || !(xEventGroupGetBits(g_event_group) & MQTT_CONNECTED_BIT)) return;
    mqtt_publish_checked(MQTT_BASE_TOPIC "/distress/state", "", 1, true);
}

void mqtt_publish_command_result(const char *request_id, const char *action,
                                 bool ok, const char *reason, const char *source)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return;
    cJSON_AddStringToObject(root, "request_id", request_id ? request_id : "unknown");
    cJSON_AddStringToObject(root, "action", action ? action : "unknown");
    cJSON_AddBoolToObject(root, "ok", ok);
    cJSON_AddStringToObject(root, "status", ok ? "ok" : "rejected");
    cJSON_AddStringToObject(root, "reason", reason ? reason : "unknown");
    cJSON_AddStringToObject(root, "source", source ? source : "unknown");
    cJSON_AddStringToObject(root, "state", bus_state_name(bus_state_get()));
    cJSON_AddBoolToObject(root, "ignition_on", bus_state_ignition_on());

    char *json = cJSON_PrintUnformatted(root);
    if (json) {
        mqtt_publish_checked(MQTT_BASE_TOPIC "/command/result", json, 1, false);
        free(json);
    }
    cJSON_Delete(root);
}

void mqtt_publish_bus_state(void)
{
    if (!s_client || !(xEventGroupGetBits(g_event_group) & MQTT_CONNECTED_BIT)) return;

    cJSON *root = cJSON_CreateObject();
    bus_state_t state = bus_state_get();
    cJSON_AddStringToObject(root, "state", bus_state_name(state));
    cJSON_AddBoolToObject(root, "ignition_on", bus_state_ignition_on());
    cJSON_AddBoolToObject(root, "armed", bus_state_is_armed());
    cJSON_AddNumberToObject(root, "exit_grace_remaining_s", bus_state_exit_grace_remaining_s());
    cJSON_AddBoolToObject(root, "distress_active", bus_state_distress_active());
    cJSON_AddBoolToObject(root, "distress_acknowledged", bus_state_distress_acknowledged());

    /* Per-sensor status resolution:
     *  - SCD4x: ERROR if read_measurement kept failing, else STALE if no
     *    valid reading in 90s, else OK.
     *  - LD2412: NO_UART if zero bytes since boot, else STALE if no frames
     *    in 90s, else OK.
     *  - BLE: STALE if no scan results in 90s, else OK.
     * A STALE flag here is informational; a persistent ERROR/NO_UART is a
     * safety concern because the firmware can lose one of its three child-
     * detection signals. The bus_state_task transitions to FAULT on sustained
     * critical sensor loss. */
    const char *co2_status_str = "ok";
    const char *ld2412_status_str = "ok";
    const char *ble_status_str = "ok";
    if (xSemaphoreTake(g_telemetry_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        int64_t now = esp_timer_get_time();
        /* CO2: prefer the explicit sensor_status, fall back to stale check. */
        sensor_status_t co2_st = g_telemetry.co2_status;
        if (co2_st == SENSOR_STATUS_OK) {
            int64_t age = g_telemetry.co2_last_update_us > 0
                ? (now - g_telemetry.co2_last_update_us) : -1;
            co2_status_str = (age < 0 || age > 90LL * 1000000LL) ? "stale" : "ok";
        } else if (co2_st == SENSOR_STATUS_ERROR) {
            co2_status_str = "error";
        } else {
            co2_status_str = "stale";
        }
        /* LD2412: explicit no_uart, then stale check. */
        sensor_status_t ld_st = g_telemetry.ld2412_status;
        if (ld_st == SENSOR_STATUS_NO_UART) {
            ld2412_status_str = "no_uart";
        } else if (ld_st == SENSOR_STATUS_ERROR) {
            ld2412_status_str = "error";
        } else {
            int64_t age = g_telemetry.ld2412_last_frame_us > 0
                ? (now - g_telemetry.ld2412_last_frame_us) : -1;
            ld2412_status_str = (age < 0 || age > 90LL * 1000000LL) ? "stale" : "ok";
        }
        /* BLE. */
        sensor_status_t ble_st = g_telemetry.ble_status;
        if (ble_st == SENSOR_STATUS_STALE) {
            ble_status_str = "stale";
        } else if (ble_st == SENSOR_STATUS_ERROR) {
            ble_status_str = "error";
        } else {
            int64_t age = g_telemetry.ble_last_scan_us > 0
                ? (now - g_telemetry.ble_last_scan_us) : -1;
            ble_status_str = (age < 0 || age > 90LL * 1000000LL) ? "stale" : "ok";
        }
        cJSON_AddBoolToObject(root, "gps_valid", g_telemetry.gps_valid);
        cJSON_AddNumberToObject(root, "ble_scan_drops", g_telemetry.ble_scan_drop_count);
        xSemaphoreGive(g_telemetry_mutex);
    }
    cJSON_AddStringToObject(root, "co2_status", co2_status_str);
    cJSON_AddStringToObject(root, "ld2412_status", ld2412_status_str);
    cJSON_AddStringToObject(root, "ble_status", ble_status_str);

    /* Ignition source diagnostics. ignition_source_name() and
     * ignition_source_status() are source-agnostic. When a bus_empty_on/off
     * override is active (bus_state.c layer), the source's own status is
     * shadowed; report "overridden" so operators can see the manual override
     * in HA rather than a misleading OK/STALE from the underlying source. */
    ignition_source_status_t ign_status;
    if (bus_state_override_active()) {
        ign_status = IGNITION_SOURCE_OVERRIDDEN;
    } else {
        ign_status = ignition_source_status();
    }
    cJSON_AddStringToObject(root, "ignition_source", ignition_source_name());
    cJSON_AddStringToObject(root, "ignition_source_status",
                             ignition_source_status_name(ign_status));

    ignition_espnow_diag_t diag;
    if (ignition_espnow_diag(&diag)) {
        cJSON *espnow = cJSON_AddObjectToObject(root, "ignition_espnow");
        cJSON_AddNumberToObject(espnow, "last_heartbeat_age_s",
                                diag.last_heartbeat_age_us >= 0
                                    ? (double)diag.last_heartbeat_age_us / 1000000.0
                                    : -1);
        cJSON_AddNumberToObject(espnow, "remote_uptime_s", diag.remote_uptime_s);
        cJSON_AddNumberToObject(espnow, "remote_battery_mv", diag.remote_battery_mv);
        cJSON_AddNumberToObject(espnow, "seq", diag.seq);
        cJSON_AddNumberToObject(espnow, "missed_seq", diag.missed_seq);
    }

    char timebuf[32];
    utc_timestamp(timebuf, sizeof(timebuf));
    cJSON_AddStringToObject(root, "timestamp", timebuf);

    char *json = cJSON_PrintUnformatted(root);
    mqtt_publish_checked(MQTT_BASE_TOPIC "/state", json, 1, true);
    free(json);
    cJSON_Delete(root);
}

void mqtt_publish_event_json(cJSON *root)
{
    if (!s_client || !(xEventGroupGetBits(g_event_group) & MQTT_CONNECTED_BIT) || !root) return;
    if (!cJSON_GetObjectItem(root, "timestamp")) {
        char timebuf[32];
        utc_timestamp(timebuf, sizeof(timebuf));
        cJSON_AddStringToObject(root, "timestamp", timebuf);
    }
    char *json = cJSON_PrintUnformatted(root);
    if (json) {
        mqtt_publish_checked(MQTT_BASE_TOPIC "/event", json, 1, false);
        free(json);
    }
}

esp_mqtt_client_handle_t mqtt_get_client(void)
{
    return s_client;
}
