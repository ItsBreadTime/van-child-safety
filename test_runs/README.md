# Public Test Run Evidence

This folder contains sanitized evidence captured during firmware, relay, Home Assistant, MQTT, sensor, and supervised real-van validation runs.

The files are included to show the shape of the evidence behind the prototype, not to claim certification or complete safety validation. The strongest interpretation is still bounded: these runs demonstrate subsystem behavior, MQTT/relay contracts, guarded operator commands, and one supervised adult-occupant real-van alarm path.

## Runs

| Run | Scope | Public-use boundary |
|---|---|---|
| [lab-validation](lab-validation/) | Simulated, desk-proxy, passive telemetry, relay, Home Assistant, command, and sensor checks. | Useful subsystem evidence; not a complete real-cabin child-left-in-van validation. |
| [field-validation](field-validation/) | Supervised real-van route and adult-occupant evidence. | Supports route notifications and a supervised adult-occupant alarm path; not an unattended-child or heat-risk test. |

## Sanitization

The child/beacon labels in the source evidence are treated as pseudonyms and are preserved. Discord IDs, local paths, private IPs/hosts, Home Assistant person/zone/device entities, UUIDs, MAC addresses, GPS fields, operator handles, and secret-like values were replaced with neutral placeholders.

Raw MQTT, relay, collector, and scenario logs are included after sanitization. The summary layer lives in the run `README.md`, `chapter4_summary.md`, and each scenario `result.md`.

Screenshots, launchd/plist/shell supervision wrappers, PID files, and temporary OS files were omitted because they were not needed to understand the evidence and are difficult to scrub reliably.

See [SANITIZATION.md](SANITIZATION.md) for details.
