#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Source-agnostic ignition detection API.
 *
 * Two compile-time selectable sources implement this contract:
 *  - IGNITION_SOURCE_GPIO:   direct GPIO4 sense (legacy, always-on hardware)
 *  - IGNITION_SOURCE_ESPNOW: ESP-NOW heartbeat from a dumb ignition-powered
 *                            remote ESP32; presence of heartbeats == ignition on,
 *                            silence == ignition off.
 *
 * bus_state.c polls ignition_get() every 250 ms and drives state transitions.
 * Each source owns its own stabilization (GPIO debounce / ESPNOW boot-grace
 * + heartbeat timeout) and reports its own health via ignition_source_status().
 */

typedef enum {
    IGNITION_SOURCE_OK = 0,       /* source reports a stable, fresh value */
    IGNITION_SOURCE_STALE,        /* no fresh data (e.g. ESPNOW: heartbeat timeout) */
    IGNITION_SOURCE_ERROR,        /* source initialization failed */
    IGNITION_SOURCE_BOOT,         /* boot grace window: assuming on until first signal */
    IGNITION_SOURCE_OVERRIDDEN,   /* bus_state.c override (bus_empty_on/off) is active */
} ignition_source_status_t;

/* Initialize the active ignition source. Called once from app_main before
 * bus_state_task starts. Safe for bus_state.c to call before WiFi is up:
 *  - GPIO source configures the pin synchronously and returns.
 *  - ESPNOW source blocks on WIFI_STARTED_BIT (set in wifi.c), then inits
 *    esp_now and registers the receive callback.
 * Returns ESP_OK on success. */
#include "esp_err.h"
void ignition_init(void);

/* Poll the stabilized ignition value. Called from bus_state_task every 250 ms.
 * Returns the debounced/stabilized ignition state (true = on). For ESPNOW,
 * returns true during BOOT grace, true while heartbeats are fresh, false
 * after heartbeat timeout. For GPIO, returns the debounced LEVEL sensing. */
bool ignition_get(void);

/* Source health. Returns one of IGNITION_SOURCE_OK/STALE/ERROR/BOOT depending
 * on the source's internal state. Note: this returns the raw source health,
 * not accounting for the bus_state.c override. bus_state.c reports the
 * override layer separately via ignition_source_status_override(). */
ignition_source_status_t ignition_source_status(void);

/* Human-readable name of the active source ("espnow" or "gpio"). */
const char *ignition_source_name(void);

/* Status as a lowercase string for telemetry/HA. */
const char *ignition_source_status_name(ignition_source_status_t status);

/* ESPNOW telemetry snapshot (only meaningful when source == ESPNOW).
 * Populated into bus/state JSON by mqtt_publish_bus_state(). */
typedef struct {
    int64_t last_heartbeat_age_us;  /* time since last valid heartbeat; -1 if never */
    uint32_t remote_uptime_s;      /* remote's uptime from the last heartbeat payload */
    uint16_t remote_battery_mv;    /* remote's ADC battery reading; 0 if unsupported */
    uint16_t seq;                  /* sequence counter from last heartbeat */
    uint16_t missed_seq;           /* estimated missed frames (gap detection) */
} ignition_espnow_diag_t;

/* Fill ignition_espnow_diag_t. Only valid for ESPNOW source; returns false
 * (and leaves *out zeroed) for the GPIO source. */
bool ignition_espnow_diag(ignition_espnow_diag_t *out);
