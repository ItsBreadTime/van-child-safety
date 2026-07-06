# T-ADVISORY-01 Result

Rounds passed/total: 1/1
Result: pass

Expected behavior:
If a child beacon is near when the bus is armed and then becomes absent while
the bus remains in an armed-equivalent state, firmware should publish a
`bus/event` advisory with `event=beacon_silent_while_armed` and the beacon id.

Observed behavior:
The automated Android Bread iBeacon transmitter was started and `bread` reached
`near`. The bus was armed with `bus_empty_on`, then the transmitter was stopped.
The firmware reported `bread` as `absent` and published `bus/event` with
`event=beacon_silent_while_armed` and `beacon_id=bread`. The system was restored
with `distress_ack`, `distress_clear`, and `bus_empty_off`; final state was
`active` with `distress_active=false`.

Metrics:
- End-to-end latency:
- Relay-only latency:
- FP count:
- FN count:
- Reconnect time:

Evidence files:
- mqtt.log
- relay_before.jsonl
- relay_after.jsonl
- adb_control.log
- t_advisory_01_summary.json

Notes for Chapter 4:
T-ADVISORY-01 passed 1/1 simulated advisory round. Firmware emitted
`beacon_silent_while_armed` for Bread after a near-to-absent transition while
armed. This validates the advisory event path in the simulated setup; it is not
a real-van beacon battery-loss validation.

Notes for Chapter 5:
The scenario intentionally used `bus_empty_on` and the Android test transmitter.
The run collected advisory evidence without allowing a fresh active emergency
cycle to complete.
