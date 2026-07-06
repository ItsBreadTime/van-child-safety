# T-HA-01

Environment: simulated-static
Date/time: 2026-07-04T19:13:11+07:00
Firmware commit: 4d69e42
Ignition source: 
Relevant config: pytest discord_relay/tests/test_ha_package.py -q; YAML/package/dashboard structural audit only
Round count planned: 1
Operator: operator
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
