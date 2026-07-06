# T-GPS-01 Result

Rounds passed/total: 0/1
Result: fail

Expected behavior:
GPS should obtain a valid fix and publish/retain usable location data during the capture window.

Observed behavior:
- Latest bus/state reports gps_valid=False.
- No location topic message arrived during the capture window; retained location may be absent or unavailable indoors.

Metrics:
- Passive capture duration: 65 s

Evidence files:
- mqtt.log
- passive_observation.json
- relay_before.jsonl

Notes for Chapter 4:
Failed the intended GPS-fix criterion: the system reported `gps_valid=false` and no retained location arrived during the capture window. Treat this as honest indoor no-fix evidence, not as GPS validation. It is useful because it records what the system reports when no fix is available.

Notes for Chapter 5:
For a stronger GPS test, move near a window or outdoors and measure time-to-fix plus retained location update.
