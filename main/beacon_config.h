#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "cJSON.h"

typedef enum {
    BEACON_TYPE_MAC,
    BEACON_TYPE_IBEACON,
} beacon_type_t;

typedef enum {
    BEACON_ABSENT,
    BEACON_FAR,
    BEACON_NEAR,
} beacon_state_t;

typedef struct {
    /* `name` is retained as the internal immutable topic id for compatibility. */
    char name[32];
    char person_name[49];
    char discord_channel_id[33];
    char home_zone_entity[65];
    char notes[161];
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
} beacon_entry_t;

#define MAX_BEACONS 20

void beacon_config_init(void);
void beacon_config_lock(void);
void beacon_config_unlock(void);
int beacon_config_count(void);
beacon_entry_t *beacon_config_get(int index);
beacon_entry_t *beacon_config_find_by_mac(const uint8_t *mac);
beacon_entry_t *beacon_config_find_by_ibeacon(const uint8_t *uuid, uint16_t major, uint16_t minor);
beacon_entry_t *beacon_config_find_by_id(const char *id);
/* Atomic snapshot of a beacon entry under the lock. Returns true if found.
 * Safe to dereference the output without holding the beacon mutex. Use this
 * from tasks other than the holder of beacon_config_lock (e.g. the
 * notification task publishing distress) to avoid use-after-free if a
 * concurrent remove_beacon memmoves the table. */
bool beacon_config_snapshot_by_id(const char *id, beacon_entry_t *out);
bool beacon_config_snapshot(beacon_entry_t *entry, beacon_entry_t *out);
void beacon_config_handle_mqtt(const char *data, int len);
void beacon_config_publish_list(void);
void beacon_config_add_telemetry(cJSON *root);
/* Returns true on successful NVS commit. Callers (legacy migration) must
 * only erase the legacy blob when this returns true, otherwise the roster
 * is lost on next boot. */
bool beacon_config_save(void);
bool beacon_config_any_near(void);
void beacon_config_queue_near_distress_events(void);
int beacon_config_effective_rssi_threshold(const beacon_entry_t *entry);
