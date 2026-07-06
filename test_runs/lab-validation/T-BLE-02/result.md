# T-BLE-02 Result

Rounds passed/total: 0/0
Result: invalid

Expected behavior:
With Crystal absent, starting the automated Bread iBeacon transmitter and arming via `bus_empty_on` should produce a Bread-specific `distress_active` state, retained `bus/distress/state`, and real Discord relay emergency. Restoration with `bus_empty_off` should return to active/clear and clear retained distress.

Observed behavior:
This attempt was aborted by the test harness and should not be counted as a firmware failure. The firmware produced a brief `armed` state and then advanced to `distress_pending`, but the harness was matching only the latest retained state and missed the transient `armed` message. The corrected rerun is `T-BLE-03`.

- Bread near before arming: True.
- Armed state seen in `mqtt.log`: True.
- Distress pending seen in `mqtt.log`: True.
- Distress active seen: False.
- Distress payload person id: None.
- Relay emergency content: None.
- Restore active/clear: True.
- Retained distress clear: True.

Harness error:
- `TimeoutError('armed_state')`

Metrics:
- Initial distress delay should be interpreted against firmware config; this run primarily verifies identity and end-to-end trigger/relay behavior.

Evidence files:
- mqtt.log
- ble_detection_events.jsonl
- ble_detection_summary.json
- adb_control.log
- command_log.txt
- bread_beacon_test_logcat.txt
- relay_before.jsonl
- relay_after.jsonl

Notes for Chapter 4:
Do not use this attempt as a system pass/fail result. It is useful as harness evidence: retained-state polling can miss a transient `armed` state when firmware advances quickly to `distress_pending`. Use `T-BLE-03` for the corrected isolated Bread-only armed BLE child-beacon detection result.

Notes for Chapter 5:
This still does not replace a supervised real-van occupant-detection test with real placement and safety controls.
