# Chapter 4 Simulated And Desk-Proxy Evidence Run

Created at: `2026-07-03T23:30:38+07:00`
Git commit: `4d69e42`
Git branch: `main`
Run type: simulated, passive, and desk-proxy subsystem evidence.

## Start Here

Use [chapter4_summary.md](chapter4_summary.md) as the table of contents for the
scenario results. It is the best single file for Chapter 4 writing because it
keeps the evidence boundaries attached to each result.

## How To Read This Run

This run is useful subsystem evidence, but it is not a full field validation of
the child-left-in-van safety claim. Most scenarios were simulated, passive, or
desk-proxy checks. They show that specific MQTT contracts, relay behavior,
command handling, and sensor telemetry paths worked during this session.

Do not cite a `pass` row as proof of real-cabin occupant detection unless the
scenario explicitly says it was a supervised van test. The completed armed BLE
and LD2412/mmWave trigger checks used `bus_empty_on` override and/or desk setup
conditions, so they are supporting evidence for firmware and relay behavior,
not final evidence of physical placement, RF propagation, cabin geometry, CO2
rise, or unattended-child safety.

The run metadata records Git commit `4d69e42`, while the preflight firmware
build log reports app version `854da60`. Treat the exact firmware-under-test
identity as requiring explanation before using this folder as archival evidence.

## Scenario Groups

| Group | Scenarios | Use for |
|---|---|---|
| BLE and beacon behavior | `T-BLE-*`, `T-ADVISORY-01` | Armed BLE trigger path, Bread beacon latency, active-state false-positive checks, and beacon-silent advisory behavior. |
| Relay behavior | `T-RELAY-*`, `T-ADVISORY-02`, `T-HA-ZONE-01` | Routine/warning/emergency relay formatting, dedupe, acknowledgement policy, sensor-fault advisory, and HA zone events. |
| Guarded operator commands | `T-DISARM-*` | Rejection of unsafe disarm commands, guarded disarm with cabin-clear confirmation, and command-result latency. |
| Ignition and ESP-NOW | `T-IGN-*` | Ignition heartbeat unplug/replug, rapid reconnect behavior, and an incomplete/failing timeout attempt. |
| Sensor telemetry | `T-SCD4X-*`, `T-SHT4X-*`, `T-LD2412-*`, `T-LD-ARMENTRY-01`, `T-GPS-01` | Passive telemetry freshness, desk-proxy mmWave trigger behavior, indoor no-fix GPS evidence, and placement/controllability caveats. |
| Home Assistant and retained MQTT | `T-HA-*`, `T-MQTT-*` | HA package/source-field checks, retained-state coherence, and short MQTT soak stability observations. |

## Key Outcomes

- Strongest BLE result: `T-BLE-03` produced Bread-specific simulated
  `distress_active`, retained distress, real Discord emergency output, and
  clean restoration with Crystal absent.
- Strongest command result: `T-DISARM-01` showed the guarded disarm workflow,
  while `T-DISARM-02` and `T-DISARM-02-LATENCY` showed unsafe disarm rejection.
- Useful negative results: `T-GPS-01` recorded indoor/no-fix GPS behavior, and
  `T-LD2412-PASSIVE-01` showed the LD2412 target did not clear during the
  intended passive movement test.
- Do not use `T-BLE-02` as a pass/fail result; it is marked invalid because the
  harness missed a transient state. Use `T-BLE-03` instead.
- `T-IGN-ESPNOW-03` has summary JSON but no `result.md` row in the generated
  summary; treat it as an incomplete/failing exploratory attempt unless it is
  rerun and documented.

## Evidence Files

- Root snapshots: `manifest.json`, `config_snapshot.txt`, `git_commit.txt`,
  `git_branch.txt`, `git_status.txt`.
- Preflight: `preflight/idf_build.txt`, `preflight/idf_size.txt`,
  `preflight/relay_pytest.txt`, `preflight/summary.json`.
- Scenario evidence: each scenario folder keeps its own `README.md`,
  `result.md` where available, MQTT logs, relay snapshots, and
  scenario-specific CSV/JSON evidence.

The original private capture path and regeneration command were redacted; this
public export keeps the sanitized run under `test_runs/lab-validation/`.
