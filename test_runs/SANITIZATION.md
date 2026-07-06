# Test Run Sanitization Notes

This archive was regenerated from the private test-run evidence tree with a conservative public-redaction pass, then organized into `lab-validation/` and `field-validation/` public folders.

## Redacted Values

- Child and beacon labels such as `Bread`, `Butter`, and `Crystal` were preserved as public pseudonyms.
- Discord snowflake IDs were replaced with `000000000000000000`.
- Private IP addresses and hosts were replaced with documentation placeholders such as `192.0.2.10` and `mqtt.example.local`.
- Local and remote filesystem paths were replaced with `<local-path>` or `<remote-path>`.
- Home Assistant person, zone, and device tracker entities were replaced with example entities.
- UUIDs and MAC addresses were replaced with non-identifying placeholders.
- iBeacon `major` and `minor` values were replaced with stable dummy values so beacon roles remain distinguishable without publishing real beacon tuples.
- GPS coordinate, speed, course, altitude, satellite, and accuracy fields were neutralized; `gps_valid` was preserved because it is test evidence about fix availability, not a location value.
- Operator handles and secret-like values were replaced with placeholders.
- Android logcat/ADB package, process, PID, UID, and object-reference noise was generalized while preserving beacon-test command and advertising evidence.

## Raw Logs

Sanitized raw logs are included where they provide evidence value, including MQTT logs, relay JSONL snapshots, remote relay logs, collector logs, watchdog logs, and stop-collection logs. The surrounding interpretation is kept in each run `README.md`, `chapter4_summary.md`, or scenario `result.md`.

## Renamed Files

- The public archive is organized by evidence type (`lab-validation/` and `field-validation/`) instead of the original private timestamped capture directories.
