# T-IGN-RAPID-01

Environment: simulated
Date/time: 2026-07-04T00:03:29+07:00
Firmware commit: 4d69e42
Ignition source: espnow
Relevant config: rapid ESP-NOW remote unplug/replug under 5 seconds; expected no stale timeout or false state transition
Round count planned: 1
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
- MQTT log: `mqtt.log`
- Serial log: `serial.log`
- Relay snapshots: `relay_before.jsonl`, `relay_after.jsonl`
- Measurements: `measurements.csv`, `co2.csv`, `rssi.csv`
