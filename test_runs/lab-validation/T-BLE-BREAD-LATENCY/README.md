# T-BLE-BREAD-LATENCY

Environment: simulated
Date/time: 2026-07-04T00:06:36+07:00
Firmware commit: 4d69e42
Ignition source: 
Relevant config: automated Android BLE test transmitter for Bread iBeacon; observe bus/beacon/bread/state
Round count planned: 10
Operator: 
Supervisor: 

Physical setup:
- Door/window state:
- Fan/AC state:
- Sensor placement:
- Beacon placement:
- LD2412 target position:

Safety notes:
- Abort criteria:
- Actual abort condition reached: yes/no

Automation notes:
- BeaconScope was force-stopped so it did not emit a duplicate Bread iBeacon.
- Temporary package: `io.operator.busbeacontest`
- Control command: `adb shell am start -W -n io.operator.busbeacontest/.CommandActivity -a io.operator.busbeacontest.START|STOP`
- MQTT evidence: `beacon_latency_events.jsonl`, `beacon_latency.csv`, `beacon_latency_summary.json`
- Android evidence: `adb_control.log`, `bread_beacon_test_logcat.txt`, `phone_control_notes.txt`
