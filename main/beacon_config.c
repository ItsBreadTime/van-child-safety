#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <sys/time.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "cJSON.h"
#include "common.h"
#include "beacon_config.h"
#include "bus_state.h"
#include "bus_mqtt.h"
#include "secrets.h"

static const char *TAG = "BCN_CFG";

typedef struct {
    char name[32];
    beacon_type_t type;
    uint8_t mac[6];
    uint8_t uuid[16];
    uint16_t major;
    uint16_t minor;
    int8_t rssi_threshold;
    beacon_state_t state;
    int rssi;
    int64_t last_seen;
    int64_t near_since;
    bool active;
} legacy_beacon_entry_t;

static beacon_entry_t s_beacons[MAX_BEACONS];
static int s_count = 0;
static SemaphoreHandle_t s_mutex = NULL;

static void load_from_nvs(void);

static int hex_char(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool valid_id(const char *id)
{
    if (!id || id[0] == '\0') return false;
    size_t len = strlen(id);
    if (len > 31) return false;
    for (size_t i = 0; i < len; i++) {
        if (!(isalnum((unsigned char)id[i]) || id[i] == '_' || id[i] == '-')) {
            return false;
        }
    }
    return true;
}

static bool valid_discord_channel(const char *channel)
{
    if (!channel || channel[0] == '\0') return true;
    size_t len = strlen(channel);
    if (len > 32) return false;
    for (size_t i = 0; i < len; i++) {
        if (!isdigit((unsigned char)channel[i])) return false;
    }
    return true;
}

static bool parse_mac(const char *str, uint8_t *mac)
{
    if (!str || strlen(str) != 17) return false;
    for (int i = 0; i < 6; i++) {
        int hi = hex_char(str[i * 3]);
        int lo = hex_char(str[i * 3 + 1]);
        if (hi < 0 || lo < 0) return false;
        mac[i] = (hi << 4) | lo;
        if (i < 5 && str[i * 3 + 2] != ':') return false;
    }
    return true;
}

static bool parse_uuid(const char *str, uint8_t *uuid)
{
    if (!str || strlen(str) != 36) return false;
    if (str[8] != '-' || str[13] != '-' || str[18] != '-' || str[23] != '-') return false;

    int byte_idx = 0;
    for (int i = 0; i < 36 && byte_idx < 16; i++) {
        if (str[i] == '-') continue;
        if (i + 1 >= 36 || str[i + 1] == '-') return false;
        int hi = hex_char(str[i]);
        int lo = hex_char(str[i + 1]);
        if (hi < 0 || lo < 0) return false;
        uuid[byte_idx++] = (hi << 4) | lo;
        i++;
    }
    return byte_idx == 16;
}

static void format_uuid(const uint8_t *uuid, char *out)
{
    snprintf(out, 37, "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
             uuid[0], uuid[1], uuid[2], uuid[3], uuid[4], uuid[5],
             uuid[6], uuid[7], uuid[8], uuid[9], uuid[10], uuid[11],
             uuid[12], uuid[13], uuid[14], uuid[15]);
}

static void copy_string(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0) return;
    if (!src) src = "";
    strncpy(dst, src, dst_len - 1);
    dst[dst_len - 1] = '\0';
}

