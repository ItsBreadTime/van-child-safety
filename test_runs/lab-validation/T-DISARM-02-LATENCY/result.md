# T-DISARM-02-LATENCY Result

Rounds passed/total: 10/10
Result: pass

Expected behavior:
Invalid disarm commands without cabin-clear confirmation are rejected by firmware and produce matching `bus/command/result` responses. The bus remains `active` and does not transition to `disarmed`. Command-result latency is measured from MQTT publish callback time to receipt of the matching result message.

Observed behavior:
Ten invalid disarm commands were published with request IDs `ch4-invalid-latency-01` through `ch4-invalid-latency-10`. All ten produced matching `bus/command/result` payloads with `ok=false`, `status=rejected`, `reason=cabin_clear_confirmation_required`, `state=active`, and `ignition_on=true`. Final passive status check showed bus state `active`, relay online, `active_distress_id=null`, and relay error clear.

Metrics:
- End-to-end latency:
- Command-result latency: min 35.6 ms, median 45.3 ms, mean 72.4 ms, max 310.5 ms
- FP count:
- FN count:
- Reconnect time:

Evidence files:
- serial.log
- latency_events.jsonl
- command_latency.csv
- latency_summary.json
- relay_before.jsonl
- relay_after.jsonl

Notes for Chapter 4:
T-DISARM-02-LATENCY passed 10/10 simulated invalid-command rounds. Firmware rejected all invalid disarm commands and did not transition to `disarmed`. Matching command-result latency was min 35.6 ms, median 45.3 ms, mean 72.4 ms, and max 310.5 ms. Evidence: `command_latency.csv`, `latency_summary.json`, and `latency_events.jsonl`.

Notes for Chapter 5:
This measures MQTT command-result responsiveness for rejected disarm commands, not the full human safety workflow or physical cabin-clear process.
