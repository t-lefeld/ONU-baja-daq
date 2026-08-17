# eCVT Actuation

**Controller:** ODrive S1 (commercial motor controller, not a custom node board)  
**Location:** CVT assembly  
**Status:** 🔄 In progress — mechanical specs confirmed, no longer blocked

## What changed

This was originally planned as a fourth STM32 Blue Pill sensor node (like
Front/Rear) reporting RPM/belt temp/actuator position over CAN. Once the
mechanical team's specs came in, the actual design is a commercial ODrive S1
motor controller assembly instead — there's no custom firmware or sensor
ADC/GPIO wiring to build here. CVT belt temperature is instead read by the
**Rear node**'s MLX90614 (see `nodes/rear/README.md`), since it's physically
closer to the belt.

## Components

| Component | Part | Notes |
|-----------|------|-------|
| Motor controller | ODrive S1 | Drives the CVT actuator motor via FOC |
| Actuator motor | ODrive D5065 | Smaller motor variant, built-in thermistor |
| Encoder | ODrive Encoder OA1 | Onboard, feeds ODrive S1's control loop directly |
| Brake resistor | 2Ω, 50W (e.g. Vishay TMC50-2 or equivalent) | Dumps regen energy; ships standard with the S1 |

## Data available

Primary/secondary RPM and actuator position live inside the ODrive S1's own
telemetry (its FOC loop already tracks these via the OA1 encoder and the
D5065's built-in thermistor) — read them over the ODrive's native CAN or UART
protocol rather than through custom node firmware. Whether this assembly taps
into the vehicle CAN bus for telemetry logging (vs. staying on its own
isolated interface) is still to be decided with the team.

## Connectors / enclosure

The ODrive S1's I/O (encoder, thermistor, brake resistor leads) uses its own
onboard JST-style connectors — no Deutsch DT/DTM parts are needed for this
assembly, and it doesn't currently have a custom PAHT-CF enclosure the way
Front/Rear/Firewall do (see `hardware/enclosures/README.md`). Revisit this
once final mounting/packaging is decided with the team.

## Notes

- Do not assume a CAN ID/message format for this assembly yet (previously
  reserved as `eCVT_Status` / 0x030-0x031 in `can/README.md` — confirm with
  the team whether that's still applicable to the ODrive's native protocol
  before relying on it)