static const char *json_string_or_null(const cJSON *json, const char *key)
{
    const cJSON *item = cJSON_GetObjectItem(json, key);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static const cJSON *json_object_or_null(const cJSON *json, const char *key)
{
    const cJSON *item = cJSON_GetObjectItem(json, key);
    return cJSON_IsObject(item) ? item : NULL;
}

static int json_int_or_default(const cJSON *json, const char *key, int fallback)
{
    const cJSON *item = cJSON_GetObjectItem(json, key);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

static const char *state_str(beacon_state_t state)
{
    return state == BEACON_NEAR ? "near" : state == BEACON_FAR ? "far" : "absent";
}

int beacon_config_effective_rssi_threshold(const beacon_entry_t *entry)
{
    if (!entry) return CONFIG_BLE_BEACON_MIN_NEAR_RSSI;
    int threshold = entry->rssi_threshold;
    if (threshold < CONFIG_BLE_BEACON_MIN_NEAR_RSSI) {
        threshold = CONFIG_BLE_BEACON_MIN_NEAR_RSSI;
    }
    return threshold;
}

static void bc_utc_timestamp(char *out, size_t out_len)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm_buf;
    gmtime_r(&tv.tv_sec, &tm_buf);
    strftime(out, out_len, "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
}

beacon_entry_t *beacon_config_find_by_id(const char *id)
{
    if (!id) return NULL;
    for (int i = 0; i < s_count; i++) {
        if (strcasecmp(s_beacons[i].name, id) == 0) return &s_beacons[i];
    }
    return NULL;
}

bool beacon_config_snapshot(beacon_entry_t *entry, beacon_entry_t *out)
{
    if (!entry || !out) return false;
    memcpy(out, entry, sizeof(*out));
    return true;
}

bool beacon_config_snapshot_by_id(const char *id, beacon_entry_t *out)
{
    if (!id || !out) return false;
    bool found = false;
    beacon_config_lock();
    beacon_entry_t *entry = beacon_config_find_by_id(id);
    if (entry) {
        memcpy(out, entry, sizeof(*out));
        found = true;
    }
    beacon_config_unlock();
    return found;
}

static bool duplicate_ibeacon_identity(const uint8_t *uuid, uint16_t major, uint16_t minor,
                                       const char *except_id)
{
    for (int i = 0; i < s_count; i++) {
        if (except_id && strcasecmp(s_beacons[i].name, except_id) == 0) continue;
        if (!s_beacons[i].active || s_beacons[i].type != BEACON_TYPE_IBEACON) continue;
        if (memcmp(s_beacons[i].uuid, uuid, 16) == 0 &&
            s_beacons[i].major == major && s_beacons[i].minor == minor) {
            return true;
        }
    }
    return false;
}

static bool any_near_locked(void)
{
    for (int i = 0; i < s_count; i++) {
        if (s_beacons[i].active && s_beacons[i].state == BEACON_NEAR) {
            return true;
        }
    }
    return false;
}

static bool destructive_change_blocked_locked(const cJSON *json, const char *action,
                                              const char **error, const char **message)
{
    bool destructive = false;

    if (strcmp(action, "remove") == 0) {
        destructive = true;
    } else if (strcmp(action, "update") == 0) {
        const cJSON *active = cJSON_GetObjectItem(json, "active");
        destructive = active && cJSON_IsFalse(active);
    }

    if (!destructive) {
        return false;
    }
    if (bus_state_is_armed()) {
        *error = "bus_armed";
        *message = "Refusing destructive beacon change while bus monitoring is armed";
        return true;
    }
    if (any_near_locked()) {
        *error = "beacon_near";
        *message = "Refusing destructive beacon change while a beacon is near";
        return true;
    }
    return false;
}

static bool duplicate_loaded_id(const char *id, int loaded_count)
{
    for (int i = 0; i < loaded_count; i++) {
        if (strcasecmp(s_beacons[i].name, id) == 0) {
            return true;
        }
    }
    return false;
}

static bool duplicate_loaded_ibeacon_identity(const uint8_t *uuid, uint16_t major,
                                              uint16_t minor, int loaded_count)
{
    for (int i = 0; i < loaded_count; i++) {
        if (!s_beacons[i].active || s_beacons[i].type != BEACON_TYPE_IBEACON) continue;
        if (memcmp(s_beacons[i].uuid, uuid, 16) == 0 &&
            s_beacons[i].major == major && s_beacons[i].minor == minor) {
            return true;
        }
    }
    return false;
}

static void clear_beacon_retained_artifacts(const char *id)
{
    char topic[256];
    snprintf(topic, sizeof(topic), MQTT_BASE_TOPIC "/beacon/%s/state", id);
    mqtt_publish_clear_retained(topic);

    char uid_base[80];
    snprintf(uid_base, sizeof(uid_base), DEVICE_ID "_beacon_%s", id);
    snprintf(topic, sizeof(topic), "%s/sensor/%s_state/config", HA_DISCOVERY_PREFIX, uid_base);
    mqtt_publish_clear_retained(topic);
    snprintf(topic, sizeof(topic), "%s/sensor/%s_rssi/config", HA_DISCOVERY_PREFIX, uid_base);
    mqtt_publish_clear_retained(topic);
}

static void add_entry_json(cJSON *arr, const beacon_entry_t *entry)
{
    cJSON *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "id", entry->name);
    cJSON_AddStringToObject(b, "name", entry->name);
    cJSON_AddStringToObject(b, "person_name", entry->person_name);
    cJSON_AddBoolToObject(b, "active", entry->active);
    cJSON_AddStringToObject(b, "state", state_str(entry->state));
    cJSON_AddNumberToObject(b, "rssi", entry->rssi);
    cJSON_AddNumberToObject(b, "rssi_threshold", entry->rssi_threshold);
    cJSON_AddNumberToObject(b, "effective_rssi_threshold",
                            beacon_config_effective_rssi_threshold(entry));
    cJSON_AddStringToObject(b, "discord_channel_id", entry->discord_channel_id);
    cJSON_AddStringToObject(b, "home_zone_entity", entry->home_zone_entity);
    cJSON_AddStringToObject(b, "notes", entry->notes);

    cJSON *beacon = cJSON_AddObjectToObject(b, "beacon");
    cJSON_AddStringToObject(beacon, "type", entry->type == BEACON_TYPE_MAC ? "mac" : "ibeacon");
    if (entry->type == BEACON_TYPE_MAC) {
        char macbuf[18];
        snprintf(macbuf, sizeof(macbuf), "%02X:%02X:%02X:%02X:%02X:%02X",
                 entry->mac[0], entry->mac[1], entry->mac[2],
                 entry->mac[3], entry->mac[4], entry->mac[5]);
        cJSON_AddStringToObject(beacon, "mac", macbuf);
        cJSON_AddStringToObject(b, "type", "mac");
        cJSON_AddStringToObject(b, "mac", macbuf);
    } else {
        char uuidbuf[37];
        format_uuid(entry->uuid, uuidbuf);
        cJSON_AddStringToObject(beacon, "uuid", uuidbuf);
        cJSON_AddNumberToObject(beacon, "major", entry->major);
        cJSON_AddNumberToObject(beacon, "minor", entry->minor);
        cJSON_AddStringToObject(b, "type", "ibeacon");
        cJSON_AddStringToObject(b, "uuid", uuidbuf);
        cJSON_AddNumberToObject(b, "major", entry->major);
        cJSON_AddNumberToObject(b, "minor", entry->minor);
    }
    cJSON_AddNumberToObject(beacon, "rssi_threshold", entry->rssi_threshold);

    cJSON *discord = cJSON_AddObjectToObject(b, "discord");
    cJSON_AddStringToObject(discord, "channel_id", entry->discord_channel_id);

    cJSON *home = cJSON_AddObjectToObject(b, "home");
    cJSON_AddStringToObject(home, "zone_entity", entry->home_zone_entity);

    cJSON_AddItemToArray(arr, b);
}

static cJSON *build_roster_json(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "version", 1);
    cJSON_AddNumberToObject(root, "max_records", MAX_BEACONS);
    cJSON_AddNumberToObject(root, "count", s_count);
    char timebuf[32];
    bc_utc_timestamp(timebuf, sizeof(timebuf));
    cJSON_AddStringToObject(root, "timestamp", timebuf);
    cJSON *records = cJSON_AddArrayToObject(root, "records");
    cJSON *beacons = cJSON_AddArrayToObject(root, "beacons");
    for (int i = 0; i < s_count; i++) {
        add_entry_json(records, &s_beacons[i]);
        add_entry_json(beacons, &s_beacons[i]);
    }
    return root;
}

