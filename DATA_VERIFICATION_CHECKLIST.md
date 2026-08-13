# Data verification checklist

Two different questions hide behind "is the data real":

- **Part 1** - is the pipeline (CAN -> LoRa -> SD -> ground station) carrying
  data faithfully, with nothing dropped, duplicated, or silently stale? This
  applies right now, even while the nodes are still running the built-in
  simulator (`sim_read_channels()` in `can_node.c`).
- **Part 2** - once real sensors replace that simulator, is a given reading a
  genuine live measurement, or a frozen/stuck/miswired value that happens to
  look plausible? This only matters after you start swapping in hardware.

Do Part 1 first. A pipeline that can't be trusted to carry data faithfully
will make Part 2 impossible to reason about, since you won't know whether a
weird reading is a bad sensor or a bad wire format.

## Part 1 - Pipeline integrity (applies now)

The hub writes the *identical* encoded bytes to the radio and the SD card in
the same pass (see the comment above `sd_log_write()` in
`firmware/nucleo_hub/telemetry_hub.c`), so LoRa and SD are guaranteed
bit-identical by construction - you never need to separately compare those
two. The one real cross-check is CAN-side raw values against what comes out
the other end.

1. **Debug UART vs. app, same seq.** With the ST-Link VCP open (USART2,
   115200) and the ground station app running, pick a frame off the VCP line
   - e.g. `#50 t=25000 | N0 2050,4500,10130 | ...` - and confirm the app's
   displayed value equals the raw int divided by that channel's scale factor
   (`TLM_CHANNELS` in `protocol/telemetry_proto.c`), e.g. `2050 * 0.01 =
   20.50`.
2. **No silent freezing.** Unplug one node's CAN connector mid-run. That
   node's tile should flip to stale/offline within a couple of missed
   frames, not keep showing its last value as if nothing happened. Replug and
   confirm it recovers.
3. **CRC errors stay at zero** on a good link. If `CRC err` climbs while
   `frames` also climbs, bytes are arriving but getting corrupted in transit
   - a wiring/noise problem, not a "fake data" problem.
4. **SD replay matches what you saw live.** After a run, `python run.py
   --replay <path-to-LOG>.TLM` from `pc_app` should reproduce the exact
   values the live dashboard showed, frame for frame - see
   `field_data/README.md`.
5. **File size sanity.** `LOG0001.TLM` should grow by 42 bytes roughly every
   500 ms. A file that's stopped growing but the hub is still running points
   at an SD write failure (`sd_errors` counter), not a sensor problem.

## Part 2 - Physical sensor authenticity (once real hardware is wired in)

The general test for any sensor: **change the physical thing it measures and
confirm the reading changes accordingly, in the right direction, within a
plausible time.** A reading that doesn't move when you perturb it is either
disconnected, stuck, or you're looking at simulated/cached data.

| Sensor | Stimulus test | Red flag |
|---|---|---|
| GPS (NEO-M8N) | Walk the antenna outside/near a window; `gps_sats` should rise into the high single digits within ~30s cold start, lat/lon should settle to a stable position within a few meters | Sats stuck at 0, or lat/lon exactly repeating (frozen fix) |
| 9DOF IMU (BNO080) | Tilt/rotate the board by hand; `accel_z` should read ~1.0g flat, drop toward 0 as you tip it on its side; `gyro_z` should spike during the rotation and return to ~0 at rest | accel_z reads exactly 1.000g and never wiggles (dead sensor returning a constant), or gyro never returns to zero when stationary (bias/drift not zeroed) |
| Wheel encoders (Littelfuse 55075) | Spin the wheel by hand; speed channel should track rotation rate and return to 0 when stopped | Speed reads nonzero at a dead stop, or doesn't change with obviously different spin rates |
| Suspension pot (Bourns 53AAA) | Compress/extend the suspension by hand; travel value should move smoothly through its range and return to ride height at rest | Value jumps in discrete steps (bad wiper contact) or pins at one end regardless of position |
| Brake pressure (Anfield T200/T201) | Apply the brake by hand/pedal; pressure should rise sharply and fall back near 0 on release | Pressure reads a nonzero baseline with no pedal input, or never rises under real braking |
| CVT temp (MLX90614) | Compare against a known reference (another thermometer, or just ambient) at rest; should read close to ambient when cold, rise only after the CVT actually runs | Reads a fixed value regardless of whether the drivetrain has run at all |
| ODrive motor telemetry | Spin the motor by hand or command a small move; `motor_current`/`motor_velocity` should respond immediately, `motor_temp` should only rise gradually during actual runs | Current/velocity reads nonzero with the motor stationary and unpowered |
| Bus voltage | Should sag slightly under load (motor current spiking) and recover at idle | Perfectly flat voltage regardless of load is suspicious - either not actually reading the rail, or reading a regulated rail instead of the raw pack |

**Cross-checks worth doing once several sensors are live together:**

- GPS speed, wheel encoder speed, and IMU-integrated speed should roughly
  agree (GPS lags a bit and is noisier at low speed, but they shouldn't
  diverge wildly).
- Lateral IMU accel (`accel_x`) and the left/right suspension travel
  difference should correlate during a corner - outside wheels compress,
  inside wheels extend, IMU shows the same-direction lateral g.
- Motor current spikes should line up with braking (regen) or acceleration
  events visible in wheel speed - a current spike with flat wheel speed is
  worth investigating rather than assuming.

None of this replaces an actual calibration pass per sensor - it's a fast
sanity check to catch "the wire's not making contact" or "this is still
returning the simulator's numbers" before you trust a data set.
