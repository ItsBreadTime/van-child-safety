# T-BLE-BREAD-LATENCY Result

Rounds passed/total: 10/10
Result: pass

Expected behavior:
Automated Bread iBeacon START/STOP commands produce `bus/beacon/bread/state` transitions `near`/`absent` without manual phone taps.

Observed behavior:
See `beacon_latency.csv`, `beacon_latency_events.jsonl`, `adb_control.log`, and `bread_beacon_test_logcat.txt`.

Metrics:
- START -> near latency min/median/mean/max: 2294.9/2636.8/2563.8/2705.0 ms
- STOP -> absent latency min/median/mean/max: 13420.9/13660.9/13797.6/14276.0 ms
- All transitions latency min/median/mean/max: 2294.9/8062.9/8180.7/14276.0 ms

Evidence files:
- beacon_latency.csv
- beacon_latency_events.jsonl
- beacon_latency_summary.json
- adb_control.log
- bread_beacon_test_logcat.txt
- phone_control_notes.txt

Notes for Chapter 4:
This is a simulated BLE transmitter latency test, not a BeaconScope UI latency test. It isolates the bus BLE scanner/MQTT reporting path using the same Bread iBeacon UUID/major/minor.

Notes for Chapter 5:
A dedicated programmable beacon/test transmitter gives better repeatability than manual BeaconScope toggles.
