# T-RELAY-02 Result

Rounds passed/total: 3/3
Result: pass

Expected behavior:
Repeated emergency distress payloads with the same distress ID should not produce repeated emergency Discord sends inside the relay repeat interval.

Observed behavior:
Three `bus/distress/state` active payloads were published with the same ID `ch4-relay-02-dedupe`. The first publish produced an emergency relay `last_event`; the second and third same-ID publishes were captured in `mqtt.log` but did not produce another emergency `last_event` inside the repeat window.

Metrics:
- End-to-end latency:
- Relay-only latency: first same-ID distress produced relay output within the capture window; repeats produced no duplicate emergency output
- FP count:
- FN count:
- Reconnect time:

Evidence files:
- serial.log
- mqtt.log
- relay_before.jsonl
- relay_after.jsonl

Notes for Chapter 4:
T-RELAY-02 passed 3/3 simulated dedupe rounds. The relay sent the first emergency for `ch4-relay-02-dedupe` and suppressed repeated same-ID distress publishes within the repeat interval. Evidence: `mqtt.log` lines for the three distress payloads and the single matching emergency `last_event`.

Notes for Chapter 5:
Unrelated live beacon transition messages occurred during the capture window; they should not be counted as distress duplicates.
