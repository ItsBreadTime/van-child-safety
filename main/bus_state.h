#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef enum {
    BUS_STATE_BOOT = 0,
    BUS_STATE_ACTIVE,
    BUS_STATE_EXIT_GRACE,
    BUS_STATE_ARMED,
    BUS_STATE_DISTRESS_PENDING,
    BUS_STATE_DISTRESS_ACTIVE,
    BUS_STATE_ACKED,
    BUS_STATE_DISARMED,
    BUS_STATE_FAULT,
} bus_state_t;

typedef enum {
    BUS_DISARM_OK = 0,
    BUS_DISARM_ALREADY,
    BUS_DISARM_REJECT_PENDING,
    BUS_DISARM_REJECT_UNACKNOWLEDGED,
    BUS_DISARM_REJECT_STATE,
} bus_disarm_result_t;

void bus_state_task(void *pvParameters);
bus_state_t bus_state_get(void);
const char *bus_state_name(bus_state_t state);
bool bus_state_is_armed(void);
bool bus_state_allows_attendance(void);
bool bus_state_ignition_on(void);
bool bus_state_distress_active(void);
bool bus_state_distress_acknowledged(void);
int bus_state_exit_grace_remaining_s(void);
uint32_t bus_state_armed_generation(void);
void bus_state_set_co2_baseline(uint16_t co2);
uint16_t bus_state_co2_baseline(void);
const char *bus_state_distress_id(void);
bool bus_state_mark_distress_pending(void);
bool bus_state_mark_distress_active(void);
void bus_state_ack_distress(void);
void bus_state_clear_distress(void);
/* Explicit operator disarm after a physical cabin-clear check. This never
 * changes the measured ignition value. Active/pending distress must be
 * acknowledged first. The disarmed state lasts until the next ignition edge
 * or reboot. */
bus_disarm_result_t bus_state_disarm(void);
const char *bus_state_disarm_result_name(bus_disarm_result_t result);
void bus_state_force_empty(bool empty);
/* True when the bus_empty_on/off override is shadowing the real source value.
 * mqtt_publish_bus_state uses this to report ignition_source_status ==
 * "overridden" so operators can see the override in HA. */
bool bus_state_override_active(void);
void bus_state_publish(void);
/* Re-publish the active distress immediately (called on MQTT reconnect). */
void bus_state_publish_distress_repeat(void);
