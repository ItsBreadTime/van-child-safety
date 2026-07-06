#pragma once

#include <mqtt_client.h>
#include "cJSON.h"
#include "common.h"

void mqtt_task(void *pvParameters);
void mqtt_publish_beacon_state(const char *name, const char *state, int rssi);
void mqtt_publish_beacon_discovery(const char *name);
void mqtt_publish_beacon_states(void);
void mqtt_publish_distress(const distress_event_t *event);
void mqtt_clear_distress(void);
void mqtt_publish_bus_state(void);
void mqtt_publish_event_json(cJSON *root);
void mqtt_publish_command_result(const char *request_id, const char *action,
                                 bool ok, const char *reason, const char *source);
esp_mqtt_client_handle_t mqtt_get_client(void);
/* Publish with error logging. Returns message id, or -1 if not connected.
 * data may be NULL for an empty retained clear. retain is bool for clarity. */
int mqtt_publish_checked(const char *topic, const char *data, int qos, bool retain);
/* Convenience: publish a retained empty payload to clear a retained topic. */
void mqtt_publish_clear_retained(const char *topic);
