# T-BLE-FP-ACTIVE-01 Result

Rounds passed/total: 6/6
Result: pass

Expected behavior:
While ignition is on and bus state is active, repeated Bread iBeacon near/absent transitions should update beacon state without arming the bus or raising distress.

Observed behavior:
- Precheck: state=active, ignition_on=True, armed=False, distress_active=False.
- Automated Bread iBeacon transitions passed 6/6 while bus stayed active/not armed/no distress.
- Transition latency min/median/mean/max: 2701.5/8132.0/7757.1/13557.8 ms.

Metrics:
- Transition latency min/median/mean/max: 2701.5/8132.0/7757.1/13557.8 ms

Evidence files:
- mqtt.log
- ble_fp_events.jsonl
- ble_fp_transitions.csv
- ble_fp_summary.json
- adb_control.log
- bread_beacon_test_logcat.txt
- relay_before.jsonl

Notes for Chapter 4:
This is useful active-state false-positive evidence: repeated Bread near/absent transitions did not arm the bus or raise distress while ignition was on. It should not be generalized to armed-state false positives, marginal RSSI threshold behavior, or real-van RF multipath.

Notes for Chapter 5:
For stronger BLE FP coverage, repeat at several physical distances/RSSI values around the configured threshold.
