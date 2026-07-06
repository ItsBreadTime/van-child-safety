# T-REAL-VAN-OCCUPANT-01

Environment: real van supervised adult occupant presence test
Date/time: 2026-07-05T13:38:40+07:00
Firmware commit: 795deef
Ignition source: live vehicle/installed system
Relevant config: Live MQTT bus/# capture plus remote Discord relay evidence; supervised adult occupant; do not lock occupant in; maintain exit path and outside observer
Round count planned: 1
Operator: operator
Supervisor: operator

Physical setup:
- Door/window state: unlocked exit path required; do not lock occupant in
- Fan/AC state: record actual state before and during test
- Sensor placement:
- Beacon placement:
- LD2412 target position:

Safety notes:
- Abort criteria: stop immediately for heat, discomfort, network uncertainty, loss of outside observer, or any blocked exit
- Actual abort condition reached: no at setup time

Automation notes:
- MQTT log: `mqtt.log`
- Serial log: `serial.log`
- Relay snapshots: `relay_before.jsonl`, `relay_after.jsonl`
- Measurements: `measurements.csv`, `co2.csv`, `rssi.csv`
