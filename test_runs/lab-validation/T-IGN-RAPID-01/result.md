# T-IGN-RAPID-01 Result

Rounds passed/total: 1/1
Result: pass

Expected behavior:
Rapid ESP-NOW remote unplug/replug shorter than the heartbeat timeout should not make the main board mark the ignition source stale, should not publish `ignition_off`, and should not transition from `active` into `exit_grace` or `armed`.

Observed behavior:
During the rapid unplug/replug round, the MQTT capture showed `bus/state` remained `active` with `ignition_on=true`, `ignition_source_status=ok`, `exit_grace_remaining_s=0`, and `distress_active=false`. No `bus/event` rows were captured, and no `bus/state` payload with `exit_grace`, `armed`, or `stale` appeared in the scenario log. Relay status remained online and error clear.

Metrics:
- End-to-end latency:
- Relay-only latency:
- FP count:
- FN count:
- Reconnect time:

Evidence files:
- serial.log
- mqtt.log
- relay_before.jsonl
- relay_after.jsonl

Notes for Chapter 4:
T-IGN-RAPID-01 passed 1/1 supervised simulated round. A rapid ESP-NOW remote unplug/replug did not produce `ignition_source_status=stale`, `ignition_off`, `exit_grace`, `armed`, or distress. Evidence: `mqtt.log`, `measurements.csv`, and `relay_after.jsonl`.

Notes for Chapter 5:
This round verifies behavior for a short interruption below the ESP-NOW heartbeat timeout; it does not replace the longer timeout test in `T-IGN-ESPNOW-01`.
