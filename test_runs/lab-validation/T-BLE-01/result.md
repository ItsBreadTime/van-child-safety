# T-BLE-01 Result

Rounds passed/total: 1/1
Result: pass

Expected behavior:
When the system is armed and an active child beacon is near, firmware raises distress and the Discord relay emits one emergency alert.

Observed behavior:
- Precheck state=active, ignition_on=True, armed=False, distress_active=False.
- Bread near before arming: True.
- Armed seen: True; pending seen: True; distress active seen: True.
- Distress payload: {"id":"distress-20260704-051124","severity":"emergency","state":"active","bus_state":"distress_active","trigger":"beacon_near","detail":"crystal","person":{"id":"crystal","person_name":"Crystal","discord_channel_id":"000000000000000000","home_zone_entity":"zone.example_home"},"timestamp":"2026-07-04T05:11:24Z","evidence":{"co2":537,"co2_baseline":533,"co2_delta_since_armed":4,"co2_consecutive_rise_count":0,"ld2412_target":true,"ld2412_moving":false,"ld2412_still":true,"moving_distance":0,"still_distance":111,"moving_energy":0,"still_energy":100,"ld2412_target_duration_s":375.3261,"ld2412_frame_age_s":0.00908,"beacon_state":"near","rssi":-55},"gps":{"valid":false,"latitude":0,"longitude":0}}.
- Relay emergency event: {"channel_id":"000000000000000000","emergency":true,"content":"@everyone EMERGENCY: possible child left in bus - Crystal (`crystal`). Trigger: Beacon near.","embed_titles":["Emergency: possible child left in bus"],"timestamp":"2026-07-04T05:11:25Z"}.
- Restore: active/clear=True, retained distress clear=True.

Metrics:
- Distress initial delay configured: 120 s
- BLE near sustain configured: 2 s

Evidence files:
- mqtt.log
- ble_detection_events.jsonl
- ble_detection_summary.json
- command_log.txt
- adb_control.log
- bread_beacon_test_logcat.txt
- relay_before.jsonl

Notes for Chapter 4:
This is useful armed BLE-trigger evidence, but not identity-isolated evidence. The setup used firmware `bus_empty_on` override and an automated Android Bread iBeacon transmitter, while Crystal was also near. The observed distress identified Crystal, so report this as "an armed BLE distress and relay path fired with multiple near beacons present," not as Bread-specific validation.

Notes for Chapter 5:
Repeat in the real van with controlled beacon placement, known RSSI/distance, and no other active child beacon near if identity isolation is required.
