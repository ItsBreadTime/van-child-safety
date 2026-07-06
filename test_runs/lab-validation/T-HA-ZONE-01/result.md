# T-HA-ZONE-01 Result

Rounds passed/total: 3/3
Result: pass

Expected behavior:
Simulated Home Assistant zone events published to `bus/ha/location/event` are received by the relay. School-zone advisory messages are non-emergency and the relay error topic remains clear.

Observed behavior:
Three simulated zone events were captured: `zone_enter` school, `zone_leave` school, then `zone_enter` school. Relay status remained online with `active_distress_id=null`, `last_event.emergency=false`, and `bus/relay/discord/error` reported clear. The `zone_leave` round produced a non-emergency warning because the live roster still had Bread's beacon in `near` state.

Metrics:
- End-to-end latency:
- Relay-only latency:
- FP count:
- FN count:
- Reconnect time:

Evidence files:
- serial.log
- mqtt.log
- relay_before.jsonl
- relay_after.jsonl

Notes for Chapter 4:
T-HA-ZONE-01 passed 3/3 simulated MQTT rounds. The relay accepted HA zone-event payloads and produced non-emergency advisory output; relay status remained online and error clear. Evidence: `mqtt.log`, `events.csv`, and retained `bus/relay/discord/last_event` after the run. This does not validate Home Assistant dashboard rendering, entity templates, mobile geofencing, or real zone transitions.

Notes for Chapter 5:
The warning content depended on live beacon state at test time; this should be described as context rather than a relay failure.