static void publish_result(const char *request_id, const char *action, bool ok,
                           const char *id, const char *error, const char *message,
                           bool used_alias)
{
    cJSON *root = cJSON_CreateObject();
    if (request_id && request_id[0]) cJSON_AddStringToObject(root, "request_id", request_id);
    if (action) cJSON_AddStringToObject(root, "action", action);
    cJSON_AddBoolToObject(root, "ok", ok);
    if (id && id[0]) cJSON_AddStringToObject(root, "id", id);
    if (error && error[0]) cJSON_AddStringToObject(root, "error", error);
    if (message && message[0]) cJSON_AddStringToObject(root, "message", message);
    if (used_alias) cJSON_AddStringToObject(root, "warning", "used_legacy_name_alias");

    char timebuf[32];
    bc_utc_timestamp(timebuf, sizeof(timebuf));
    cJSON_AddStringToObject(root, "timestamp", timebuf);

    char *json = cJSON_PrintUnformatted(root);
    if (json) {
        mqtt_publish_checked(MQTT_BASE_TOPIC "/beacon/config/result", json, 1, false);
        free(json);
    }
    cJSON_Delete(root);
}

static void load_from_json_string(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse roster JSON (corrupt); leaving roster empty");
        return;
    }
    int version = json_int_or_default(root, "version", 1);
    if (version != 1) {
        ESP_LOGW(TAG, "Roster JSON version=%d (expected 1); attempting v1 parse", version);
        /* No migration path yet. Fields json_string_or_null will return NULL
         * for any unknown future-schema keys, so we don't crash - we just
         * load the v1-shaped fields we recognize. */
    }
    cJSON *records = cJSON_GetObjectItem(root, "records");
    if (!cJSON_IsArray(records)) records = cJSON_GetObjectItem(root, "beacons");
    if (!cJSON_IsArray(records)) {
        ESP_LOGE(TAG, "Roster JSON has no records[]/beacons[] array; leaving roster empty");
        cJSON_Delete(root);
        return;
    }

    int count = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, records) {
        if (count >= MAX_BEACONS) break;
        const char *id = json_string_or_null(item, "id");
        if (!id) id = json_string_or_null(item, "name");
        if (!valid_id(id)) continue;
        if (duplicate_loaded_id(id, count)) {
            ESP_LOGW(TAG, "Skipping duplicate beacon id in NVS JSON: %s", id);
            continue;
        }

        beacon_entry_t *entry = &s_beacons[count];
        memset(entry, 0, sizeof(*entry));
        copy_string(entry->name, sizeof(entry->name), id);
        copy_string(entry->person_name, sizeof(entry->person_name),
                    json_string_or_null(item, "person_name"));
        if (entry->person_name[0] == '\0') copy_string(entry->person_name, sizeof(entry->person_name), id);

        const char *discord_id = json_string_or_null(item, "discord_channel_id");
        const cJSON *discord_obj = cJSON_GetObjectItem(item, "discord");
        if (!discord_id && discord_obj) discord_id = json_string_or_null(discord_obj, "channel_id");
        copy_string(entry->discord_channel_id, sizeof(entry->discord_channel_id), discord_id);

        const char *home_zone = json_string_or_null(item, "home_zone_entity");
        const cJSON *home_obj = cJSON_GetObjectItem(item, "home");
        if (!home_zone && home_obj) home_zone = json_string_or_null(home_obj, "zone_entity");
        copy_string(entry->home_zone_entity, sizeof(entry->home_zone_entity), home_zone);

        copy_string(entry->notes, sizeof(entry->notes), json_string_or_null(item, "notes"));
        entry->active = !cJSON_IsFalse(cJSON_GetObjectItem(item, "active"));
        entry->state = BEACON_ABSENT;
        /* Re-validate rssi_threshold on load: the int8 cast on add already
         * rejects out-of-range, but a load path trusts the stored JSON. A
         * value of 200 would wrap to -56 and produce wrong near/far
         * behavior. Clamp to the valid range instead of dropping the record. */
        int rssi_int = json_int_or_default(item, "rssi_threshold",
                                            json_int_or_default(item, "rssi", -65));
        if (rssi_int < -127) rssi_int = -127;
        if (rssi_int > 0) rssi_int = 0;
        entry->rssi_threshold = (int8_t)rssi_int;

        cJSON *beacon = cJSON_GetObjectItem(item, "beacon");
        const cJSON *src = cJSON_IsObject(beacon) ? beacon : item;
        const char *type = json_string_or_null(src, "type");
        if (type && strcmp(type, "mac") == 0) {
            const char *mac = json_string_or_null(src, "mac");
            if (!mac) mac = json_string_or_null(src, "value");
            if (!parse_mac(mac, entry->mac)) continue;
            entry->type = BEACON_TYPE_MAC;
        } else {
            const char *uuid = json_string_or_null(src, "uuid");
            if (!parse_uuid(uuid, entry->uuid)) continue;
            entry->type = BEACON_TYPE_IBEACON;
            int major = json_int_or_default(src, "major", 0);
            int minor = json_int_or_default(src, "minor", 0);
            if (major < 0 || major > 65535 || minor < 0 || minor > 65535) {
                ESP_LOGW(TAG, "Skipping beacon %s with invalid iBeacon major/minor", id);
                continue;
            }
            entry->major = (uint16_t)major;
            entry->minor = (uint16_t)minor;
            if (entry->active &&
                duplicate_loaded_ibeacon_identity(entry->uuid, entry->major, entry->minor, count)) {
                ESP_LOGW(TAG, "Skipping duplicate active iBeacon identity in NVS JSON: %s", id);
                continue;
            }
        }
        count++;
    }
    s_count = count;
    cJSON_Delete(root);
}

