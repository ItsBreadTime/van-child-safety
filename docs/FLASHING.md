# Flashing And OTA

## USB Flash

```bash
idf.py build
idf.py -p /dev/cu.usbserial-XXXX flash monitor
```

Use the serial device that matches your attached board.

## OTA

The OTA server starts only after WireGuard is connected and only when `CONFIG_OTA_TOKEN` is non-empty.

Example:

```bash
curl --fail \
  --data-binary @build/van_child_safety.bin \
  "http://<wireguard-address>:3232/update?token=<ota-token>"
```

OTA should be reachable only over a private network such as WireGuard. Rotate the OTA token if it is exposed.
