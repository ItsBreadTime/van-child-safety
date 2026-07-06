# Configuration

Most firmware options live in `main/Kconfig.projbuild` and can be changed with `idf.py menuconfig`.

Sensitive deployment values should be placed in `main/secrets.h`, copied from `main/secrets.h.example`. Keep `main/secrets.h` out of Git.

## Required Secrets

- Wi-Fi SSID/password
- WireGuard private key, peer public key, and optional preshared key
- MQTT broker URI, username, and password
- OTA token, if OTA updates are enabled
- Optional legacy Discord webhook URL, normally left blank

## Important Runtime Options

- `MQTT_BASE_TOPIC`: default `bus`
- `DEVICE_ID`: default `bus_001`
- `DEVICE_NAME`: Home Assistant display name
- `IGNITION_SOURCE`: ESP-NOW remote by default, GPIO optional
- `EXIT_GRACE_SECONDS`: delay after ignition-off before armed monitoring
- `DISTRESS_INITIAL_DELAY_S`: delay before emergency broadcast
- `BLE_BEACON_MIN_NEAR_RSSI`: default near threshold
- `CO2_RISE_THRESHOLD_PPM`: CO2 delta trigger threshold

## ESP-NOW Secret

`ESPNOW_SECRET` is not a strong cryptographic protocol; it is a shared build-time discriminator hashed into the heartbeat payload so adjacent systems do not cross-talk accidentally. Change it for every deployment and compile the same value into the main firmware and ignition remote.

## Relay Environment

See `discord_relay/.env.example` for MQTT, Discord, operator allow-list, and metrics settings.
