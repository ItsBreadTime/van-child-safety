# School Van Child Left-Behind Monitoring and Alert System

Prototype ESP32-based van child-safety monitor using BLE beacons, cabin sensors, MQTT/Home Assistant, and operator alerts for child-left-in-vehicle risk.

> Safety note: this is a research/prototype system, not a certified safety product. Do not rely on it as the only way to verify that a vehicle is empty. A physical cabin check and responsible operator procedure remain required.

## What It Does

- Reads ignition state from an ESP-NOW ignition remote or a protected GPIO input.
- Tracks cabin evidence from BLE iBeacon tags, LD2412 mmWave presence, SCD4x CO2, SHT4x temperature/humidity, and optional GPS.
- Publishes telemetry, state, roster, command, and distress topics over MQTT.
- Integrates with Home Assistant through MQTT discovery and optional YAML templates.
- Sends readable Discord bot alerts through a local Python relay.
- Supports guarded disarm workflows that require explicit cabin-clear confirmation.

## Repository Layout

```text
main/                 ESP-IDF firmware for the main ESP32-S3 unit
ignition_remote/      ESP-NOW heartbeat firmware for an ignition-powered remote
discord_relay/        Python Discord/MQTT relay with tests and Docker support
homeassistant/        Sanitized Home Assistant package/dashboard templates
hardware/             PCB, enclosure, and manufacturing/export files
docs/                 Public setup, configuration, protocol, and safety docs
test_runs/            Sanitized validation evidence and raw logs
```

The Android/mobile beacon app, thesis/report narrative drafts, private configs, generated office documents, and unsanitized runtime artifacts are intentionally not included in this public export. Sanitized validation evidence is included under [test_runs](test_runs/).

## Quick Start

1. Install ESP-IDF 5.x for `esp32s3`.
2. Copy `main/secrets.h.example` to `main/secrets.h` and fill in your own Wi-Fi, WireGuard, MQTT, and OTA values.
3. Build the firmware:

```bash
idf.py set-target esp32s3
idf.py build
```

4. Flash over USB:

```bash
idf.py -p /dev/cu.usbserial-XXXX flash monitor
```

5. Configure the relay:

```bash
cd discord_relay
cp .env.example .env
docker compose up -d --build
```

See [docs/SETUP.md](docs/SETUP.md) for the full setup path.

## Documentation

- [Setup](docs/SETUP.md)
- [Configuration](docs/CONFIGURATION.md)
- [MQTT contract](docs/MQTT.md)
- [Home Assistant templates](docs/HOME_ASSISTANT.md)
- [Hardware](docs/HARDWARE.md)
- [Flashing and OTA](docs/FLASHING.md)
- [Safety model](docs/SAFETY.md)
- [Sanitized test evidence](test_runs/)

## Tests

```bash
python -m pip install -r discord_relay/requirements.txt -r discord_relay/requirements-test.txt
python -m pytest discord_relay/tests -q
```

## License

Software is licensed under the Apache License 2.0. Hardware design files under `hardware/` are licensed under CERN-OHL-S v2; see [hardware/LICENSE](hardware/LICENSE).
