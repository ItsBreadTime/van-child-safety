# T-DISARM-01 Result

Rounds passed/total: 1/1
Result: pass

Expected behavior:
After an active distress exists, `distress_ack` should move the firmware to
acked state. A guarded JSON disarm command with `confirm_cabin_clear=true`
should then return a matching `bus/command/result` with `ok=true` and move
`bus/state` to `disarmed`. The test should end with the system restored to
`active` and retained `bus/distress/state` cleared.

Observed behavior:
The automated workflow first restored the bus to active/clear, then published
`bus_empty_on` to enter armed state and `distress_test` to create an intentional
presence distress. The firmware reached `distress_pending` and then
`distress_active` after the configured delay. `distress_ack` moved the state to
`acked`. The guarded disarm command with request id
`ch4-disarm-01-1783167042` returned `ok=true`, `status=ok`,
`reason=disarmed`, and `state=disarmed`; `bus/state` also reported
`state=disarmed`. The script restored with `bus_empty_off` and
`distress_clear`; final `bus/state` was `active`, `distress_active=false`, and
the retained distress payload was empty.

Metrics:
- End-to-end latency:
- Relay-only latency:
- FP count:
- FN count:
- Reconnect time:
  - Distress pending to active used the configured `DISTRESS_INITIAL_DELAY_S`
    window (about 120 s).

Evidence files:
- mqtt.log
- relay_before.jsonl
- relay_after.jsonl
- command_log.txt
- disarm_workflow_events.jsonl
- disarm_workflow_summary.json

Notes for Chapter 4:
T-DISARM-01 passed 1/1 simulated guarded-disarm workflow round. The test
observed intentional distress, acknowledgement, matching guarded disarm command
result, actual `bus/state=disarmed`, and clean restoration to active/clear.
This is command/state-machine evidence, not a substitute for a human cabin
inspection.

Notes for Chapter 5:
The command result contract reports `status=ok` and `reason=disarmed` for
successful guarded disarm. Relay retained status again lagged behind firmware
state in this run, so relay status freshness after distress clear remains a
small follow-up item.
