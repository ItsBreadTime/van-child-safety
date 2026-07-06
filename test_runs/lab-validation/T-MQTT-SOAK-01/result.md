# T-MQTT-SOAK-01 Result

Rounds passed/total: 1/1
Result: pass

Expected behavior:
During a passive 5-minute active-state observation, MQTT telemetry continues at the expected cadence, relay remains healthy, and no new distress appears.

Observed behavior:
- Captured 39 MQTT messages over 300 s.
- Captured 14 telemetry messages; interval median 30004.0 ms, max 74193.8 ms.
- Latest bus/state: state=active, ignition_on=True, armed=False, distress_active=False, co2_status=ok, ld2412_status=ok, ble_status=ok.
- Latest telemetry subset: temperature=27.14, humidity=57.5, co2=1108, wifi_rssi=-63, gps_valid=False.
- Relay status=online, relay error=clear.
- Distress topic messages observed: 1; inspect soak_summary.json.

Metrics:
- Telemetry interval min/median/mean/max: 0.3/30004.0/24171.1/74193.8 ms

Evidence files:
- mqtt.log
- soak_events.jsonl
- soak_summary.json
- telemetry_intervals.csv
- relay_before.jsonl

Notes for Chapter 4:
Passive short soak; useful for reporting observed MQTT cadence and active-state live-system stability. It does not exercise arming, distress generation, reconnect recovery, operator commands, or physical detection triggers.

Notes for Chapter 5:
Use `T-SCD4X-PASSIVE-01` or a longer controlled CO2 run for stronger CO2 freshness claims; this soak was not a CO2-specific stimulus test.
