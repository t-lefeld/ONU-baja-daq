# Project status and what's left

Last updated: 2026-08-22

> **If this file disagrees with `HANDOFF.md`, believe `HANDOFF.md`.** That one
> is kept current as the orientation doc; this one is the longer task list and
> lags behind it. Status claims here were last reconciled against it on
> 2026-08-22.

## The one-paragraph version

There are now **three** stages in this repo: v1, v2, v3.

**v1 is real and working.** Three Bluepill nodes → CAN → LoRa → SD → PC app,
verified end to end on actual hardware. It carries 9 placeholder channels
(temp/humidity/pressure, batt/current/power, accel x/y/z). Kept alive on the
1st Bluepill as a bench spare even after the v2 cutover below.

**v2 is fully bench-verified on real hardware as of 2026-08-16.** The
protocol, Python decoder, DBC, sensor drivers, E-CVT controller, and both the
Bluepill node firmware (`can_node_v2.c`) and hub firmware
(`telemetry_hub_v2.c`) all exist and pass tests (15 suites, all green).
`can_node_v2.c`/`telemetry_hub_v2.c` are flashed to the 2nd Bluepill (Front),
3rd Bluepill (Rear), and Nucleo hub, and end-to-end data flow, SD logging, and
fault recovery (node drop/reconnect, full bus drop/reconnect) are all confirmed
working on the bench. All 24 channels are still **simulated in firmware**, not
read from real sensors — that's v3.

**v3 is in progress as of 2026-08-16.** CubeMX pin-out is done for all three
boards (Front, Rear, Hub). What is *not* done: no sensor driver has been wired
into `can_node_v2.c`'s `sim_read_channels()` or `telemetry_hub_v2.c`'s
`hub2_originate_node0/3()` yet, and nothing has been physically verified
against a real sensor. `V3_BRINGUP_CHECKLIST.md` has exact per-sensor status.

---

## Track A — finish validating what already works (hardware, needs you)

These are the only items that can't be done at a keyboard.

- [x] **Stage 3: SD card.** Confirmed working on the bench 2026-08-16 —
      `LOG0001.TLM` grows as expected and `sd_errors` stays at zero.
- [x] **Stage 4: Fault recovery.** Confirmed working on the bench 2026-08-16 —
      a dropped node goes stale within a couple of frames and recovers on
      replug; a full bus drop increments `busoff_events` and the hub recovers
      rather than wedging.
- [ ] **Measure real LoRa throughput.** Everything about v2's frame size rests
      on a ~200 B/s estimate that came from a code comment and has never been
      measured. See Track C — this number decides whether v2 fits at 2 Hz.

## Track B — make v2 actually run

- [x] **1. Wire `proto_v2.py` into the ground station.** Done — `protocols.py`
      switches between v1/v2, `run.py --proto v2 --port COM9` speaks it live.
- [x] **2. Write a v2 node firmware.** Done — `can_node_v2.c`, now copied into
      the 2nd and 3rd Bluepill projects (Front/Rear) with `main.c` pointed at
      it.
- [x] **3. Write v2 hub reassembly.** Done — `telemetry_hub_v2.c` (502 lines,
      committed 2026-08-13), now copied into the Nucleo hub project with
      `main.c` pointed at it and v1's `telemetry_hub.c` excluded from the
      build via `.cproject` (both define a strong
      `HAL_CAN_RxFifo0MsgPendingCallback`; linking both is a link error).
- [x] **4. Decide how Node 3 (E-CVT/Motor) gets populated.** Settled: the hub
      originates nodes 0 and 3 itself, simulated for now, through the same
      `tlm2_reasm_apply_page()` path real CAN frames use — see
      `firmware/NODE_INTEGRATION_V2.md`'s "Open questions" section for the
      reasoning.
