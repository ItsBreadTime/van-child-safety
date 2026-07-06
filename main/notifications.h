#pragma once

void notification_task(void *pvParameters);

/* Re-publish the current active distress immediately (called on MQTT reconnect).
 * No-op if no distress is currently active. */
void notification_republish_distress(void);

/* Handle a distress_ack command. If the tracker is in the pending window
 * (alarm not yet broadcast), cancel the alarm before it fires and emit
 * distress_clear so retained MQTT state and the relay both see the
 * incident as closed. If the alarm has already fired, this is a no-op
 * (the regular ack path in bus_state.c handles the post-broadcast case). */
void notification_handle_ack(void);
