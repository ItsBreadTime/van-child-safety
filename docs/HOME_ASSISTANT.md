# Home Assistant

The files in `homeassistant/` are sanitized templates, not a drop-in copy of a live installation.

## Files

- `bus_project_package.yaml`: MQTT sensors, helpers, scripts, and automations.
- `bus_child_home_zone_blueprint.yaml`: per-child home-zone event publisher.
- `dashboard.yaml`: Lovelace dashboard example using common HACS cards.

## Before Use

Replace placeholders such as:

- `person.bus_tracker`
- `zone.school`
- `zone.example_home`
- `notify.mobile_app_operator_phone`
- entity IDs under `sensor.bus_sensor_*`

The dashboard entity IDs depend on your Home Assistant MQTT discovery history. Confirm the actual entity IDs in Developer Tools before copying the dashboard.

## Security

Keep Home Assistant long-lived access tokens, MQTT credentials, exact addresses, and live route evidence out of this repo.
