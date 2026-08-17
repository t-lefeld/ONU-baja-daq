# Enclosures

All node enclosures are 3D printed in PAHT-CF (polyamide + carbon fiber) for stiffness, heat resistance, and impact tolerance in the Baja environment.

## Material

**Filament:** PAHT-CF  
**Printer:** Bambu P1S  
**Rating:** IP66 (dust-tight, high-pressure water jet resistant)  
**Color scheme:** Black body, orange accents

## Why PAHT-CF

- Higher heat deflection temperature than PLA or PETG — safe in engine bay adjacent locations
- Carbon fiber reinforcement provides rigidity without metal machining
- Printable on the P1S without enclosure modification
- IP66 achieved via gasket seal on lid — no potting required

## Sealing

- Lid sealed with closed-cell foam gasket (3M 4504 or equivalent)
- All cable entries via IP68-rated cable glands (M12 or M16 depending on harness diameter)
- Deutsch connector panel mount where multiple connections enter the enclosure

## Files

Print files located in this folder. Each enclosure folder contains:
- `body.stl` — main enclosure body
- `lid.stl` — sealed lid
- `print_settings.txt` — Bambu P1S slicer settings (layer height, infill, supports)

## Node Enclosures

| Node | Enclosure | Mounting |
|------|-----------|----------|
| Front | `front-node/` | Suspension upright bracket |
| Rear | `rear-node/` | Rear frame tab |
| eCVT | none planned | ODrive S1 assembly uses its own onboard connectors/housing — no custom PAHT-CF box currently planned (see `nodes/ecvt/README.md`) |
| Firewall | `firewall-node/` | Firewall panel mount |

All three printed enclosures (Front, Rear, Firewall) use a 1.7mm × 0.9mm TPU
gasket bead compressed ~25% (0.45mm rib depth on the lid, 0.85mm locating
channel in the base), closed with a fully-printed PAHT-CF over-center toggle
latch (flex-free — cantilever snap-fits aren't used since CF-filled nylon
has low elongation-at-break and cracks under repeated flex).
