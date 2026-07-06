# Enclosure

Two-piece 3D-printed enclosure exports.

| File | Purpose |
|---|---|
| `top.step` | Top shell CAD export |
| `top.stl` | Top shell printable mesh |
| `bottom.step` | Bottom shell CAD export |
| `bottom.stl` | Bottom shell printable mesh |
| `assembly.png` | Assembly render |

STEP and STL exports preserve the final geometry, not the full parametric model history.

## Design Checks

- Keep antennas clear of metal.
- Do not block the LD2412 radar path with dense or metallic material.
- Give GPS a sky-facing path if used.
- Confirm heat, vibration, and mounting safety before any vehicle use.
