# T-DISARM-02 Result

Rounds passed/total: 3/3
Result: pass

Expected behavior:
Invalid disarm commands without cabin-clear confirmation are rejected by firmware. The command result keeps the matching `request_id`, reports `ok=false`, and `bus/state` does not enter `disarmed`.

Observed behavior:
Three invalid disarm commands were published with request IDs `ch4-invalid-disarm-01` through `ch4-invalid-disarm-03`. Each produced `bus/command/result` with `ok=false`, `status=rejected`, `reason=cabin_clear_confirmation_required`, and `state=active`. The retained bus state remained `active` with `ignition_on=true`.

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
T-DISARM-02 passed 3/3 simulated command rounds. Firmware rejected disarm without cabin-clear confirmation and did not transition to `disarmed`. Evidence: `mqtt.log` lines for `bus/command`, `bus/command/result`, and `bus/state`.

Notes for Chapter 5:
