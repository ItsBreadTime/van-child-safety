# T-BLE-FP-03 Result

Rounds passed/total: 1/1
Result: pass

Expected behavior:
With the Bread iBeacon transmitter stopped while the bus remains active, the
beacon should report `absent` and the system should not arm or publish a
non-empty distress payload.

Observed behavior:
The Android Bread iBeacon test transmitter was stopped through ADB. The
firmware reported `bus/beacon/bread/state` as `absent`, `bus/state` remained
`active`, `distress_active=false`, and no non-empty `bus/distress/state`
payload was observed during the capture window.

Metrics:
- End-to-end latency:
- Relay-only latency:
- FP count: 0
- FN count:
- Reconnect time:

Evidence files:
- mqtt.log
- relay_before.jsonl
- relay_after.jsonl
- adb_control.log
- t_ble_fp_03_summary.json

Notes for Chapter 4:
T-BLE-FP-03 passed 1/1 simulated absent-beacon round. This supports the absent
state/no-distress behavior while the bus is active. It does not test armed-state
RSSI threshold false positives or real-van RF multipath.

Notes for Chapter 5:
