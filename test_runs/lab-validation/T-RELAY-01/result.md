# T-RELAY-01 Result

Rounds passed/total: 3/3
Result: pass

Expected behavior:
Simulated relay inputs produce the expected notification severity classes: routine `bus_clear`, warning `sensor_fault`, and emergency `distress/state active`. Relay status should remain online and error clear.

Observed behavior:
Three simulated MQTT events were published. `bus_clear` produced a non-emergency "Bus clear" relay event. `sensor_fault` produced a non-emergency warning relay event. `bus/distress/state` with `severity=emergency` and `state=active` produced an emergency `@everyone` relay event. `bus/relay/discord/error` remained clear.

Metrics:
- End-to-end latency:
- Relay-only latency: approximately 0-3 s from MQTT capture timestamp to relay `last_event` timestamp for the simulated events
- FP count:
- FN count:
- Reconnect time:

Evidence files:
- serial.log
- mqtt.log
- relay_before.jsonl
- relay_after.jsonl

Notes for Chapter 4:
T-RELAY-01 passed 3/3 simulated relay severity rounds. Routine, warning, and emergency MQTT inputs were reflected in relay output with matching severity (`emergency=false` for routine/warning, `emergency=true` for distress). Evidence: `mqtt.log`, `events.csv`, and retained `bus/relay/discord/last_event`. This supports relay policy for synthetic payloads, not firmware sensor detection.

Notes for Chapter 5:
Live beacon transition messages occurred during the test window; Chapter 4 should cite the scenario-specific MQTT event lines when reporting this result.
