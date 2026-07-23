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
| Wheel hub encoder (Littelfuse 55075) | Front, Rear | DTM04-3P | Key A |
| Suspension bellcrank pot (Bourns 53AAA-B28-B15L) | Front, Rear | DTM04-3P | Key B |
| Pressure transducer (Anfield T200/T201) | Front | DT04-3P or DT04-4P (match ordered variant) | — |
| E-CVT belt temp (MLX90614, I2C — needs VDD/GND/SDA/SCL) | Rear | DTM04-4P | — |
| CAN bus trunk | Front, Rear, Firewall | DT04-4P | — |

Front and Rear each carry two DTM04-3P connectors (encoder + pot) that look
identical — order them in different Deutsch keyways (Key A vs Key B) so they
physically can't be cross-plugged, on top of labeling both ends of the
harness cable and documenting the mapping in a wiring table.

## Crimping Notes

- Use Deutsch crimping tool (DAM-1 or equivalent) — do not use generic crimpers
- Solid wire not permitted — stranded only in DT/DTM contacts
- Seal plugs required on all unused cavities
- Contact size: DT uses size 16, DTM uses size 20

## Sourcing

- Deutsch connectors: Del City, Allied Electronics, or direct from TE Connectivity
- Pre-assembled pigtails available from Del City for common configurations
