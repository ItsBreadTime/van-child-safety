# T-HA-IGN-01 Result

Rounds passed/total: 1/1
Result: pass

Expected behavior:
When the simulated ignition override changes, the MQTT `bus/state` fields used
by Home Assistant entities should update consistently: `ignition_on=false` and
an armed-equivalent state after `bus_empty_on`, then `ignition_on=true` and
`state=active` after `bus_empty_off`.

Observed behavior:
The test used the documented `bus_empty_on`/`bus_empty_off` command path. After
`bus_empty_on`, MQTT `bus/state` reported `ignition_on=false`, `armed=true`, and
`state=armed`. After `bus_empty_off`, MQTT `bus/state` reported
`ignition_on=true` and `state=active`. This verifies the MQTT source fields that
Home Assistant consumes. It does not include a live Home Assistant screenshot.

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
- t_ha_ign_01_summary.json

Notes for Chapter 4:
T-HA-IGN-01 passed 1/1 simulated MQTT-source round. The state fields consumed by
Home Assistant updated across the ignition override transition. This should be
reported as MQTT source-state evidence, not live dashboard rendering evidence.

Notes for Chapter 5:
The stronger follow-up remains a live Home Assistant dashboard screenshot during
a real ignition-source toggle.
