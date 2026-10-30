# Connector Reference

All harness connectors use the Deutsch DT and DTM series throughout the vehicle for environmental sealing and vibration resistance.

## Series Used

| Series | Type | Used For |
|--------|------|----------|
| DT | Power + signal, larger gauge | Power distribution, CAN bus trunk |
| DTM | Signal, smaller gauge | Sensor connections, node taps |

## CAN Bus Pinout (DT 4-pin)

| Pin | Signal | Wire Color |
|-----|--------|------------|
| A | CAN H | Yellow |
| B | CAN L | Green |
| C | GND | Black |
| D | 12V switched | Red |

## Sensor Connectors (DTM 4-pin)

| Pin | Signal |
|-----|--------|
| 1 | VCC (5V or 3.3V) |
| 2 | GND |
| 3 | Signal A |
| 4 | Signal B (if needed) |

## Per-Sensor Connector Assignments

| Sensor | Node(s) | Connector | Keyway |
|--------|---------|-----------|--------|
| Gear tooth encoder (Littelfuse 55505-00-02-A-ND) | Front ×2, Rear ×1 | DTM04-3P | Key A |
| Suspension bellcrank encoder (AS5600, analog out) | Front ×2, Rear ×2 | DTM04-3P | Key B |
| Brake pressure transducer (Anfield T200/T201) | Front | DT04-3P or DT04-4P (match ordered variant) | — |
| Temperature (MLX90614, I2C — needs VDD/GND/SDA/SCL) | Rear | DTM04-4P | — |
| CAN bus trunk | Front, Rear, Firewall, Display | DT04-4P | — |

Front and Rear each carry several DTM04-3P connectors that look identical —
order them in different Deutsch keyways (Key A for gear tooth encoders, Key B
for suspension encoders) so they physically can't be cross-plugged, on top of
labeling both ends of the harness cable and documenting the mapping in a
wiring table.

**Keying alone no longer distinguishes left from right.** The Front node now
has two Key A connectors (FL, FR) and two Key B (FL, FR), and the Rear node
two Key B. Same-key connectors on one node *can* be cross-plugged, which
swaps left and right without any error — the data looks plausible and is
simply mirrored. Label them, and verify at bring-up by moving one corner at a
time and watching which channel responds.

The shock nitrogen pressure transducers are a **testing instrument only** and
are not part of the race harness — don't allocate connectors for them.

## Crimping Notes

- Use Deutsch crimping tool (DAM-1 or equivalent) — do not use generic crimpers
- Solid wire not permitted — stranded only in DT/DTM contacts
- Seal plugs required on all unused cavities
- Contact size: DT uses size 16, DTM uses size 20

## Sourcing

- Deutsch connectors: Del City, Allied Electronics, or direct from TE Connectivity
- Pre-assembled pigtails available from Del City for common configurations
