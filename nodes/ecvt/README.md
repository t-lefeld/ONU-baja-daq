# eCVT Actuation

**Controller:** ODrive S1 (commercial motor controller, not a custom node board)  
**Location:** CVT assembly  
**Status:** ⏸️ **DAQ side paused.** The ODrive assembly itself is settled and
sits on the CAN bus, but nothing is logging it yet. It should drop in easily
next year — this is a deliberate deferral, not a blocker.

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
D5065's built-in thermistor) — read them over the ODrive's native CAN
protocol rather than through custom node firmware.

**Settled:** the S1 sits on the shared vehicle CAN bus, not an isolated
interface, and the DAQ listens to it. Because the S1 has its own STM32 and
speaks CAN directly, no custom board is needed for this node — the only PCB
here is for power distribution, and that is a separate, later project.

Watch for CAN ID collisions: the ODrive addresses messages as
`(axis_node_id << 5) | cmd_id`, which overlaps the DAQ's own block unless the
ODrive stays below node id 16. See `protocol/V2_DESIGN_NOTES.md`.

## Connectors / enclosure

The ODrive S1's I/O (encoder, thermistor, brake resistor leads) uses its own
onboard JST-style connectors — no Deutsch DT/DTM parts are needed for this
assembly, and it doesn't currently have a custom PAHT-CF enclosure the way
Front/Rear/Firewall do (see `hardware/enclosures/README.md`). Revisit this
once final mounting/packaging is decided with the team.

## Power

The ODrive runs from the engine's **stator**, separately rectified and
filtered — not from the DAQ's DeWalt packs. A BLDC motor tolerates supply
noise far better than the sensor nodes, so its filtering can be simpler. The
rectification and filtering is its own piece of work, outside the DAQ.

## Notes

- Do not assume a CAN ID/message format for this assembly yet (previously
  reserved as `eCVT_Status` / 0x030-0x031 in `can/README.md` — confirm
  whether that's still applicable to the ODrive's native protocol before
  relying on it)
- `firmware/control/` holds a PI control loop for this actuator. It has never
  touched a real ODrive, engine, or CVT, and every gain in it is a
  placeholder
