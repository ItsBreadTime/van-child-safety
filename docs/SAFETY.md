# Safety Model

This system is a prototype. It should support, not replace, human safety procedure.

## Core Assumptions

- A responsible operator still performs a physical cabin check.
- MQTT command topics are restricted by broker ACLs.
- Disarm requires explicit confirmation that the cabin has been checked.
- Emergency alerts should be readable and actionable, with evidence attached.
- Sensor faults, stale data, and network loss should fail toward caution.

## Known Limitations

- BLE RSSI is noisy and cannot prove that a child is physically present.
- CO2 rise depends on cabin volume, ventilation, timing, and sensor placement.
- mmWave presence sensors can see non-child motion or residual occupant signatures.
- GPS and Home Assistant location data can be unavailable, stale, or privacy-sensitive.
- ESP-NOW heartbeat loss means ignition status is inferred, not directly measured.

Do not create real heat-risk or unattended-child scenarios while testing.
