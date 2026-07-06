#pragma once

#include <stdint.h>

/* Shared ESP-NOW wire protocol between the main firmware (ignition_espnow.c)
 * and the dumb ignition remote (ignition_remote/main.c).
 *
 * The remote is ignition/accessory-powered; when ignition is on it boots,
 * inits ESP-NOW, and broadcasts this struct every ESPNOW_HEARTBEAT_INTERVAL_MS.
 * When ignition turns off it loses power; heartbeats stop and the main's
 * receiver declares ignition off after ESPNOW_HEARTBEAT_TIMEOUT_MS.
 *
 * The heartbeat carries diagnostics that cost nothing to populate:
 *  - seq: detects dropped frames (gap detection)
 *  - uptime_s: remote reboots (ignition restored) are flagged by uptime reset
 *  - battery_mv: catches a failing supply regulator before it becomes silent
 *
 * The magic filters out unrelated ESP-NOW broadcasters. It is shared via
 * Kconfig (CONFIG_ESPNOW_SECRET) at compile time on both sides. */

/* The magic is the FNV-1a hash of CONFIG_ESPNOW_SECRET (a Kconfig string shared
 * by both sides). This lets operators use a memorable secret; both main and
 * remote hash the same string to the same 32-bit magic at build time. Change
 * CONFIG_ESPNOW_SECRET to avoid cross-talk between adjacent buses running the
 * same firmware. */

/* Payload broadcast by the remote. 12 bytes. */
typedef struct __attribute__((packed)) {
    uint32_t magic;       /* IGNITION_ESPNOW_MAGIC / CONFIG_ESPNOW_SECRET */
    uint16_t seq;         /* monotonic, wraps every ~36 h at 2 s interval */
    uint32_t uptime_s;    /* remote's esp_timer_get_time()/1e6 at send time */
    uint16_t battery_mv;  /* ADC read of supply rail, 0 if unsupported */
} ignition_heartbeat_t;

_Static_assert(sizeof(ignition_heartbeat_t) == 12,
               "ignition_heartbeat_t must be 12 bytes on the wire");
