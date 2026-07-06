# T-SCD4X-PASSIVE-01 Result

Rounds passed/total: 1/1
Result: pass

Expected behavior:
SCD4x-derived CO2 telemetry is fresh and plausible while the bus remains active/clear. This is not a CO2-rise trigger test.

Observed behavior:
- Captured 4 telemetry frames over 120 s.
- CO2 range: 814-817 ppm.
- Latest `co2_status`: ok.
- Latest bus state: active, ignition_on=True, armed=False, distress_active=False.
- Non-empty distress messages observed: 0.
- Relay status: online; relay error: clear.

Metrics:
- CO2 plausible range check: True.
- CO2 freshness/status check: True.

Evidence files:
- mqtt.log
- scd4x_events.jsonl
- scd4x_passive_summary.json
- co2_samples.csv
- relay_before.jsonl
- relay_after.jsonl

Notes for Chapter 4:
This test supports the narrow claim that SCD4x/CO2 telemetry was fresh and plausible in the simulated/desk environment during this short passive window. It does not validate the `co2_rise` distress trigger, cabin airflow behavior, threshold tuning, or emergency escalation.

Notes for Chapter 5:
A controlled CO2-rise test should record cabin setup, temperature, baseline, positive 3-minute trend, and the threshold crossing before claiming CO2-trigger detection.
