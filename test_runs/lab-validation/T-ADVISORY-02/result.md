# T-ADVISORY-02 Result

Rounds passed/total: 1/1
Result: pass

Expected behavior:
A `sensor_fault` advisory event should be delivered by the relay as a
non-emergency warning, with relay error remaining clear.

Observed behavior:
The test published a simulated MQTT `bus/event` payload with
`event=sensor_fault`, `sensor=ld2412`, and a Chapter 4 test source. The relay
updated retained `bus/relay/discord/last_event` with non-emergency content
warning that a critical bus sensor is reporting a persistent fault, and the
embed title was `Sensor fault`. The relay error topic remained clear.

Metrics:
- End-to-end latency:
- Relay-only latency:
- FP count:
- FN count:
- Reconnect time:

Evidence files:
- mqtt.log
- relay_before.jsonl
- relay_after.jsonl
- command_log.txt
- t_advisory_02_summary.json

Notes for Chapter 4:
T-ADVISORY-02 passed 1/1 simulated MQTT advisory round. This validates relay
handling of a `sensor_fault` advisory event. It does not prove firmware detects
a disconnected or failed LD2412/SCD4x sensor.

Notes for Chapter 5:
The firmware-level sensor-fault tests still require hardware fault injection or
sensor disconnection under controlled conditions.
