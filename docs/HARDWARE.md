# Hardware

The prototype uses:

- ESP32-S3 board with PSRAM
- SCD4x CO2 sensor
- SHT4x temperature/humidity sensor
- HLK-LD2412 mmWave presence sensor
- GPS receiver, optional
- BLE/iBeacon tags
- Ignition-powered ESP32 remote, optional but recommended
- Carrier PCB and 3D-printed enclosure under `hardware/`

## Main Firmware Pin Map

| Function | Default |
|---|---|
| I2C SDA | GPIO5 |
| I2C SCL | GPIO6 |
| LD2412 UART TX from ESP32 | GPIO7 |
| LD2412 UART RX to ESP32 | GPIO8 |
| GPS UART RX | GPIO9 |
| Legacy ignition GPIO | GPIO4 |

Check `main/Kconfig.projbuild` and the PCB files before wiring a different board.

## Enclosure Notes

- Keep the ESP32 antenna clear of metal.
- Give the GPS antenna a reasonable sky view if GPS is used.
- Do not place thick FR4 or metal directly in front of the LD2412 radar path.
- Validate thermal behavior and power stability in the actual enclosure before field use.
