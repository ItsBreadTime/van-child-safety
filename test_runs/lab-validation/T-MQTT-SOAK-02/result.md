# T-MQTT-SOAK-02 Result

Rounds passed/total: 1/1
Result: pass

Expected behavior:
During a passive 15-minute active-state observation, MQTT telemetry continues at the expected cadence, relay remains healthy, and no new distress appears.

Observed behavior:
- Captured 40 MQTT messages over 900.0 s.
- Captured 30 telemetry messages; interval min/median/mean/max 30000.0/30000.0/30000.0/30000.0 ms.
- Latest bus/state: {"state": "active", "ignition_on": true, "armed": false, "exit_grace_remaining_s": 0, "distress_active": false, "distress_acknowledged": false, "gps_valid": false, "co2_status": "ok", "ld2412_status": "ok", "ble_status": "ok", "ignition_source": "espnow", "ignition_source_status": "overridden", "ignition_espnow": {"last_heartbeat_age_s": 3.662144, "remote_uptime_s": 382, "remote_battery_mv": 0, "seq": 191, "missed_seq": 73}, "timestamp": "2026-07-04T05:11:24Z"}.
- Latest telemetry: {"temperature": 28.68, "humidity": 46.6, "co2": 734, "co2_baseline": 533, "co2_delta_since_armed": 201, "ld2412_target": true, "ld2412_moving": false, "ld2412_still": true, "moving_distance": 0, "still_distance": 105, "moving_energy": 0, "still_energy": 100, "detection_distance": 0, "ld2412_light": 0, "latitude": 0, "longitude": 0, "course": 0, "altitude": 0, "speed": 0, "satellites": 0, "gps_valid": false, "co2_delta_3min": 16, "wifi_rssi": -51, "bus_empty": false, "ignition_on": true, "bus_armed": false, "bus_state": "active", "beacons": [{"id": "crystal", "name": "crystal", "person_name": "Crystal", "state": "near", "rssi": -46, "rssi_threshold": -75, "discord_channel_id": "000000000000000000", "home_zone_entity": "zone.example_home"}, {"id": "bread", "name": "bread", "person_name": "Bread", "state": "absent", "rssi": -57, "rssi_threshold": -75, "discord_channel_id": "000000000000000000", "home_zone_entity": "zone.example_home"}]}.
- Relay status: {"status": "online", "mqtt_connected": true, "discord_connected": true, "firmware_online": true, "roster_loaded": true, "roster_count": 2, "bus_state": "active", "active_distress_id": null, "operator_commands_enabled": false, "operator_gateway_ready": false, "version": "0.5.1", "timestamp": "2026-07-04T05:35:22Z"}.
- Relay error: {"status": "clear", "message": "clear", "timestamp": "2026-07-04T05:11:38Z"}.
- Distress topic messages observed: 0; non-empty distress messages: 0.

Metrics:
- Telemetry interval min/median/mean/max 30000.0/30000.0/30000.0/30000.0 ms
- State unexpected count: 0

Evidence files:
- mqtt.log
- soak_events.jsonl
- soak_summary.json
- telemetry_intervals.csv
- relay_before.jsonl
- relay_after.jsonl

Notes for Chapter 4:
Passive extended soak; useful for reporting telemetry cadence, retained live-state stability, relay health, and passive SCD4x/CO2 plus LD2412/mmWave telemetry visibility. This does not prove CO2-rise or mmWave-presence distress trigger correctness because the bus remained active (`ignition_on=true`, `bus_armed=false`) and no controlled sensor stimulus was applied. Also report that retained relay status showed `operator_commands_enabled=false` and `operator_gateway_ready=false`, so this run should not be used as operator-command readiness evidence.

Notes for Chapter 5:
Review any interval outliers or duplicate bursts before treating cadence as deterministic.
