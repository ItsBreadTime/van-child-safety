# T-SHT4X-01 Result

Rounds passed/total: 1/1
Result: pass

Expected behavior:
SHT4x-derived temperature/RH telemetry appears in MQTT and values are reasonable for the room.

Observed behavior:
- Captured 3 bus/telemetry message(s).
- Latest telemetry keys: altitude, beacons, bus_armed, bus_empty, bus_state, co2, co2_baseline, co2_delta_3min, co2_delta_since_armed, course, detection_distance, gps_valid, humidity, ignition_on, latitude, ld2412_light, ld2412_moving, ld2412_still, ld2412_target, longitude, moving_distance, moving_energy, satellites, speed, still_distance, still_energy, temperature, wifi_rssi.
- Latest temperature/RH: 27.13-27.15 C / 58.3-58.5%.

Metrics:
- Passive capture duration: 65 s

Evidence files:
- mqtt.log
- passive_observation.json
- relay_before.jsonl

Notes for Chapter 4:
Passive temperature/RH telemetry presence and plausibility check only. No heat, humidity, enclosure, or van environmental response was tested.

Notes for Chapter 5:
If telemetry lacks SHT4x fields, expose temperature/RH explicitly in `bus/telemetry` or record serial logs for this scenario.
