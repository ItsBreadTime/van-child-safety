# T-REAL-VAN-OCCUPANT-01 Result

Rounds passed/total: 1/1
Result: pass

Expected behavior:
- During a supervised adult occupant presence test, the system should keep
  publishing MQTT state, telemetry, beacon, and relay evidence without timing
  out.
- After ignition-off/exit-grace completes, the bus should enter `armed`.
- If a carried beacon remains near and presence evidence remains active, the
  system should transition to `distress_active` and publish a non-empty
  `bus/distress/state` emergency payload.
- The Discord relay should send one emergency notification for the distress id
  and then dedupe repeats until the configured repeat interval.
- The test must remain supervised and physically safe: unlocked exit path,
  reachable phone, and an outside observer able to stop the test.

Observed behavior:
- Collection ran from 2026-07-05 13:39:15 +07 until manual stop at
  2026-07-05 13:50:11 +07 under launchd/caffeinate. The MQTT log contains
  1,567 lines after collection.
- The run produced 19 telemetry samples, 97 RSSI rows, 14 event rows, 10
  `bus/state` samples, and 22 remote relay log lines for the scenario window.
- The relay recovered from the previous run's network-failure/degraded state:
  `firmware_online=true` appeared at 13:40:39, followed by `bus_state=active`
  at 13:40:40.
- Routine route/attendance relay messages were observed before the occupant
  alarm round: `Bread exited at school.` at 13:43:01, `Bread entered the bus.`
  at 13:43:27, `Warning: bus left school while these children still appear
  near: Bread.` at 13:44:00, and `Bus reached school.` at 13:44:05.
- The fresh occupant alarm round began with `ignition_off`/`exit_grace` at
  13:44:07. The bus then published `armed` at 13:46:07.
- At 13:48:07, the firmware published `bus/state` as `distress_active` and
  emitted a non-empty `bus/distress/state` payload with id
  `distress-20260705-064807`, severity `emergency`, trigger `beacon_near`,
  detail `bread`, and person `Bread`.
- The distress evidence at 13:48:07 included CO2 849 ppm, baseline 544 ppm,
  delta since armed 305 ppm, CO2 consecutive rise count 3, LD2412 target true,
  still target distance 100, beacon state near, and RSSI -55.
- The remote relay log shows the emergency was received and queued at 13:48:07
  UTC, then sent successfully to the emergency channel at 13:48:08 UTC:
  "@everyone EMERGENCY: possible child left in bus - Bread (`bread`). Trigger:
  Beacon near."
- Repeated retained/periodic distress payloads at 13:49:07 and 13:50:07 used
  the same distress id and were deduped by the relay with remaining repeat
  windows of 240 s and 180 s.
- The MQTT `bus/relay/discord/status` record at 13:48:08 reported
  `status=online`, `mqtt_connected=true`, `discord_connected=true`,
  `firmware_online=true`, `bus_state=distress_active`, and
  `active_distress_id=distress-20260705-064807`.
- No operator acknowledgement or disarm flow was run in this scenario.
- The run notes state the intended safety boundary: unlocked exit path, do not
  lock occupant in, stop for heat/discomfort/network uncertainty/loss of
  observer/blocked exit. The public evidence folder includes sanitized logs,
  but not screenshots, photos, serial log, or detailed physical observations,
  so the report should describe the physical safety setup from operator notes
  rather than from captured files alone.

Metrics:
- End-to-end latency: from `armed` at 13:46:07 to `distress_active` at
  13:48:07 was 120 s.
- Relay-only latency: distress MQTT at 13:48:07 to remote relay
  `discord_send_ok` at 13:48:08 was about 1 s.
- FP count: 0 emergency false positives observed during the fresh occupant
  alarm round.
- FN count: 0 for the intended single supervised occupant round; the expected
  emergency was produced and relayed.
- Reconnect time: no MQTT collector reconnect/exception was observed during
  this scenario. The relay came online from the previous degraded state at
  13:40:39 before the alarm round.
- CO2: telemetry min/average/max during the scenario was 542 / 668.3 / 916 ppm.
  At first distress, CO2 was 849 ppm with 305 ppm delta since armed.
- Presence evidence: LD2412 target became true during the run and was true in
  the distress evidence. Bread beacon RSSI ranged from -98 to -51 dBm, with the
  distress trigger using beacon state near at -55 dBm.

Evidence files:
- mqtt.log
- remote_relay.log
- relay_before.jsonl
- relay_after.jsonl
- events.csv
- rssi.csv
- measurements.csv
- co2.csv
- stop_collection.log
- collector_supervisor.log

Notes for Chapter 4:
- This run supports the real-van supervised adult occupant alarm path: after
  arming, the system detected the carried beacon/presence condition, entered
  `distress_active`, published a structured emergency distress payload, and the
  relay delivered the emergency Discord notification.
- The strongest timing claim from this run is a 120 s armed-to-distress
  interval and about 1 s MQTT-distress-to-relay-send interval.
- This is a controlled adult-occupant safety test, not an unattended-child or
  heat-risk test. The Chapter 4 wording should preserve that boundary.
- Use `remote_relay.log` and MQTT `bus/relay/discord/*` topics as relay
  evidence. The helper's local `relay_before.jsonl` and `relay_after.jsonl`
  snapshots are not authoritative for the remote relay host.

Notes for Chapter 5:
- Add a remote-relay snapshot mode to `scripts/chapter4_test_runner.py` so
  future field runs capture the active relay log on `operator@example-host`
  directly instead of copying stale local relay files.
- Capture photos/screenshots and physical setup notes during the next run:
  door/window state, fan/AC state, observer identity, sensor placement, beacon
  placement, and occupant stop/abort condition.
- Add an acknowledgement/disarm follow-up round after an emergency scenario so
  Chapter 4 can show the complete operator response workflow, not only alert
  generation.
