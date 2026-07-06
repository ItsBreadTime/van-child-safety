# Remaining Chapter 4 Tests Summary

Generated after completed run evidence in `chapter4_summary.md`.

## Covered Or Partly Covered In This Run

The list below is evidence coverage, not final requirement closure. A scenario is
"covered" only for the condition described here. Simulated MQTT publishes,
`bus_empty_on` override, Android BLE transmission, desk mmWave setup, and
passive telemetry should keep those qualifiers when copied into the report.

- `T-DISARM-02` invalid guarded-disarm rejection
- `T-DISARM-02-LATENCY` invalid command-result latency
- `T-HA-ZONE-01` simulated Home Assistant zone events
- `T-RELAY-01`, `T-RELAY-02`, `T-RELAY-03` Discord relay severity, dedupe, ack behavior
- `T-IGN-ESPNOW-01` remote unplug/replug stale recovery
- `T-IGN-RAPID-01` rapid unplug/replug no false transition
- `T-BLE-BREAD-LATENCY` automated Bread iBeacon latency
- `T-BLE-FP-ACTIVE-01` active-state BLE toggle no arming/no distress
- `T-BLE-01` armed BLE child-beacon trigger with multiple near beacons present
- `T-BLE-03` isolated Bread-only armed BLE child-beacon trigger, real Discord emergency, restored active/clear
- `T-MQTT-SOAK-01` 5-minute MQTT soak/jitter observation
- `T-MQTT-SOAK-02` 15-minute MQTT soak/jitter observation
- `T-SCD4X-PASSIVE-01` fresh passive SCD4x/CO2 telemetry and plausibility observation
- `T-LD2412-PASSIVE-01` active-state LD2412/mmWave movement-response observation; failed target-clear controllability criterion
- `T-LD-ARMENTRY-01` desk-proxy LD2412/mmWave `presence_detected` trigger with target present at arming
- `T-SHT4X-01` passive temperature/RH telemetry
- `T-GPS-01` indoor GPS fix attempt; failed because no valid fix/location was obtained

## Claims This Run Supports

- The relay and MQTT command/reporting paths behaved correctly for the simulated
  payloads that were published.
- Invalid disarm without cabin-clear confirmation was rejected by firmware and
  did not move the bus into `disarmed`.
- BLE child-beacon distress can be produced in an armed simulated setup using
  Android transmitter input and `bus_empty_on` override.
- LD2412/mmWave `presence_detected` can produce distress in a desk-proxy
  target-present-at-arming setup.
- Passive SCD4x, SHT4x, LD2412, beacon, relay, and MQTT telemetry were visible
  during the observed windows.

## Claims This Run Does Not Support Yet

- That the system detects an occupant left in the real van after ignition-off
  arming.
- That LD2412/mmWave placement can reliably clear and re-detect a seated person
  in the cabin; the passive LD2412 attempt failed the clear criterion.
- That CO2-rise distress works under controlled cabin airflow and temperature
  conditions.
- That GPS location is available; the indoor GPS attempt failed.
- That Home Assistant dashboards and entities are correct; the HA evidence here
  is simulated MQTT zone-event handling, not a rendered dashboard audit.
- That operator command handling is fully ready in every observed state; at
  least one passive soak retained relay status showed the operator gateway not
  ready.

## Critical Gap: Real Van Occupant Detection

No completed test in this run proves the core end-to-end claim that the system
detects a child/person left inside an actual van after ignition-off arming. The
current evidence supports narrower behavior: MQTT, relay, ignition transitions,
BLE state reporting, simulated armed BLE distress triggering, desk-proxy LD2412
presence triggering, telemetry, and active-state false-positive behavior. It
also records failed GPS-fix and LD2412 target-clear attempts. The remaining
validation must include supervised real-van occupant-detection scenarios using
an adult volunteer and/or beacon/dummy setup. Never create an unattended child
or heat-risk scenario.

Also note the traceability caveat: the run folder records commit `4d69e42`, but
the preflight firmware build log reports app version `854da60`. Before citing
this as archival evidence, explain whether the test firmware, generated build,
or documentation snapshot changed between session creation and preflight.

Detailed plan: `real_van_child_detection_test_plan.md`.

## Can Still Be Run Now With No Physical Action

