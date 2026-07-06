# Chapter 4 Scenario Summary

Generated at: 2026-07-05T13:52:03+07:00

## Reader Summary

This is the strongest current real-van evidence folder. It supports two
different claims:

- `T-REAL-VAN-OCCUPANT-01` supports the supervised real-van emergency path:
  armed state, occupant/beacon evidence, `distress_active`, structured MQTT
  distress payload, and remote Discord emergency delivery were all observed.
- `T-REAL-VAN-ROUTE-01` supports real route/attendance notifications and field
  telemetry capture, but the final armed segment is only partial because a
  network failure interrupted the run before completed distress evidence was
  captured.

Do not combine these into a single broad claim that the whole unattended-child
system is fully validated. The safe wording is: real-van route notifications
were field-observed, and a supervised adult-occupant emergency path was
successfully demonstrated.

## Scenario Table

| Scenario ID | Environment | Setup | Rounds passed/total | Result | Chapter 4 notes |
|---|---|---|---:|---|---|
| T-REAL-VAN-OCCUPANT-01 | real van supervised adult occupant presence test | Live MQTT bus/# capture plus remote Discord relay evidence; supervised adult occupant; do not lock occupant in; maintain exit path and outside observer | 1/1 | pass | - This run supports the real-van supervised adult occupant alarm path: after arming, the system detected the carried beacon/presence condition, entered `distress_active`, published a structured emergency distress payload, and the relay delivered the emergency Discord notification. - The strongest timing claim from this run is a 120 s armed-to-distress interval and about 1 s MQTT-distress-to-relay-send interval. - This is a controlled adult-occupant safety test, not an unattended-child or heat-risk test. The Chapter 4 wording should preserve that boundary. - Use `remote_relay.log` and MQTT `bus/relay/discord/*` topics as relay evidence. The helper's local `relay_before.jsonl` and `relay_after.jsonl` snapshots are not authoritative for the remote relay host. |
| T-REAL-VAN-ROUTE-01 | real van route / drop-off and pick-up field test | Live MQTT bus/# capture plus remote Discord relay evidence; relay notifications fixed in commit 795deef; broker 192.0.2.10; relay host 192.0.2.10 | 2/3 observed evidence groups | partial | - This run supports the fixed routine Discord relay path for real BLE beacon transitions during a supervised route: entered-bus, exited-bus, exited-at-school, reached-home, and bus-reached-school messages were observed after the notification fix. - This run also supports that MQTT evidence collection can stay active during a live field route when owned by launchd/caffeinate, and it produced usable state, telemetry, RSSI, HA-location, and relay-status evidence. - The final armed/distress-pending segment is only partial evidence. It shows arming and `distress_pending`, but not a completed distress notification or clean recovery, because a network failure made the firmware appear offline before collection stopped. - Do not use this run alone to claim full real-van child-left-in-van validation. It is strongest as evidence for live route attendance notifications and field telemetry capture. |
