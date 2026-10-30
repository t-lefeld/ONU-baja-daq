# Real sensor drivers

Base driver code for the real sensor suite from `simulation/vehicle_data.py`,
written so you can go through pin-by-pin and wire each one up on your own
schedule. **None of this is wired into `can_node.c` / `telemetry_hub.c` yet**
- that's intentionally left for you, once pins are assigned per board, so
nothing here forces a decision you haven't made yet.

Every file has `TODO` comments marking exactly what needs a real pin number,
calibration constant, or datasheet double-check before it'll work - search
for `TODO` in a file to get the full list for that sensor.

## Sensor -> node -> file map

| Sensor | Real part | Node (per `simulation/vehicle_data.py`) | Channels | Driver |
|---|---|---|---|---|
| GPS | HGLRC Mini M100 | Hub (Firewall) | gps_lat, gps_lon, gps_speed, gps_heading, gps_sats | `gps_neo_m8n.h/.c` |
| IMU | BNO080 | Hub (Firewall) | accel_x, accel_y, accel_z, gyro_z | `imu_bno080.h/.c` |
| Gear tooth encoder x3 | Littelfuse 55505-00-02-A-ND | Front (fl, fr), Rear (brake disc) | wheel_speed_* | `wheel_encoder.h/.c` |
| Suspension encoder x4 | AS5600, analog output | Front (fl/fr), Rear (rl/rr) | suspension_* | `suspension_pot.h/.c` |
| Brake pressure | Anfield T200/T201 | Front | brake_pressure_f | `pressure_transducer.h/.c` |
| Temperature | Melexis MLX90614 | Rear | cvt_temp | `cvt_temp_mlx90614.h/.c` |
| Motor telemetry | ODrive S1 (native CAN) | E-CVT | motor_current, motor_velocity, motor_temp, bus_voltage, brake_resistor_w | `odrive_can.h/.c` |

**Driver filenames lag the parts.** `gps_neo_m8n.*` is named for a u-blox
NEO-M8N and `suspension_pot.*` for a potentiometer; the car now uses an HGLRC
Mini M100 and an AS5600. The files were not renamed, because in both cases
the driver logic still applies:

- The M100 is a u-blox M10-based module emitting read-only NMEA over UART,
  same shape as the M8N, so the parser carries over. **Check the module's
  power-on baud rate against the `9600` the driver assumes** — that figure
  came from the M8N and has not been verified against the M100.
- The AS5600 runs in **analog output** mode (its I2C address is fixed at
  `0x36`, so the two per node would collide), which means it is read through
  the ADC exactly like the pot was — same divider, same two-point
  calibration. See `suspension/travel-sensor/README.md`.

Rename them when you next touch the code; don't let the filename convince you
of the part.

There are **three** gear tooth sensors, not four: one per front wheel, plus a
single rear sensor that reads the **brake disc** rather than a wheel hub. Its
pulses-per-revolution differs from the front pair — calibrate it separately.

`pressure_transducer.h/.c` is also what the shock nitrogen pressure
transducers use — same ADC-divider-calibrate pattern. Those are a **testing
instrument only** and do not go on the car at a race.

## Confidence level per driver

Not all of these are equally solid - worth knowing which ones to trust vs.
scrutinize before wiring:

- **wheel_encoder, suspension_pot, pressure_transducer**: straightforward
  GPIO/ADC reads, standard patterns, high confidence once pin/calibration
  TODOs are filled in. For `suspension_pot` against an AS5600, confirm the
  reading is monotonic across the full sweep before calibrating — a magnetic
  encoder **wraps** past its single-turn range instead of clipping, so
  over-travel reads as a jump to the opposite extreme rather than a stuck
  value.
- **cvt_temp_mlx90614**: well-defined SMBus protocol, high confidence.
- **odrive_can**: the 3 core command IDs (`Get_Encoder_Estimates`, `Get_Iq`,
  `Get_Bus_Voltage_Current`) are from the ODrive CAN protocol reference and
  should be solid. `Get_Temperature`'s command ID is an educated guess, NOT
  verified - check it against `can_simple.dbc`, which ships with every
  ODrive firmware release in their repo's `Firmware/` folder, before
  trusting motor_temp readings. `brake_resistor_w` is a rough estimate from
  regen current, not a real measurement - there's no direct CAN message for
  it in the standard protocol.
- **imu_bno080**: the SHTP framing/plumbing (packet read/write, feature
  enable) is structurally correct SH-2 protocol, but this is genuinely the
  most complex sensor here. The Q-point scaling constants (`ACCEL_Q_POINT`,
  `GYRO_Q_POINT`) are from the SH-2 reference manual - if bring-up readings
  look "close but off by a clean factor like 2x or 4x," that's the first
  thing to re-check.

## Before any of this: generate the .ioc

Every driver here needs a peripheral that CubeMX has to create first -
the `hadc`/`hi2c` handles, the GPIO clock enables, the EXTI NVIC entries.
**Open the project's `.ioc` in STM32CubeMX, assign the peripheral, and
Generate Code before filling in any TODO below.** Do not hand-edit a
`.ioc`: CubeMX silently drops keys it does not recognise, so a bad edit
looks like everything working until the peripheral misbehaves.

If a handle is never initialised, these drivers return zeros rather than
erroring - a missing CubeMX step shows up as a channel reading a
plausible, constant, entirely fake value.

See `firmware/PINOUT.md` for what is already assigned (and the EXTI
line-sharing trap that will bite two wheel encoders), and
`firmware/CUBEMX_SETUP.md` for the verification checks.

## Wiring this in once pins are assigned

Each driver's `_read()` / `_update()` / `_poll()` function returns a plain
float in physical units (mph, mm, psi, degC, g, deg/s) - it does not know
anything about the CAN wire format. When you're ready to actually use one:

1. In CubeMX, configure whatever peripheral the driver needs (GPIO+EXTI,
   ADC channel, I2C, or nothing extra for `odrive_can` since it reuses the
   CAN peripheral already configured for the node's normal traffic).
2. Fill in that driver's TODO constants (pins, calibration, node ID).
3. In `can_node.c` (or `telemetry_hub.c` for the Hub's directly-attached GPS/
   IMU), replace the relevant channel(s) currently coming from
   `sim_read_channels()` with a call into the real driver instead.

One thing worth deciding before that last step: the current wire protocol
(`protocol/telemetry_proto.c`) only carries 3 channels per node
(`TLM_CH_PER_NODE`), but several of these real nodes need more (Hub needs 9,
Front/Rear/Motor need 5 each) - that's a separate protocol-capacity change
from what's in this folder, worth planning out before the first real sensor
goes on the wire rather than after.
