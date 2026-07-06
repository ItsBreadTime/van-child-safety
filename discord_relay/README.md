# Discord Relay

Dockerized Python relay that subscribes to the Van Child Safety MQTT topics, sends Discord bot alerts, and optionally exposes operator-only slash commands for status, guarded disarm, and beacon roster management.

## Setup

```bash
cp .env.example .env
docker compose up -d --build
```

Configure `.env` with your own MQTT broker, Discord bot token, channel IDs, and operator allow-list values.

## MQTT Inputs

- `bus/state`
- `bus/telemetry`
- `bus/beacon/+/state`
- `bus/beacon/config/state`
- `bus/distress/state`
- `bus/event`
- `bus/ha/location/event`
- `bus/ha/location/state`
- `bus/command/result`
- `bus/beacon/config/result`

## MQTT Outputs

- `bus/relay/discord/status`
- `bus/relay/discord/last_event`
- `bus/relay/discord/error`
- `bus/command`
- `bus/beacon/config/set`

## Operator Commands

Slash commands are disabled unless `DISCORD_COMMAND_GUILD_ID` and at least one operator user or role allow-list are configured.

- `/bus_status`
- `/disarm confirm:true`
- `/beacon list`
- `/beacon refresh`
- `/beacon add`
- `/beacon update`
- `/beacon active`
- `/beacon remove confirm:true`

`/disarm` is fail-closed: the caller must be allow-listed, `confirm` must be true, MQTT and firmware must be online, and success is shown only after matching firmware confirmation.

## Health And Metrics

The relay exposes:

- `GET /health`
- `GET /metrics`

The Docker image runs as a non-root user and writes logs/state under `/data`.
