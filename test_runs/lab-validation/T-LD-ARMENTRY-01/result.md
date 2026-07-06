# T-LD-ARMENTRY-01 Result

Rounds passed/total: 1/1
Result: pass

Expected behavior:
With Bread and Crystal absent and an LD2412 target already present at arm time, arming via `bus_empty_on` should use the extended arm-entry sustain path, then produce `presence_detected`, a real Discord emergency, and restore cleanly with `bus_empty_off`.

Observed behavior:
- Bread absent precheck: absent.
- Crystal absent precheck: absent.
- LD target present before arming: True.
- Armed state seen: True.
- Distress pending seen: True.
- Distress active seen: True.
- Distress trigger: presence_detected.
- Evidence target/distance/energy: target=True, still_distance=101, still_energy=100, target_duration_s=4339.537177.
- Relay emergency content: @everyone EMERGENCY: possible child left in bus - unknown child (no active beacon currently near). Trigger: Presence detected..
- Restore active/clear: True.
- Retained distress clear: True.
- Post-run retained relay status caveat: `bus/relay/discord/status` remained online/error-clear but retained the prior `active_distress_id` until the relay next publishes status; firmware `bus/state` and retained `bus/distress/state` were clear.

Metrics:
- This is the arm-entry sustain path because the target did not clear during `T-LD2412-PASSIVE-01`.
- The result is desk-proxy evidence for firmware LD2412 trigger behavior, not final real-cabin occupant validation.

Evidence files:
- mqtt.log
- ld_arm_entry_events.jsonl
- ld_arm_entry_summary.json
- command_log.txt
- relay_before.jsonl
- relay_after.jsonl

Notes for Chapter 4:
This test shows that the LD2412/mmWave `presence_detected` firmware and relay path can fire in a simulated desk environment when a target is already present at arming. It should be reported as desk-proxy trigger-path evidence only. It does not prove target clear/re-detect behavior, real-seat coverage, or real-cabin occupant detection.

Notes for Chapter 5:
Because the desk target could not be cleared, investigate sensor placement/reflections and repeat normal enter-after-armed behavior in the van if possible. The relay should also publish a fresh status after distress clear so retained `active_distress_id` does not look stale after restoration.