static void load_legacy_blob(nvs_handle_t handle)
{
    legacy_beacon_entry_t legacy[MAX_BEACONS];
    size_t size = sizeof(legacy);
    if (nvs_get_blob(handle, "list", legacy, &size) != ESP_OK) return;
    int count = size / sizeof(legacy_beacon_entry_t);
    if (count > MAX_BEACONS) count = MAX_BEACONS;

    for (int i = 0; i < count; i++) {
        beacon_entry_t *entry = &s_beacons[s_count];
        memset(entry, 0, sizeof(*entry));
        copy_string(entry->name, sizeof(entry->name), legacy[i].name);
        copy_string(entry->person_name, sizeof(entry->person_name), legacy[i].name);
        entry->type = legacy[i].type;
        memcpy(entry->mac, legacy[i].mac, sizeof(entry->mac));
        memcpy(entry->uuid, legacy[i].uuid, sizeof(entry->uuid));
        entry->major = legacy[i].major;
        entry->minor = legacy[i].minor;
        entry->rssi_threshold = legacy[i].rssi_threshold;
        entry->state = BEACON_ABSENT;
        entry->active = legacy[i].active;
        if (valid_id(entry->name)) s_count++;
    }
    if (s_count > 0) {
        ESP_LOGI(TAG, "Migrated %d legacy beacons from NVS blob", s_count);
        /* Only erase the legacy blob after a successful JSON save. If the
         * save fails (NVS full, commit error), erasing the legacy blob
         * would lose the roster permanently on next boot. */
        if (beacon_config_save()) {
            if (nvs_erase_key(handle, "list") == ESP_OK) {
                nvs_commit(handle);
                ESP_LOGI(TAG, "Erased legacy 'list' blob after migration");
            }
        } else {
            ESP_LOGE(TAG, "Legacy migration: JSON save failed; keeping 'list' blob as fallback");
        }
    }
}

static void load_from_nvs(void)
{
    nvs_handle_t handle;
    if (nvs_open("beacons", NVS_READONLY, &handle) != ESP_OK) return;

    size_t size = 0;
    if (nvs_get_str(handle, "json", NULL, &size) == ESP_OK && size > 0) {
        char *json = malloc(size);
        if (json && nvs_get_str(handle, "json", json, &size) == ESP_OK) {
            int count_before = s_count;
            load_from_json_string(json);
            if (s_count > 0 || count_before == s_count) {
                ESP_LOGI(TAG, "Loaded %d beacon records from JSON NVS", s_count);
            } else {
                /* load_from_json_string returned with s_count=0 - either the
                 * roster is legitimately empty or the JSON was corrupt. Try
                 * the legacy blob as a fallback so we don't lose data. */
                ESP_LOGW(TAG, "JSON roster parse yielded 0 records; trying legacy 'list' blob");
                load_legacy_blob(handle);
            }
        }
        free(json);
    } else {
        load_legacy_blob(handle);
    }
    nvs_close(handle);
}