- [ ] **5. Bench-test the above on real hardware.** Nothing above has been
      flashed or run yet — flash Front/Rear/Hub, leave the 1st Bluepill on
      v1 as spare, run the ground station in `--proto v2`, confirm all 24
      channels update from real hardware. This is the actual "v2 works" bar,
      not the code being wired in.
- [ ] **6. Bundle the new dashboards into the built .exe.** `pc_app/build.bat`
      doesn't include `simulation/dashboards/`, so the 4 designs work when
      running from source but would 404 in a PyInstaller build.

## Track C — unverified assumptions worth checking before trusting

Not bugs. Things asserted from documentation that nobody has confirmed against
real hardware. Each would produce plausible-looking but wrong numbers.

- [ ] **ODrive `Get_Temperature` command ID** (`firmware/sensors/odrive_can.c`).
      The other three ODrive command IDs came from the official CAN protocol
      reference; this one is an educated guess. Check it against
      `can_simple.dbc`, which ships in the ODrive firmware repo's `Firmware/`
      folder. Until then, don't trust `motor_temp`.
- [ ] **BNO080 Q-point scaling** (`firmware/sensors/imu_bno080.c`). The
      accel/gyro fixed-point exponents are from the SH-2 reference manual. If
      bring-up readings look "close but off by a clean factor of 2 or 4,"
      this is the first thing to re-check.
- [ ] **LoRa link budget.** v2's frame is 80 bytes vs v1's 42. At the assumed
      throughput that's ~80% link utilization with ~20% slack at 2 Hz. If the
      real measurement (Track A) comes in lower, the ranked fallbacks are in
      `protocol/V2_DESIGN_NOTES.md`: raise air rate to 4.8k, drop to 1 Hz,
      delta-encode only changed channels, or split nodes across cycles.
- [ ] **`brake_resistor_w` is an estimate, not a measurement.** There's no
      ODrive CAN message for brake resistor power; it's inferred from negative
      bus current during regen. Fine for a dashboard glance, not for verifying
      resistor sizing.

## Track D — yours by design (pins and calibration)

You asked to do these by hand. Every one is marked with a `TODO:` comment in
the relevant driver — search `TODO` in `firmware/sensors/` for the full list.

- [ ] **Generate the `.ioc` in CubeMX first** for any board gaining a
      peripheral - see `firmware/PINOUT.md`. No `.ioc` has been generated
      for the new sensors, and hand-editing one silently drops keys.
- [ ] Pin assignments for every sensor (GPIO/EXTI, ADC channels, I2C, UART)
- [ ] Tone-ring tooth counts and rolling circumference per wheel
- [ ] Suspension pot endpoint voltages (compress fully, read; extend fully, read)
- [ ] Brake transducer divider sizing + endpoint calibration
- [ ] ODrive `axis_node_id` — must match what you configured, and must not
      collide with the `0x200` block v2 uses
- [ ] E-CVT tuning constants (`firmware/control/README.md` has a starting
      procedure: set `ki=0`, raise `kp` until oscillation, then back off)

## Track E — repo hygiene

- [ ] **Nothing is committed.** `git log` reports no commits at all; all 11
      top-level entries are untracked. Weeks of work exists only on this one
      disk. This is the highest-value five minutes available right now.
- [ ] Consider whether `field_data/` logs should be committed or kept local
      once they start getting large (`.gitignore` already has an exception
      carved out for them).

---

## Known gaps in the E-CVT controller

Listed separately because they're design limits, not TODOs:

- No engine RPM source exists in this repo. The controller consumes an RPM
  value; nothing produces one yet.
- No wheel-slip detection. The grade estimate derives true acceleration from
  wheel speed, so a spinning or locked wheel silently corrupts it.
- Cornering rejection is a crude yaw-rate threshold, not real lateral/
  longitudinal decoupling.
- Assumes a velocity-controlled actuator. A position-controlled CVT actuator
  would need adaptation.
- The accelerometer sign convention is assumed and unverified against real
  mounted hardware (there's an `invert_accel_x` config flag for this).
