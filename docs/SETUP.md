# Setup

## Prerequisites

- ESP-IDF 5.x with ESP32-S3 support
- Python 3.12 for the relay and tests
- Docker or another Python runtime for the relay
- MQTT broker reachable from the firmware and relay
- Home Assistant with MQTT integration, if using the HA templates

## Firmware

```bash
cp main/secrets.h.example main/secrets.h
idf.py set-target esp32s3
idf.py menuconfig
idf.py build
idf.py -p /dev/cu.usbserial-XXXX flash monitor
```

Fill `main/secrets.h` with your own network, WireGuard, MQTT, and OTA values. Do not commit that file.

## Ignition Remote

The ignition remote is a separate ESP-IDF project. It broadcasts ESP-NOW heartbeats while powered by the vehicle ignition/accessory rail.

```bash
idf.py -C ignition_remote set-target esp32s3
idf.py -C ignition_remote menuconfig
idf.py -C ignition_remote -p /dev/cu.usbserial-XXXX flash monitor
```

The main firmware and remote firmware must use the same `ESPNOW_SECRET`.

## Discord Relay

```bash
cd discord_relay
cp .env.example .env
docker compose up -d --build
```

Set MQTT credentials, Discord bot token, channel IDs, and optional operator allow-lists in `.env`.

## Home Assistant

Start from the sanitized templates in `homeassistant/`. Adjust person/entity IDs, zone names, notification service names, and dashboard entity IDs for your own HA instance.
