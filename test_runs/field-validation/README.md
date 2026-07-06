# Chapter 4 Real-Van Evidence Run

Created at: `2026-07-05T13:10:41+07:00`
Git commit: `795deef`
Git branch: `main`
Run type: supervised real-van route and adult-occupant evidence.

## Start Here

Use [chapter4_summary.md](chapter4_summary.md) for the short scenario table.
This run is the strongest current real-van evidence in the archive, but it still
needs careful wording: the emergency scenario used a supervised adult occupant,
not an unattended child or heat-risk setup.

## Scenario Summary

| Scenario | Result | Best claim to make | Boundary |
|---|---|---|---|
| [T-REAL-VAN-OCCUPANT-01](T-REAL-VAN-OCCUPANT-01/) | pass, 1/1 | Supervised adult-occupant real-van test reached `distress_active`, published structured emergency MQTT evidence, and the remote Discord relay sent the emergency notification. | Not an unattended-child, heat-risk, or full operator-response test; no ack/disarm round was performed. |
| [T-REAL-VAN-ROUTE-01](T-REAL-VAN-ROUTE-01/) | partial, 2/3 evidence groups | Real BLE/route notifications and field telemetry capture worked during the supervised route. | Final armed/distress-pending segment was interrupted by network failure, so it is not a completed safety-detection round. |

## Key Evidence

- `T-REAL-VAN-OCCUPANT-01` captured a 120 s armed-to-distress interval and
  about 1 s MQTT-distress-to-remote-relay-send interval.
- `T-REAL-VAN-OCCUPANT-01` recorded the first distress as
  `distress-20260705-064807`, trigger `beacon_near`, detail `bread`, and person
  `Bread`.
- `T-REAL-VAN-ROUTE-01` observed routine Discord messages for entered-bus,
  exited-bus, exited-at-school, reached-home, and bus-reached-school events
  after the notification fix.
- Remote relay evidence is stronger than the local `relay_before.jsonl` and
  `relay_after.jsonl` snapshots for this run, because the active relay was on
  the remote host. Prefer `remote_relay.log` where present plus MQTT
  `bus/relay/discord/*` topics.

## Evidence Files

- Root snapshots: `manifest.json`, `config_snapshot.txt`, `git_commit.txt`,
  `git_branch.txt`, `git_status.txt`.
- Scenario logs: `mqtt.log`, `remote_relay.log` where available,
  `collector_supervisor.log`, `stop_collection.log`, launchd/caffeinate logs,
  and relay snapshots.
- Scenario data: `events.csv`, `rssi.csv`, `measurements.csv`, `co2.csv`.
- Screenshots are omitted from the public archive; the relevant notification
  and state evidence is preserved in sanitized logs and scenario result files.

The original private capture path and regeneration command were redacted; this
public export keeps the sanitized run under `test_runs/field-validation/`.
