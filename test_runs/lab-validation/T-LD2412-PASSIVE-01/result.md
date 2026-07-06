# T-LD2412-PASSIVE-01 Result

Rounds passed/total: 0/1
Result: fail

Expected behavior:
While the bus remains active/clear, LD2412/mmWave telemetry should respond to operator movement and the target should be controllable: clear to `ld2412_target=false` when the operator leaves the sensor field, then return to `ld2412_target=true` when the operator re-enters.

Observed behavior:
- Captured 7 telemetry frames.
- Latest `ld2412_status`: ok.
- Target false seen: False.
- Target true after false seen: False.
- Still distance range: 102..400 cm.
- Still energy range: 66..100.
- Moving energy range: 0..0.
- Active/clear throughout latest state: True.
- Non-empty distress messages observed: 0.

Metrics:
- Target controllability: did-not-clear.
- Desk movement response detected: True.

Evidence files:
- mqtt.log
- ld2412_passive_events.jsonl
- ld2412_passive_summary.json
- ld2412_samples.csv
- operator_phases.txt
- relay_before.jsonl
- relay_after.jsonl

Notes for Chapter 4:
Failed the intended controllability criterion because the target never cleared to `ld2412_target=false`. This is a valuable negative result: distance and energy changed, but the binary target state stayed latched. Do not claim LD2412 placement is validated from this run; use it to justify placement/reflection retesting.

Notes for Chapter 5:
Desk mmWave geometry may keep a target latched due to reflections or nearby objects; use real cabin placement for final occupant-detection claims.