void beacon_config_init(void)
{
    s_mutex = xSemaphoreCreateRecursiveMutex();
    memset(s_beacons, 0, sizeof(s_beacons));
    load_from_nvs();
}

void beacon_config_lock(void)
{
    if (s_mutex) xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
}

void beacon_config_unlock(void)
{
    if (s_mutex) xSemaphoreGiveRecursive(s_mutex);
}

int beacon_config_count(void)
{
    return s_count;
}

beacon_entry_t *beacon_config_get(int index)
{
    if (index < 0 || index >= s_count) return NULL;
    return &s_beacons[index];
}

beacon_entry_t *beacon_config_find_by_mac(const uint8_t *mac)
{
    for (int i = 0; i < s_count; i++) {
        if (s_beacons[i].active && s_beacons[i].type == BEACON_TYPE_MAC &&
            memcmp(s_beacons[i].mac, mac, 6) == 0) {
            return &s_beacons[i];
        }
    }
    return NULL;
}

beacon_entry_t *beacon_config_find_by_ibeacon(const uint8_t *uuid, uint16_t major, uint16_t minor)
{
    /* Strict UUID + major + minor match. major=0/minor=0 is a valid iBeacon
     * identity, not a wildcard. */
    for (int i = 0; i < s_count; i++) {
        if (s_beacons[i].active && s_beacons[i].type == BEACON_TYPE_IBEACON &&
            memcmp(s_beacons[i].uuid, uuid, 16) == 0 &&
            s_beacons[i].major == major && s_beacons[i].minor == minor) {
            return &s_beacons[i];
        }
    }
    return NULL;
}

static bool add_beacon(const cJSON *json, char *changed_id, size_t changed_len,
                       const char **error, const char **message, bool *used_alias)
{
    const char *id = json_string_or_null(json, "id");
    if (!id) {
        id = json_string_or_null(json, "name");
        if (id && used_alias) *used_alias = true;
    }
    const char *person_name = json_string_or_null(json, "person_name");
    const cJSON *beacon_obj = json_object_or_null(json, "beacon");
    const cJSON *discord_obj = json_object_or_null(json, "discord");
    const cJSON *home_obj = json_object_or_null(json, "home");
    const char *type = json_string_or_null(json, "type");
    if (!type && beacon_obj) type = json_string_or_null(beacon_obj, "type");

    if (s_count >= MAX_BEACONS) {
        *error = "max_records";
        *message = "Maximum beacon records reached";
        return false;
    }
    if (!valid_id(id)) {
        *error = "invalid_id";
        *message = "id must be 1-31 chars of A-Z, a-z, 0-9, underscore, or hyphen";
        return false;
    }
    if (beacon_config_find_by_id(id)) {
        *error = "duplicate_id";
        *message = "Beacon id already exists";
        return false;
    }
    if (!type) type = "ibeacon";

    const char *discord_id_src = json_string_or_null(json, "discord_channel_id");
    if (!discord_id_src && discord_obj) discord_id_src = json_string_or_null(discord_obj, "channel_id");
    const char *home_zone_src = json_string_or_null(json, "home_zone_entity");
    if (!home_zone_src && home_obj) home_zone_src = json_string_or_null(home_obj, "zone_entity");
    const char *notes_src = json_string_or_null(json, "notes");

    if (person_name && person_name[0] == '\0') person_name = NULL;
    if (person_name && strlen(person_name) > 48) {
        *error = "invalid_person_name";
        *message = "person_name must be 1-48 bytes";
        return false;
    }
    if (!valid_discord_channel(discord_id_src)) {
        *error = "invalid_discord_channel_id";
        *message = "discord_channel_id must be numeric and at most 32 bytes";
        return false;
    }
    if (home_zone_src && strlen(home_zone_src) > 64) {
        *error = "invalid_home_zone_entity";
        *message = "home_zone_entity must be at most 64 bytes";
        return false;
    }
    if (notes_src && strlen(notes_src) > 160) {
        *error = "invalid_notes";
        *message = "notes must be at most 160 bytes";
        return false;
    }

    beacon_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    copy_string(entry.name, sizeof(entry.name), id);
    copy_string(entry.person_name, sizeof(entry.person_name), person_name ? person_name : id);
    copy_string(entry.discord_channel_id, sizeof(entry.discord_channel_id), discord_id_src);
    copy_string(entry.home_zone_entity, sizeof(entry.home_zone_entity), home_zone_src);
    copy_string(entry.notes, sizeof(entry.notes), notes_src);
    /* Validate rssi_threshold BEFORE the (int8_t) cast. int8_t range is
     * -128..127, so values 129..255 would wrap to -127..-1 and silently
     * pass the bounds check, producing a wrong threshold that could make
     * the beacon never reach "near" (missed distress). */
    int rssi_threshold_int = json_int_or_default(json, "rssi_threshold",
                                                  json_int_or_default(json, "rssi",
                                                      beacon_obj ? json_int_or_default(beacon_obj, "rssi_threshold", -65) : -65));
    if (rssi_threshold_int < -127 || rssi_threshold_int > 0) {
        *error = "invalid_rssi";
        *message = "rssi threshold must be between -127 and 0";
        return false;
    }
    entry.rssi_threshold = (int8_t)rssi_threshold_int;
    entry.active = true;
    entry.state = BEACON_ABSENT;

    if (strcmp(type, "mac") == 0) {
        const char *mac = json_string_or_null(json, "mac");
        if (!mac && beacon_obj) mac = json_string_or_null(beacon_obj, "mac");
        if (!mac) mac = json_string_or_null(json, "value");
        if (!mac && beacon_obj) mac = json_string_or_null(beacon_obj, "value");
        if (!parse_mac(mac, entry.mac)) {
            *error = "invalid_mac";
            *message = "MAC must be AA:BB:CC:DD:EE:FF";
            return false;
        }
        entry.type = BEACON_TYPE_MAC;
    } else if (strcmp(type, "ibeacon") == 0) {
        const char *uuid = json_string_or_null(json, "uuid");
        if (!uuid && beacon_obj) uuid = json_string_or_null(beacon_obj, "uuid");
        if (!parse_uuid(uuid, entry.uuid)) {
            *error = "invalid_uuid";
            *message = "UUID must be 8-4-4-4-12 hex format";
            return false;
        }
        int major = json_int_or_default(json, "major",
                                        beacon_obj ? json_int_or_default(beacon_obj, "major", 0) : 0);
        int minor = json_int_or_default(json, "minor",
                                        beacon_obj ? json_int_or_default(beacon_obj, "minor", 0) : 0);
        if (major < 0 || major > 65535 || minor < 0 || minor > 65535) {
            *error = "invalid_major_minor";
            *message = "major and minor must be 0-65535";
            return false;
        }
        entry.type = BEACON_TYPE_IBEACON;
        entry.major = (uint16_t)major;
        entry.minor = (uint16_t)minor;
        if (duplicate_ibeacon_identity(entry.uuid, entry.major, entry.minor, NULL)) {
            *error = "duplicate_ibeacon";
            *message = "An active record already uses this UUID/major/minor";
            return false;
        }
    } else {
        *error = "invalid_type";
        *message = "type must be ibeacon or mac";
        return false;
    }

    s_beacons[s_count++] = entry;
    if (!beacon_config_save()) {
        /* Roll back the in-memory add so the next save doesn't carry a
         * record the operator was told had failed. */
        s_count--;
        *error = "nvs_save_failed";
        *message = "Beacon add accepted but NVS persistence failed; record not retained";
        return false;
    }
    copy_string(changed_id, changed_len, entry.name);
    *message = "Beacon added";
    ESP_LOGI(TAG, "Added beacon/person: %s (%s)", entry.name, entry.person_name);
    return true;
}

