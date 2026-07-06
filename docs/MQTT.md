# MQTT Contract

Default base topic: `bus`.

## Firmware Publishes

- `bus/status`: retained `online` / `offline`
- `bus/state`: retained JSON bus state and diagnostics
- `bus/telemetry`: JSON sensor telemetry
- `bus/event`: JSON event stream
- `bus/distress/state`: retained JSON distress payload, empty retained message when clear
- `bus/beacon/<id>/state`: retained JSON beacon state
- `bus/beacon/config/state`: retained JSON roster
- `bus/beacon/config/result`: JSON roster command result
- `bus/command/result`: JSON command result

## Firmware Subscribes

- `bus/command`
- `bus/beacon/config/set`

## Relay Publishes

- `bus/relay/discord/status`
- `bus/relay/discord/last_event`
- `bus/relay/discord/error`
- `bus/command`, for optional guarded operator commands
- `bus/beacon/config/set`, for optional roster commands

## Home Assistant Publishes

- `bus/ha/location/event`
- `bus/ha/location/state`

Use broker ACLs so only trusted clients can publish command and roster topics.
