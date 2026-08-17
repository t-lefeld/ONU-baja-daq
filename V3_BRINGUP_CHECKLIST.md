# V3 bring-up checklist — per board

A checkbox list to work through board by board. Pin/CubeMX details live in
`V3_CUBEMX_AND_SENSOR_INTEGRATION_GUIDE.md` (the canonical pinout — this
file doesn't repeat pin numbers, just the order of operations). Verification
stimulus tests live in `DATA_VERIFICATION_CHECKLIST.md` Part 2 — linked
per-sensor below instead of duplicated.

Same four-step pattern for every sensor, every board:

1. CubeMX: assign the peripheral, Generate Code.
2. Fill in that driver's `TODO:` constants in `firmware/sensors/*.h`.
3. Tell me it's ready — I wire the driver's read call into `can_node_v2.c`
   (Front/Rear) or `telemetry_hub_v2.c` (Hub), replacing the simulated
   channel, one sensor at a time.
4. Physically verify — perturb the sensor, confirm the dashboard channel
   moves correctly (see the linked row in `DATA_VERIFICATION_CHECKLIST.md`).

Do these one sensor at a time, not all-at-once per board — easier to tell
which one broke if something doesn't read right.

---

## Hub (Nucleo-L476RG) — node 0

No PCB shared yet for this board — pins in the guide are still suggestions
(NEO-M8N on USART3 PC4/PC5, BNO080 on I2C1 PB8/PB9 + INT/RST on PB1/PB2).
Confirm/adjust against your actual Hub wiring before generating code.

- [x] GPS (NEO-M8N): CubeMX USART3 config (done 2026-08-16 — PC4/PC5,
      Asynchronous, 9600 baud per NEO-M8N's power-on default, RX interrupt
      enabled)
- [ ] GPS: fill `gps_neo_m8n.h` TODOs
- [ ] GPS: wire into `hub2_originate_node0()` (replaces sim `gps_lat`,
      `gps_lon`, `gps_speed`, `gps_heading`, `gps_sats`)
- [ ] GPS: verify — walk antenna outside, check `gps_sats` climbs, lat/lon
      settles (`DATA_VERIFICATION_CHECKLIST.md` Part 2, GPS row)
- [x] IMU (BNO080): CubeMX I2C1 + INT/RST GPIO config (done 2026-08-16 —
      I2C1 SCL/SDA on PB8/PB9 Standard Mode 100kHz, PB1 EXTI1 falling-edge
      pull-up for INT, PB2 GPIO output for RST. Note: CubeMX auto-assigned
      SDA to PB7 the first time since both PB7 and PB9 are valid on the
      L476 — corrected to PB9 by hand to match this doc)
- [ ] IMU: fill `imu_bno080.h` TODOs (watch the Q-point scaling — README
      flags "off by a clean factor like 2x or 4x" as the first thing to
      recheck if readings look wrong)
- [ ] IMU: wire into `hub2_originate_node0()` (replaces sim `accel_x`,
      `accel_y`, `accel_z`, `gyro_z`)
- [ ] IMU: verify — tilt board by hand, check `accel_z`/`gyro_z` respond
      (Part 2, IMU row)

## Front Bluepill — node 1

Pins confirmed from your actual PCB (see the guide's Front table). One open
item: PREST1 vs PREST2 — decide primary/backup/averaging logic before
wiring `brake_pressure_f` (see guide's calibration section 3).

- [x] Wheel encoders (FL+FR): CubeMX GPIO+EXTI on PA0/PA1 (done 2026-08-16 —
      EXTI0/EXTI1 falling edge + pull-up, NVIC EXTI0/EXTI1 enabled, code
      generated to the repo project and verified via `run_all_tests.py`)
- [ ] Encoders: fill `wheel_encoder.h` TODOs (tooth count, rolling
      circumference — guide's calibration section 2)
- [ ] Encoders: wire into Front's channel packing (replaces sim
      `wheel_speed_fl`, `wheel_speed_fr`)
- [ ] Encoders: verify — spin wheel by hand (Part 2, wheel encoder row)
- [x] Suspension pots (FL+FR): CubeMX ADC1 on PA4/PA5 (done 2026-08-16)
- [ ] Suspension: fill `suspension_pot.h` TODOs (Vmin/Vmax endpoint
      calibration — measure by hand, guide's calibration section 1)
- [ ] Suspension: wire into Front's channel packing (replaces sim
      `suspension_fl`, `suspension_fr`)
- [ ] Suspension: verify — compress/extend by hand (Part 2, suspension row)
- [x] Pressure transducers (PREST1+PREST2): CubeMX ADC1 on PA6/PA7 (done
      2026-08-16 — ADC clock fixed to a valid 12MHz via /6 prescaler,
      PLLMUL left at X9 so system clock stayed 72MHz)
- [ ] Pressure: fill `pressure_transducer.h` TODOs for BOTH sensors
      (divider math, zero offset, span — guide's calibration section 3)
- [ ] Pressure: decide PREST1-only / averaged / failover logic (tell me
      which — affects how I wire it in)
- [ ] Pressure: wire into Front's channel packing (replaces sim
      `brake_pressure_f`)
- [ ] Pressure: verify — apply brake by hand (Part 2, brake pressure row)

## Rear Bluepill — node 2

Pins confirmed from your actual PCB, **except the CVT thermistor I2C pins —
fix the PCB trace first** (guide's Rear table: reroute SCL from PB8 to PB6,
keep SDA on PB7).

- [x] Wheel encoders (RL+RR): CubeMX GPIO+EXTI on PA0/PA1 (done 2026-08-16)
- [ ] Encoders: fill `wheel_encoder.h` TODOs (separate calibration from
      Front — different tire/gearing if applicable)
- [ ] Encoders: wire into Rear's channel packing (replaces sim
      `wheel_speed_rl`, `wheel_speed_rr`)
- [ ] Encoders: verify — spin wheel by hand
- [x] Suspension pots (RL+RR): CubeMX ADC1 on PA4/PA5 (done 2026-08-16)
- [ ] Suspension: fill `suspension_pot.h` TODOs
- [ ] Suspension: wire into Rear's channel packing (replaces sim
      `suspension_rl`, `suspension_rr`)
- [ ] Suspension: verify — compress/extend by hand
- [ ] **PCB fix: reroute CVT thermistor SCL trace from PB8 to PB6** — this is
      physical board rework, not something CubeMX touches; still needs doing
      before the sensor can actually be wired in
- [x] CVT temp (MLX90614): CubeMX I2C1 on PB6/PB7 (default, no remap) — done
      2026-08-16, Standard Mode 100kHz. This is the *firmware-side* config
      only — it assumes the PCB fix above happens; if the trace still runs
      to PB8 on the physical board, this pin config won't match reality
- [ ] CVT temp: fill `cvt_temp_mlx90614.h` TODOs, aim sensor at CVT belt
      per guide's calibration section 4
- [ ] CVT temp: wire into Rear's channel packing (replaces sim `cvt_temp`)
- [ ] CVT temp: verify — compare against a reference thermometer at rest
      (Part 2, CVT temp row)

## Motor / E-CVT — node 3

No pins — the ODrive is its own CAN node.

- [ ] Set ODrive `axis_node_id` via `odrivetool` (default 0 is fine, must
      stay below 16 — see `protocol/V2_DESIGN_NOTES.md`)
- [ ] Confirm `HUB2_ODRIVE_AXIS_NODE_ID` in `telemetry_hub_v2.h` matches
      whatever you set
- [ ] Wire the real ODrive onto the CAN bus
- [ ] **Remove the 1st Bluepill stand-in from the bus** (it's not a v3
      component — see `can_node_v2.h`'s comment on the stand-in's role)
- [ ] **Remove the `HUB2_SIMULATE_NODE3=0` build flag** (both
      `platformio.ini` and the CubeIDE `.cproject`) — once the stand-in's
      off the bus, the hub goes back to originating node 3 from
      ODrive-decoded values, same mechanism as node 0, so this should
      revert to its default (1)
- [ ] Wire `odrive_can.c`'s decode into `hub2_originate_node3()` (replaces
      the simulated `motor_current`, `motor_velocity`, `motor_temp`,
      `bus_voltage`; `brake_resistor_w` stays estimated — see the guide's
      confidence notes, there's no direct CAN message for it)
- [ ] Verify — spin motor by hand or command a small move, confirm
      `motor_current`/`motor_velocity` respond (Part 2, ODrive row)

---

## After all boards are done

- [ ] Run `python tools/run_all_tests.py` — all 15 suites should still pass
- [ ] Full bench run with all real sensors live, cross-check the
      correlations in `DATA_VERIFICATION_CHECKLIST.md`'s "worth doing once
      several sensors are live together" section (GPS speed vs. encoder
      speed vs. IMU-integrated speed; lateral accel vs. suspension
      difference; motor current vs. braking/acceleration events)
- [ ] Update `HANDOFF.md`'s v1/v2/v3 status table to mark v3 verified