static bool remove_beacon(const cJSON *json, char *changed_id, size_t changed_len,
                          const char **error, const char **message, bool *used_alias)
{
    const char *id = json_string_or_null(json, "id");
    if (!id) {
        id = json_string_or_null(json, "name");
        if (id && used_alias) *used_alias = true;
    }
    if (!valid_id(id)) {
        *error = "invalid_id";
        *message = "missing or invalid id";
        return false;
    }
    for (int i = 0; i < s_count; i++) {
        if (strcasecmp(s_beacons[i].name, id) != 0) continue;
        beacon_entry_t removed_entry = s_beacons[i];
        copy_string(changed_id, changed_len, s_beacons[i].name);
        memmove(&s_beacons[i], &s_beacons[i + 1], (s_count - i - 1) * sizeof(beacon_entry_t));
        s_count--;
        if (!beacon_config_save()) {
            /* Rolled back above; restore the entry in memory because the
             * operator was told the remove failed. */
            memmove(&s_beacons[i + 1], &s_beacons[i], (s_count - i) * sizeof(beacon_entry_t));
            s_beacons[i] = removed_entry;
            s_count++;
            *error = "nvs_save_failed";
            *message = "Beacon remove accepted but NVS persistence failed; record not removed";
            return false;
        }
        *message = "Beacon removed";
        return true;
    }
    *error = "not_found";
    *message = "Beacon id not found";
    return false;
}

