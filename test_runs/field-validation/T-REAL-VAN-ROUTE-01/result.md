# T-REAL-VAN-ROUTE-01 Result

Rounds passed/total: 2/3 observed evidence groups
Result: partial

Expected behavior:
- During a supervised real van route, the system should keep publishing MQTT
  state, telemetry, beacon, GPS/location, and relay status evidence without
  timing out.
- Beacon attendance changes should produce routine Discord relay messages such
  as child entered bus, child exited bus, child exited at school, and child
  reached home.
- Home Assistant zone/location events should produce route-context messages
  such as bus reached school or bus left school with children still near.
- Ignition transitions should be visible as `ignition_off`, `exit_grace`,
  `ignition_on`, and `active`/`armed` state changes.
- No emergency distress notification should be sent unless the system reaches a
  valid child-left-in-van trigger condition.

Observed behavior:
- MQTT collection ran from 2026-07-05 13:11:35 +07 until manual stop at
  2026-07-05 13:35:14 +07. The launchd-owned collector started at 13:13:39 +07
  and kept collecting until the requested stop.
- The MQTT log contains 1,806 lines after collection, including 36 telemetry
  samples, 182 RSSI rows, 39 event rows, 29 `bus/state` samples, 169 Home
  Assistant location-state samples, and 17 relay-status samples.
- The Discord relay status stayed online through the routine attendance part of
  the run, with `mqtt_connected=true`, `discord_connected=true`,
  `firmware_online=true`, `roster_loaded=true`, `roster_count=3`, and relay
  version `0.5.2`.
- Routine relay messages were observed through `bus/relay/discord/last_event`:
  `Crystal entered the bus.` at 13:15:29, `Bread exited the bus.` at 13:18:25,
  `Butter exited the bus.` at 13:19:41, `Crystal reached home.` at 13:20:51,
  `Bus reached school.` at 13:21:33 and 13:25:48, `Bread entered the bus.` at
  13:23:35, `Butter entered the bus.` at 13:24:17, `Crystal entered the bus.`
  at 13:25:23, and `Crystal exited at school.` plus `Butter exited at school.`
  at 13:27:50.
- One advisory relay message was also observed at 13:17:23:
  `Warning: bus left school while these children still appear near: Crystal.`
  This shows the school-zone warning path fired; whether it was correct depends
  on the physical route context at that moment.
- Ignition/state evidence was captured repeatedly: 12 `ignition_off` events,
  11 `ignition_on` events, 12 `exit_grace` state samples, 14 `active` samples,
  one `armed` event at 13:29:21, and two `distress_pending` state samples after
  arming.
- No non-empty `bus/distress/state` payload and no emergency relay
  `last_event` were captured during the run.
- Telemetry showed CO2 values from 591 ppm to 2,549 ppm, LD2412 target reported
  `True` in the telemetry samples, and the MQTT `gps_valid` field in telemetry
  stayed `False`; GPS/location evidence was still published through
  `bus/location/attributes` and Home Assistant location topics.
- After the final armed period, a network failure occurred. The relay status
  changed to degraded at 13:32:23 because `firmware_online=false` while the bus
  state remained `distress_pending`. This prevents treating the final armed
  segment as a clean completed safety-detection round, but the non-firing
  condition is attributed to network failure rather than confirmed detection
  logic failure.
- The generated `relay_before.jsonl` and `relay_after.jsonl` files are not good
  live relay snapshots for this run because the active relay was running on
  `operator@example-host`, while the helper copied the local stale relay data file.
  Live relay evidence for this run is therefore taken from MQTT
  `bus/relay/discord/status`, MQTT `bus/relay/discord/last_event`, and remote
  container logs.

Metrics:
- End-to-end latency: routine relay events appeared in MQTT at the same second
  or next second after the corresponding beacon/zone MQTT transition where the
  triggering transition was visible in the log. Physical action-to-MQTT latency
  was not measured separately.
- Relay-only latency: MQTT relay `last_event` updates were observed within the
  log timestamp resolution of the triggering beacon/zone event, typically
  0-1 s. Exact Discord client delivery latency was not measured.
- FP count: 0 emergency false positives observed. One non-emergency advisory
  warning occurred at 13:17:23 and should be checked against the route context.
- FN count: 0 obvious missed routine notifications for the clearly captured
  entered/exited/reached-home/reached-school transitions listed above. The run
  still contains `attendance_dedupe` skips in the remote relay log for repeated
  near-state chatter, so it should not be used as proof that all possible
  attendance edges are covered.
- Reconnect time: MQTT collector reconnected after a collector callback error
  and broker timeout sequence around 13:30:25-13:30:45; effective capture gap
  was about 20 s. Relay MQTT status was online again at 13:30:45.

Evidence files:
- mqtt.log
- relay_before.jsonl
- relay_after.jsonl
- events.csv
- rssi.csv
- measurements.csv
- co2.csv
- stop_collection.log
- collector_supervisor.log

Notes for Chapter 4:
- This run supports the fixed routine Discord relay path for real BLE beacon
  transitions during a supervised route: entered-bus, exited-bus,
  exited-at-school, reached-home, and bus-reached-school messages were observed after the
  notification fix.
- This run also supports that MQTT evidence collection can stay active during a
  live field route when owned by launchd/caffeinate, and it produced usable
  state, telemetry, RSSI, HA-location, and relay-status evidence.
- The final armed/distress-pending segment is only partial evidence. It shows
  arming and `distress_pending`, but not a completed distress notification or
  clean recovery, because a network failure made the firmware appear offline
  before collection stopped.
- Do not use this run alone to claim full real-van child-left-in-van validation.
  It is strongest as evidence for live route attendance notifications and
  field telemetry capture.

Notes for Chapter 5:
- Move relay snapshots to the remote relay host, or add a helper mode that can
  SSH to `operator@example-host` and copy the active container/log file. The local
  `relay_before.jsonl`/`relay_after.jsonl` snapshots were stale for this run.
- Fix the temporary collector callback signature issue seen at 13:30:25 so the
  collector does not throw on disconnect under the installed paho-mqtt version.
- Add operator notes during the run for each physical stop: who got on/off,
  expected zone, door state, fan/AC state, and whether each advisory was
  expected. That will make the Chapter 4 pass/fail judgment much stronger.
- Investigate and document the network failure after the 13:29:21 armed event
  before using the final armed segment as evidence for the safety alarm path.
