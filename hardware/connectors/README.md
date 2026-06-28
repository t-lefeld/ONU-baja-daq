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

## Crimping Notes

- Use Deutsch crimping tool (DAM-1 or equivalent) — do not use generic crimpers
- Solid wire not permitted — stranded only in DT/DTM contacts
- Seal plugs required on all unused cavities
- Contact size: DT uses size 16, DTM uses size 20

## Sourcing

- Deutsch connectors: Del City, Allied Electronics, or direct from TE Connectivity
- Pre-assembled pigtails available from Del City for common configurations