static bool update_beacon(const cJSON *json, char *changed_id, size_t changed_len,
                          const char **error, const char **message, bool *used_alias)
{
    const char *id = json_string_or_null(json, "id");
    if (!id) {
        id = json_string_or_null(json, "name");
        if (id && used_alias) *used_alias = true;
    }
    beacon_entry_t *entry = beacon_config_find_by_id(id);
    if (!entry) {
        *error = "not_found";
        *message = "Beacon id not found";
        return false;
    }

    const char *person_name = json_string_or_null(json, "person_name");
    const cJSON *discord_obj = json_object_or_null(json, "discord");
    const cJSON *home_obj = json_object_or_null(json, "home");
    const char *discord_channel_id = json_string_or_null(json, "discord_channel_id");
    if (!discord_channel_id && discord_obj) discord_channel_id = json_string_or_null(discord_obj, "channel_id");
    const char *home_zone_entity = json_string_or_null(json, "home_zone_entity");
    if (!home_zone_entity && home_obj) home_zone_entity = json_string_or_null(home_obj, "zone_entity");
    const char *notes = json_string_or_null(json, "notes");
    const cJSON *beacon_obj = json_object_or_null(json, "beacon");
    const cJSON *rssi = cJSON_GetObjectItem(json, "rssi");
    if (!rssi) rssi = cJSON_GetObjectItem(json, "rssi_threshold");
    if (!rssi && beacon_obj) rssi = cJSON_GetObjectItem(beacon_obj, "rssi_threshold");
    const cJSON *active = cJSON_GetObjectItem(json, "active");

    if (person_name && person_name[0] && strlen(person_name) > 48) {
        *error = "invalid_person_name";
        *message = "person_name must be 1-48 bytes";
        return false;
    }
    if (home_zone_entity && strlen(home_zone_entity) > 64) {
        *error = "invalid_home_zone_entity";
        *message = "home_zone_entity must be at most 64 bytes";
        return false;
    }
    if (notes && strlen(notes) > 160) {
        *error = "invalid_notes";
        *message = "notes must be at most 160 bytes";
        return false;
    }

    /* Snapshot the entry before mutation so a failed NVS save can roll back
     * the in-memory state to what was on disk. Without this, an NVS failure
     * would leave the operator told the update failed but the in-memory
     * roster reflecting the (unsaved) change. */
    beacon_entry_t snapshot = *entry;
    bool rssi_threshold_changed = false;

    if (person_name && person_name[0]) copy_string(entry->person_name, sizeof(entry->person_name), person_name);
    if (discord_channel_id) {
        if (!valid_discord_channel(discord_channel_id)) {
            *error = "invalid_discord_channel_id";
            *message = "discord_channel_id must be numeric and at most 32 bytes";
            return false;
        }
        copy_string(entry->discord_channel_id, sizeof(entry->discord_channel_id), discord_channel_id);
    }
    if (home_zone_entity) copy_string(entry->home_zone_entity, sizeof(entry->home_zone_entity), home_zone_entity);
    if (notes) copy_string(entry->notes, sizeof(entry->notes), notes);
    if (rssi) {
        if (!cJSON_IsNumber(rssi) || rssi->valueint < -127 || rssi->valueint > 0) {
            *error = "invalid_rssi";
            *message = "rssi threshold must be between -127 and 0";
            return false;
        }
        int8_t next_rssi_threshold = (int8_t)rssi->valueint;
        if (next_rssi_threshold != entry->rssi_threshold) {
            rssi_threshold_changed = true;
        }
        entry->rssi_threshold = next_rssi_threshold;
    }
    if (active) {
        bool will_be_active = cJSON_IsTrue(active);
        /* If activating (or remaining active) an iBeacon record, ensure no OTHER
         * active record shares this UUID/major/minor. Without this check, two
         * active records with identical iBeacon identity would both be matched
         * by find_by_ibeacon in array order, silently making the second child's
         * beacon permanently invisible -> missed left-in-van. */
        if (will_be_active && !entry->active && entry->type == BEACON_TYPE_IBEACON) {
            if (duplicate_ibeacon_identity(entry->uuid, entry->major, entry->minor, entry->name)) {
                *error = "duplicate_ibeacon";
                *message = "An active record already uses this UUID/major/minor";
                return false;
            }
        }
        entry->active = will_be_active;
        if (!entry->active) {
            entry->state = BEACON_ABSENT;
            entry->near_since = 0;
        }
    }
    if (rssi_threshold_changed) {
        entry->state = BEACON_ABSENT;
        entry->near_since = 0;
        entry->last_seen = 0;
    }

    if (!beacon_config_save()) {
        /* Roll back the in-memory entry so the next save attempt starts
         * from what's actually persisted. */
        *entry = snapshot;
        *error = "nvs_save_failed";
        *message = "Beacon update accepted but NVS persistence failed; change rolled back";
        return false;
    }
    copy_string(changed_id, changed_len, entry->name);
    *message = "Beacon updated";
    return true;
}

static void list_beacons(void)
{
    cJSON *root = build_roster_json();
    char *json = cJSON_PrintUnformatted(root);
    if (json) {
        mqtt_publish_checked(MQTT_BASE_TOPIC "/beacon/config/state", json, 1, true);
        free(json);
    }
    cJSON_Delete(root);
}

