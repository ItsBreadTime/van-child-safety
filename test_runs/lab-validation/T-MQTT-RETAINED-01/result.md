# T-MQTT-RETAINED-01 Result

Rounds passed/total: 1/1
Result: pass

Expected behavior:
Retained MQTT topics for bus state, firmware online status, relay status/error, distress state, telemetry, and beacon state are present and coherent.

Observed behavior:
- Retained snapshot captured 10 messages across 9 latest topics.
- Checks passed 14/14.
- All retained-state coherence checks passed.

Metrics:
- Coherence checks passed: 14/14

Evidence files:
- mqtt.log
- retained_snapshot.json
- retained_audit_summary.json
- relay_before.jsonl

Notes for Chapter 4:
This is a retained-state coherence snapshot across core MQTT topics. It is useful evidence that the retained state was coherent at capture time, but it should not be treated as proof that retained metadata cannot go stale after future distress, clear, relay reconnect, or roster updates.

Notes for Chapter 5:
If beacon config embeds dynamic `state`, keep it synchronized with per-beacon state topics or remove dynamic state from config snapshots to avoid stale dashboard data.
