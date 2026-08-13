# Project status and what's left

Last updated: 2026-08-12

## The one-paragraph version

There are **two parallel systems** in this repo right now, and it's worth being
clear about which is which.

**v1 is real and working.** Three Bluepill nodes → CAN → LoRa → SD → PC app,
verified end to end on actual hardware. It carries 9 placeholder channels
(temp/humidity/pressure, batt/current/power, accel x/y/z).

**v2 is a tested library that nothing runs yet.** The protocol, the Python
decoder, the DBC, the sensor drivers, and the E-CVT controller all exist, all
pass tests (13 suites, all green), and none of them are wired into any
firmware or into the ground station. `can_node.c` and `telemetry_hub.c` are
still pure v1 — no `#include` of the v2 protocol anywhere in `firmware/`.

That's a deliberate state, not an oversight: the base got built first so the
integration can happen against something stable. But it means **no v2 byte has
ever gone over a real wire.**

---

## Track A — finish validating what already works (hardware, needs you)

These are the only items that can't be done at a keyboard.

- [ ] **Stage 3: SD card.** `HUB_ENABLE_SD` is already `1` in firmware, but
      it's never been confirmed working. Format a card FAT32, insert, run the
      hub, then verify `LOG0001.TLM` grows by 42 bytes every 500 ms. If it
      doesn't, check the `sd_errors` counter.
- [ ] **Stage 4: Fault recovery.** Unplug a node's CAN connector mid-run,
      confirm the dashboard flips that node to stale within a couple frames,
      replug, confirm it recovers. Then unplug the bus itself and confirm
      `busoff_events` increments and the hub recovers rather than wedging.
- [ ] **Measure real LoRa throughput.** Everything about v2's frame size rests
      on a ~200 B/s estimate that came from a code comment and has never been
      measured. See Track C — this number decides whether v2 fits at 2 Hz.

## Track B — make v2 actually run (software, I can do)

Ordered by dependency. Each one is blocked by the one above it.

- [ ] **1. Wire `proto_v2.py` into the ground station.** The decoder is
      byte-exact with the C and fully tested, but nothing calls it —
      `sources.py` / `pump.py` / `run.py` only know v1. Needs a
      `--proto v2` switch. *Deliberately deferred:* this edits the one code
      path currently proven working on your hardware, and there's no v2 data
      to feed it yet, so there's no rush.
- [ ] **2. Write a v2 node firmware.** Nothing transmits v2. `can_node.c`
      would need to pack channels into multiple pages via
      `tlm2_can_pack_pages()` instead of one 8-byte frame. Best done as a new
      file alongside the working `can_node.c`, not as an edit to it.
- [ ] **3. Write v2 hub reassembly.** `telemetry_hub.c` collects one frame per
      node; v2 needs the epoch-based page reassembler
      (`tlm2_reasm_*`) so torn bursts don't publish half-updated snapshots.
- [ ] **4. Decide how Node 3 (E-CVT/Motor) gets populated.** It's the only
      node with no physical board — its data comes from the ODrive over CAN,
      read by the hub. So the hub both *receives* it and *originates* that
      node's record. That's a real design question, not a coding task, and
      it's currently unanswered.
- [ ] **5. Bundle the new dashboards into the built .exe.** `pc_app/build.bat`
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