| Scenario | What It Would Add | Risk / Notes |
|---|---|---|
| Longer MQTT soak, e.g. 30-60 min extension | Better telemetry interval, duplicate publish, Wi-Fi RSSI, relay health statistics beyond the completed 15-min run | Safe; passive only; takes 30-60 min |
| `T-HA-01` dashboard/entity audit | HA entities match MQTT state and relay health | Safe if HA access is available; needs browser/API access |
| `T-HA-IGN-01` passive entity check only | HA ignition entity currently reflects `ignition_on=true` | Safe if not toggling ignition; stronger version needs ignition remote |
| SCD4x/CO2 extended passive run | Longer CO2 freshness, plausibility, and drift statistics beyond the completed short/fifteen-minute evidence | Safe; passive only |
| `T-SHT4X-01` extended | Temperature/RH stability over time | Safe; passive only |
| `T-GPS-01` extended indoor | Stable no-fix/no-location reporting indoors | Safe, but should be treated as additional failure/diagnostic evidence unless a fix is obtained |
| BLE repeatability rerun | Repeat automated Bread start/stop statistics | Safe while bus remains active; BeaconScope should stay stopped |
| MQTT retained-state audit | Retained topics are coherent (`bus/state`, beacon states, relay status/error, distress clear) | Safe; read-only |

## Can Be Run Now If You Are Near Hardware

| Scenario | Action Needed | What It Would Add | Risk / Notes |
|---|---|---|---|
| `T-IGN-ESPNOW-03` | Unplug ignition remote long enough for stale/timeout, then replug | Heartbeat timeout age, stale status, exit-grace recovery | Medium; can approach arming if left unplugged too long |
| `T-IGN-ESPNOW-02` | Reboot main board while remote is off or absent | Boot grace then fail-closed stale behavior | Medium/high; needs board reboot and can arm if sensors/beacons are present |
| `T-IGN-OVERRIDE-01` | Publish `bus_empty_on`, then create real ignition source edge | Override clears only on real source edge | Medium; command changes state semantics, should be scripted carefully |
| `T-BLE-FP-01` physical RSSI threshold | Move phone/beacon to marginal distance/orientation | Beacon chatter around RSSI threshold does not cause false distress in active state | Low/medium while active; stronger armed version is higher risk |
| `T-LD-NORMAL-01` | Create a clear no-target baseline, then enter LD2412 range after arming | Normal enter-after-armed mmWave trigger path | Medium; current desk setup failed to clear target, so this likely needs better placement or van setup |
| `T-GPS-02` outdoor/window variant | Move GPS antenna/device near window/outdoors | Time-to-fix and retained location update | Low if physically convenient; needed because `T-GPS-01` failed |

## Needs Explicit Safety Go-Ahead Because It Can Arm Or Trigger Alerts

| Scenario | What It Tests | Why It Needs Confirmation |
|---|---|---|
| Additional armed BLE repeatability rounds | Active child beacon near while armed triggers distress consistently across more trials | Requires ignition-off/armed state and can send real distress/Discord |
| `T-LD-NORMAL-02` | LD2412 target enters after armed in real van | Requires armed state and a target; can send real distress |
| Additional `T-LD-ARMENTRY` repeatability | Target present before arming triggers after extended sustain | Desk proxy passed once; repeat only if needed |
| `T-CO2-01` / `T-CO2-02` / `T-CO2-03` | Controlled CO2 rise trigger behavior | Requires arming and environmental stimulus; can trigger distress |
| `T-MULTI-01` | Multiple beacons plus adult target, one distress cycle and relay dedupe | Intentionally exercises emergency path |
| `T-DISARM-01` | Ack distress, inspect cabin, guarded disarm success | Requires a real or simulated active distress and guarded safety command |
| `T-BUSCLEAR-01` | Armed bus with no beacon and no LD2412 target emits bus_clear | Requires ignition-off/armed state and ensuring cabin clear |

## Fault / Disconnection Tests Left

| Scenario | Action Needed | Notes |
|---|---|---|
| `T-STALE-BLE-01` | Disable/prevent BLE updates longer than threshold | Could be tested by firmware/BLE disruption; may affect safety signals |
| `T-STALE-LD-01` | Disconnect LD2412 UART before/during run | Requires hardware access; may degrade safety detection |
| `T-STALE-LD-02` | LD2412 valid then stale | Hardware/fault injection needed |
| `T-SENSOR-INIT-01` | Boot with one sensor disconnected | Requires reboot and physical sensor disconnection |
| `T-NETWORK-01` | Cut Wi-Fi/MQTT while armed, restore | High value but disruptive; should be controlled, and offline alerts are delayed by design |

## Not Applicable Unless Hardware/Firmware Mode Changes

| Scenario | Reason |
|---|---|
| `T-IGN-GPIO-01` | Current system uses ESP-NOW ignition source, not GPIO ignition |
| `T-IGN-MIGRATE-01` | Requires flashing/migrating ignition source configuration |

## Recommended Next Tests

1. Real-van supervised occupant-detection plan in `real_van_child_detection_test_plan.md`.
2. `T-LD-NORMAL-02` in the van or with improved desk placement where target can clear before arming.
3. `T-CO2-03` only in supervised real-van conditions with temperature/abort controls.
4. `T-IGN-ESPNOW-03` if the operator can safely unplug/replug the ignition remote and watch timing.
5. `T-HA-01` if Home Assistant access is available.