void beacon_config_handle_mqtt(const char *data, int len)
{
    char *buf = malloc(len + 1);
    if (!buf) return;
    memcpy(buf, data, len);
    buf[len] = '\0';

    cJSON *json = cJSON_Parse(buf);
    free(buf);
    if (!json) {
        publish_result(NULL, NULL, false, NULL, "invalid_json", "Payload is not valid JSON", false);
        return;
    }

    const char *request_id = json_string_or_null(json, "request_id");
    const char *action = json_string_or_null(json, "action");
    if (!action) {
        publish_result(request_id, NULL, false, NULL, "missing_action", "Missing action", false);
        cJSON_Delete(json);
        return;
    }

    char changed_id[32] = {0};
    const char *error = NULL;
    const char *message = NULL;
    bool ok = false;
    bool used_alias = false;
    bool removed = false;

    beacon_config_lock();
    if (destructive_change_blocked_locked(json, action, &error, &message)) {
        ok = false;
    } else if (strcmp(action, "add") == 0) {
        ok = add_beacon(json, changed_id, sizeof(changed_id), &error, &message, &used_alias);
        list_beacons();
    } else if (strcmp(action, "remove") == 0) {
        ok = remove_beacon(json, changed_id, sizeof(changed_id), &error, &message, &used_alias);
        removed = ok;
        list_beacons();
    } else if (strcmp(action, "update") == 0) {
        ok = update_beacon(json, changed_id, sizeof(changed_id), &error, &message, &used_alias);
        if (ok) list_beacons();
    } else if (strcmp(action, "list") == 0) {
        ok = true;
        message = "Beacon roster published";
        list_beacons();
    } else {
        error = "unknown_action";
        message = "Unknown beacon config action";
    }
    beacon_config_unlock();

    publish_result(request_id, action, ok, changed_id, error, message, used_alias);

    if (ok && changed_id[0]) {
        if (removed) {
            /* Clear retained per-beacon state + discovery topics so HA doesn't
             * keep showing a ghost beacon. */
            clear_beacon_retained_artifacts(changed_id);
        } else {
            beacon_entry_t snapshot;
            if (beacon_config_snapshot_by_id(changed_id, &snapshot) && snapshot.active) {
                mqtt_publish_beacon_discovery(changed_id);
            } else {
                clear_beacon_retained_artifacts(changed_id);
            }
        }
    }

    cJSON_Delete(json);
}

void beacon_config_publish_list(void)
{
    beacon_config_lock();
    list_beacons();
    beacon_config_unlock();
}

void beacon_config_add_telemetry(cJSON *root)
{
    cJSON *arr = cJSON_AddArrayToObject(root, "beacons");
    beacon_config_lock();
    for (int i = 0; i < s_count; i++) {
        if (!s_beacons[i].active) continue;
        cJSON *b = cJSON_CreateObject();
        cJSON_AddStringToObject(b, "id", s_beacons[i].name);
        cJSON_AddStringToObject(b, "name", s_beacons[i].name);
        cJSON_AddStringToObject(b, "person_name", s_beacons[i].person_name);
        cJSON_AddStringToObject(b, "state", state_str(s_beacons[i].state));
        cJSON_AddNumberToObject(b, "rssi", s_beacons[i].rssi);
        cJSON_AddNumberToObject(b, "rssi_threshold", s_beacons[i].rssi_threshold);
        cJSON_AddNumberToObject(b, "effective_rssi_threshold",
                                beacon_config_effective_rssi_threshold(&s_beacons[i]));
        cJSON_AddStringToObject(b, "discord_channel_id", s_beacons[i].discord_channel_id);
        cJSON_AddStringToObject(b, "home_zone_entity", s_beacons[i].home_zone_entity);
        cJSON_AddItemToArray(arr, b);
    }
    beacon_config_unlock();
}

bool beacon_config_save(void)
{
    nvs_handle_t handle;
    if (nvs_open("beacons", NVS_READWRITE, &handle) != ESP_OK) return false;

    bool ok = false;
    cJSON *root = build_roster_json();
    char *json = cJSON_PrintUnformatted(root);
    if (json) {
        esp_err_t err = nvs_set_str(handle, "json", json);
        if (err == ESP_OK && nvs_commit(handle) == ESP_OK) {
            ok = true;
        } else {
            ESP_LOGE(TAG, "NVS save failed: %s", esp_err_to_name(err));
        }
        free(json);
    }
    cJSON_Delete(root);
    nvs_close(handle);
    return ok;
}

bool beacon_config_any_near(void)
{
    bool any = false;
    beacon_config_lock();
    for (int i = 0; i < s_count; i++) {
        if (s_beacons[i].active && s_beacons[i].state == BEACON_NEAR) {
            any = true;
            break;
        }
    }
    beacon_config_unlock();
    return any;
}

void beacon_config_queue_near_distress_events(void)
{
    beacon_config_lock();
    for (int i = 0; i < s_count; i++) {
        if (!s_beacons[i].active || s_beacons[i].state != BEACON_NEAR) continue;
        distress_event_t evt = {
            .type = DISTRESS_BEACON_NEAR,
            .timestamp = esp_timer_get_time(),
        };
        copy_string(evt.beacon_name, sizeof(evt.beacon_name), s_beacons[i].name);
        if (xQueueSend(g_distress_queue, &evt, 0) != pdTRUE) {
            ESP_LOGW(TAG, "Distress queue full, dropping arm-time beacon event");
        }
    }
    beacon_config_unlock();
}
