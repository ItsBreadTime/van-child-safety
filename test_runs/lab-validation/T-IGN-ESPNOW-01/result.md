# T-IGN-ESPNOW-01 Result

Rounds passed/total: 1/1
Result: pass

Expected behavior:
When the ESP-NOW ignition remote is unplugged while the main board is active, the main board stops receiving heartbeats, `ignition_source_status` changes from `ok` to `stale`, `ignition_on` becomes false, and `bus/state` enters `exit_grace`. When the remote is plugged back in, heartbeats resume and the bus returns to `active`.

Observed behavior:
At capture start, `bus/state` was `active`, `ignition_on=true`, and `ignition_source_status=ok`. After the remote was unplugged, `bus/state` changed to `exit_grace` with `ignition_on=false`, `ignition_source_status=stale`, and `last_heartbeat_age_s=10.125010`. After the remote was plugged back in, `bus/state` returned to `active` with `ignition_on=true`, `ignition_source_status=ok`, and `last_heartbeat_age_s=0.187159`. No active distress remained latched; relay status stayed online and error clear.

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
T-IGN-ESPNOW-01 passed 1/1 supervised simulated round. ESP-NOW remote unplug produced `ok` to `stale` and `active` to `exit_grace`; replug restored `ok` and `active`. Evidence: `mqtt.log` lines for `bus/state`, `bus/event` (`ignition_off`, `ignition_on`), and relay status.

Notes for Chapter 5:
The round was intentionally stopped before the system entered `armed` because both active beacons were near and LD2412 target was true; this avoided turning an ignition test into a detection/distress test.
