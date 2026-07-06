# T-RELAY-03 Result

Rounds passed/total: 3/3
Result: pass

Expected behavior:
After a distress is acknowledged, repeated active distress payloads for the same ID should not re-send an emergency mention.

Observed behavior:
The first active payload for `ch4-relay-03-ack` produced an emergency relay `last_event`. The following `state=acked` payload was captured. A later repeated active payload for the same ID was captured but did not produce a second emergency relay `last_event`.

Metrics:
- End-to-end latency:
- Relay-only latency: initial active distress produced relay output within the capture window; post-ack repeat produced no duplicate emergency output
- FP count:
- FN count:
- Reconnect time:

Evidence files:
- serial.log
- mqtt.log
- relay_before.jsonl
- relay_after.jsonl

Notes for Chapter 4:
T-RELAY-03 passed 3/3 simulated acknowledgement-policy rounds. The relay sent the initial emergency notification, accepted the acked distress payload, and suppressed the repeated active distress payload after acknowledgement. Evidence: `mqtt.log` lines for active, acked, repeated active, and the single emergency `last_event`.

Notes for Chapter 5:
This test supports relay mention suppression for an acknowledged distress payload; full operator workflow remains covered by guarded disarm tests with firmware command results.
