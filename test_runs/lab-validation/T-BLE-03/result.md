# T-BLE-03 Result

Rounds passed/total: 1/1
Result: pass

Expected behavior:
With Crystal absent, starting the automated Bread iBeacon transmitter and arming via `bus_empty_on` should produce a Bread-specific `distress_active` state, retained `bus/distress/state`, and real Discord relay emergency. Restoration with `bus_empty_off` should return to active/clear and clear retained distress.

Observed behavior:
- Crystal absent precheck: absent.
- Bread near before arming: True.
- Armed state seen: True.
- Distress pending seen: True.
- Distress active seen: True.
- Distress payload person id: bread.
- Relay emergency content: @everyone EMERGENCY: possible child left in bus - Bread (`bread`). Trigger: Beacon near..
- Restore active/clear: True.
- Retained distress clear: True.

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
This is the strongest BLE result in this run: with Crystal absent, the Android Bread transmitter plus `bus_empty_on` override produced Bread-specific `distress_active`, retained distress, real Discord emergency output, and clean restoration. Report it as simulated end-to-end BLE trigger evidence, not as real-van RF placement validation.

Notes for Chapter 5:
Repeat with real van placement, measured beacon position/RSSI, and documented supervision before claiming child-left-in-van field performance.
