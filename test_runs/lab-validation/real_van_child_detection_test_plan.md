# Real Van Child-Detection Test Plan

## Purpose

This run has not yet validated the core end-to-end behavior: detecting an
occupant left inside a real van after ignition-off arming. The supporting
systems have been tested, but Chapter 4 still needs real-van evidence for the
detection paths themselves: BLE beacon, LD2412 mmWave presence, CO2 rise, and
the integrated distress/relay cycle.

This plan uses a supervised adult volunteer and/or beacon/dummy setup. Do not
use an unattended child, do not lock anyone inside, and do not create a heat-risk
condition.

## Roles

- Safety supervisor: outside the van, has key, watches the person, can open the
  door immediately.
- Test subject: adult volunteer only, seated in the target seat.
- Operator: runs MQTT/serial/Discord evidence capture and timestamps actions.
- Observer: records door/window/fan/AC state, cabin temperature, and photos.

One person may fill operator/observer roles, but the safety supervisor should be
dedicated during any occupied-van round.

## Safety Rules

- Use a shaded/cool location; avoid midday heat.
- Doors remain unlocked. Safety supervisor stays beside the van.
- Subject keeps phone/voice contact with supervisor.
- Abort immediately if subject is uncomfortable, cabin temperature rises beyond
  the chosen limit, power behaves unexpectedly, or the system enters an
  unexpected state.
- Suggested conservative abort thresholds:
  - cabin temperature >= 30 C, or lower if subject feels warm;
  - any dizziness, shortness of breath, anxiety, or discomfort;
  - round duration exceeds planned maximum;
  - Discord/relay/emergency state cannot be cleared after the round.
- Keep each occupied round short. BLE and LD2412 rounds should complete within
  seconds to tens of seconds after arming. CO2 rounds may need longer, so use
  extra caution and stop as soon as the trigger/evidence is sufficient.

## Required Evidence

For every scenario:

- `mqtt.log` covering `bus/state`, `bus/telemetry`, `bus/event`,
  `bus/distress/state`, `bus/beacon/+/state`, relay topics.
- Serial log if available.
- Relay before/after JSONL snapshots.
- Discord screenshot for emergency and clear/ack/disarm flow if real Discord is
  used.
- Home Assistant screenshot if dashboard evidence is needed.
- Photo of setup: sensor placement, subject seat, beacon placement.
- Manual notes: weather, ambient/cabin temperature, window state, door state,
  fan/AC state, ignition remote action time, subject position.
- Metrics: time from ignition-off/armed to distress, trigger source, CO2/RSSI
  trend, LD2412 distance/energy/target state, relay notification latency.

## Preflight

1. Confirm relay online and error clear.
2. Confirm `bus/state` starts as:
   - `state=active`
   - `ignition_on=true`
   - `armed=false`
   - `distress_active=false`
   - sensor statuses acceptable.
3. Confirm the operator can clear the test condition by turning ignition back on
   and, only after explicit confirmation, using the normal ack/disarm workflow.
4. Start evidence capture before changing ignition state.
5. Confirm real Discord alerts are acceptable for this test round, or redirect
   to a test channel if available.

## Scenario 1: Empty Van False-Positive Baseline

IDs covered: `T-CO2-FP-02`, `T-LD-FP-03`, `T-BLE-FP-03`, optionally
`T-BUSCLEAR-01`.

Procedure:

1. Remove child beacons or keep them far enough to be absent.
2. Confirm no person is inside and LD2412 target is false if practical.
3. Turn ignition remote off and allow system to enter `armed`.
4. Observe for the planned window, for example 3-5 minutes.
5. Expected: no distress. If implemented, `bus_clear` may appear when cabin is
   clear.
6. Turn ignition back on and record final state.

Pass criteria:

- No `distress_active=true`.
- No emergency Discord notification.
- Sensor/relay state remains coherent.

## Scenario 2: BLE Beacon Detection In Van

IDs covered: `T-BLE-02`.

Procedure:

1. Place Bread beacon/phone on the target seat where a child bag/tag would be.
2. No person is required for this round.
3. Confirm `bus/beacon/bread/state` becomes `near`.
4. Turn ignition remote off and allow the system to arm.
5. Expected: after BLE near sustain, distress triggers with beacon evidence.
6. Capture Discord alert and `bus/distress/state`.
7. Turn ignition back on. Then perform ack/disarm/clear only if explicitly
   confirmed safe and needed for the test record.

Pass criteria:

- Distress occurs after arming.
- Trigger/evidence references beacon/Bread or near active beacon state.
- Relay sends one emergency, not repeated duplicates.

## Scenario 3: LD2412 Occupant Detection In Van

IDs covered: `T-LD-NORMAL-02` and/or `T-LD-ARMENTRY-02`.

Normal-path procedure:

1. Start with van empty during arming if possible.
2. Turn ignition remote off and allow system to enter `armed`.
3. Adult subject enters and sits in the target seat, staying still.
4. Expected: LD2412 target passes distance/energy/sustain gate and triggers
   distress.

Arm-entry procedure:

1. Adult subject is seated before ignition-off.
2. Turn ignition remote off and allow system to enter `armed`.
3. Expected: arm-entry path uses extended sustain and then triggers if gates
   pass.

Pass criteria:

- `ld2412_target=true` and distance/energy evidence appear in telemetry.
- Distress occurs only after the configured sustain/gate behavior.
- No immediate ungated arm-entry trigger.

## Scenario 4: CO2 Rise Detection In Van

IDs covered: `T-CO2-03`.

Procedure:

1. Use only an adult volunteer, supervised continuously.
2. Record baseline CO2, door/window/fan/AC state, and cabin temperature.
3. Turn ignition remote off and allow system to arm.
4. Adult remains seated and breathing normally; stop as soon as trigger evidence
   is sufficient or abort threshold is reached.
5. Expected: CO2 rises above threshold with positive trend and triggers
   distress.

Pass criteria:

- `co2`, `co2_delta_since_armed`, and `co2_delta_3min` support the trigger.
- Distress occurs after freshness/trend/threshold conditions, not while CO2 is
  stale.
- Round stays within safety limits.

## Scenario 5: Integrated Realistic Round

IDs covered: `T-MULTI-01`.

Procedure:

1. Adult subject sits in the target seat with Bread beacon/phone near them.
2. Optional second beacon can be placed on another seat for multi-child evidence.
3. Turn ignition remote off and allow system to arm.
4. Observe which signal triggers first: BLE, LD2412, or CO2.
5. Capture one full distress cycle and relay behavior.
6. Confirm dedupe: no repeated emergency spam for the same distress id within
   the repeat interval.

Pass criteria:

- Exactly one distress cycle for the same stuck-child event.
- Emergency message contains useful evidence: bus state, trigger, beacon/child
  identity if available, LD2412/CO2 context, timestamp.
- Ack/disarm/clear workflow restores safe state after supervised cabin
  inspection.

## Suggested Order

1. Empty van false-positive baseline.
2. BLE-only beacon in van.
3. LD2412 normal path with adult subject entering after armed.
4. LD2412 arm-entry path with adult subject present before arming.
5. CO2 rise in van only if temperature and supervision are safe.
6. Integrated multi-signal round.

## Minimum Reportable Set

If time is limited, the minimum real-van evidence should be:

1. Empty van no false distress.
2. One occupied-van trigger using adult volunteer plus beacon.
3. One full Discord emergency evidence screenshot.
4. One safe recovery workflow showing final `bus/state` no active distress.

This would honestly support the claim that the prototype can detect an
occupant-like condition in a real van, while the report should still state that
testing did not involve an unattended child and did not create a heat-risk
scenario.
