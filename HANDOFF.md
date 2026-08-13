# Handoff — read this first

Written 2026-08-12 for whoever (or whatever) picks this up next. Assume no
memory of prior conversations. This file is the orientation; the other docs
are the detail.

---

## What this is

A telemetry system for a **Baja SAE** competition vehicle, built by one
student (Tate, Ohio Northern University). Sensors on the car → CAN bus →
a hub board → 900 MHz LoRa radio → laptop in the pits, with an SD card on the
hub as a black box in case the radio drops.

**Hardware, all physically in hand and working:**

- 3× STM32F103C8 "Blue Pill" — CAN sensor nodes
- 1× STM32L476RG Nucleo — hub (aggregates CAN, drives radio + SD)
- 2× Ebyte E22-900T22 LoRa modules — one `-D` (DIP, on the hub), one `-U`
  (USB dongle, on the laptop). **Both configured to channel `0x45`
  (decimal 69) = 919.125 MHz.** Config mode: DIP module needs M1=HIGH/M0=LOW
  wiring; the USB dongle has a side button (hold ~2 s, LED goes solid red).
- microSD module on the hub over SPI
- Programming: a Black Magic Probe (a Black Pill flashed with BMP firmware)
  for the Bluepills over PlatformIO; the Nucleo uses its own onboard ST-Link.

**Planned but not yet wired:** u-blox NEO-M8N GPS, BNO080 IMU, 4× Littelfuse
55075 wheel encoders, 4× Bourns 53AAA suspension pots, Anfield T200/T201
brake pressure, MLX90614 CVT temp, ODrive S1 motor controller.

---

## The single most important thing to understand

**There are two parallel systems in this repo. Only one of them runs.**

| | v1 | v2 |
|---|---|---|
| Status | **Working on real hardware**, verified end to end | Tested libraries, **runs nowhere** |
| Channels | 9 placeholder (temp/humidity/pressure, batt/current/power, accel x/y/z) | 24 real (GPS, IMU, wheel speed, suspension, brake, CVT, motor) |
| Nodes | 3 | 4 |
| Frame | 42 bytes @ 2 Hz | 80 bytes @ 2 Hz |
| Firmware | `can_node.c`, `telemetry_hub.c` | `can_node_v2.c` only — **`telemetry_hub_v2.c` does not exist** |

**No v2 byte has ever crossed a real wire.** The protocol, Python decoder,
DBC, sensor drivers and E-CVT controller all pass tests in isolation, and are
wired into nothing.

This is deliberate — the base was built first so integration happens against
something stable. But do not describe v2 as "working," and do not let the
green test board imply otherwise.

---

## Ground rules that have held all along

1. **Never modify the v1 files.** `protocol/telemetry_proto.{h,c}`, its four
   mirrored copies under `firmware/projects/*/Core/Src/`,
   `firmware/bluepill_node/can_node.c`, `firmware/nucleo_hub/telemetry_hub.c`.
   That is the known-good image Tate can always reflash to. v2 work goes in
   new files alongside.
2. **This repo has a `.git` directory but ZERO commits.** `git diff` is
   useless for checking what changed — everything is untracked. Verify
   "additive only" by mtime: `ls -l --time-style=+%m-%d_%H:%M`. v1 files
   should read Aug 1–4.
3. **Verify, don't assert.** Run the thing. Compile the thing. This has
   repeatedly caught errors that reading the code did not — see "mistakes
   already made" below.
4. **Tate does pins and calibration by hand.** He asked for clearly-labelled
   `TODO:` markers rather than guesses. Respect that.
5. **Do not generate `.ioc` files.** CubeMX silently drops keys it does not
   recognise, so a hand-edited one looks fine until a peripheral misbehaves.
   `firmware/PINOUT.md` says this loudly; keep it that way.

---

## Layout

```
protocol/     wire formats. telemetry_proto.* = v1 (live).
              telemetry_proto_v2.* = v2 (tested, unused). Both DBCs.
firmware/
  bluepill_node/   can_node.c (v1, live), can_node_v2.c (v2, tested)
  nucleo_hub/      telemetry_hub.c (v1, live), lora_e22.c, sd_log.c,
                   sd_spi.c, fat32.c. telemetry_hub_v2.h exists, .c DOES NOT
  sensors/         7 real-sensor drivers, all TODO-marked, none wired in
  control/         E-CVT PI controller + IMU hill feedforward, host-tested
  projects/        the 4 CubeIDE/PlatformIO projects (vendored HAL etc.)
pc_app/       Python ground station. run.py serves the browser dashboards.
              telemetry/proto.py = v1, proto_v2.py = v2, protocols.py picks.
simulation/   synthetic 24-channel feed + the dashboard app
field_data/   real SD captures go here. Empty on purpose (real data only).
tools/        15 test suites. run_all_tests.py runs everything.
```

**Docs worth reading before acting:** `PROJECT_STATUS.md` (what's left, by
track), `firmware/PINOUT.md` (pin audit + EXTI trap), `protocol/V2_DESIGN_NOTES.md`
(v2 rationale + open risks), `DATA_VERIFICATION_CHECKLIST.md` (how to tell
real sensor data from fake).

---

## Test suite

```
python tools/run_all_tests.py      # 15 suites, expect exit 0
```

**On Tate's Windows machine most C suites SKIP** — he has no host compiler
(STM32CubeIDE's targets ARM, not the host) and no `cantools`. His effective
coverage is roughly a third of what a Linux box with gcc gets. A green board
on his machine is honest but much quieter than it looks. `pip install
cantools` is the cheap win; the C tests need MSYS2/MinGW.

Convention: **a missing toolchain SKIPs and returns 0, never fails.** A suite
that goes red on a normal Windows box trains everyone to ignore red.

---

## Mistakes already made — do not repeat

- **A test that could only pass by not running.** `test_qt.py`'s Qt half
  skipped on Linux (no EGL), so it reported green for weeks. First time it
  actually ran, on Windows, it aborted the process via `qFatal` before a
  single assertion. It has been retired along with the native window.
- **Browser caching masqueraded as "the file didn't save."** Spent a round
  debugging code that wasn't the code running. `simulation/server.py` now
  sends `no-cache` on HTML/JS/CSS.
- **Windows exposed a real leak Linux hid.** `pump.py` dropped a failed CSV
  recorder without closing it. POSIX doesn't care; Windows can't delete an
  open file. Fixed — but the lesson is that platform-specific failures are
  often real bugs, not test noise.
- **"Identical by construction" was unenforced.** LoRa and SD getting the
  same bytes was true by reading the code, but SD was compiled *out* of the
  tests. `tools/test_paths_agree.py` now proves it, including a leg that
  confirms the check would actually fail if they diverged.
- **Three parallel subagents all died** (session limit / API error). One left
  `telemetry_hub_v2.c` unwritten. Run agents one at a time here.

---

## Known-unverified — flag these, don't trust them

- **ODrive `Get_Temperature` CAN command ID** (`firmware/sensors/odrive_can.c`)
  is an educated guess. The other three IDs are from the official reference.
  Check against `can_simple.dbc` in the ODrive firmware repo.
- **BNO080 Q-point scaling** (`imu_bno080.c`) is from the SH-2 manual, never
  run against hardware. Symptom of it being wrong: readings off by a clean
  power of 2.
- **LoRa throughput (~200 B/s)** came from a code comment and has never been
  measured. v2's 80-byte frame sits at ~80% link utilisation on that
  assumption. If the real number is lower, v2 does not fit at 2 Hz —
  fallbacks are ranked in `V2_DESIGN_NOTES.md`.
- **`brake_resistor_w`** is inferred from negative bus current, not measured.
  No such CAN message exists.

---

## Open decisions needing Tate, not code

1. **How node 3 (E-CVT/Motor) gets populated.** It has no physical board —
   the hub both receives ODrive frames and originates that node's record.
   Agreed approach: hub does nodes 0 and 3, two Bluepills do Front and Rear,
   third Bluepill is a spare. Not yet implemented.
2. **ODrive `axis_node_id`** must not collide with the `0x200` block v2 uses
   for node pages.
3. Whether to push to GitHub before or after the remaining cleanup.

---

## Immediate next steps, in order

1. **Commit to git.** Weeks of work exist on one disk with zero commits.
   `.gitignore` already excludes `pc_app/.browser-profile/` (1044 files
   including Chromium `Login Data` and `Vpn Tokens` — app-generated, but it
   must not be pushed). Verify with
   `git ls-files --cached | findstr browser-profile` → must print nothing.
2. **Write `telemetry_hub_v2.c`.** The `.h` exists and defines the contract.
   Biggest blocker for v2 on hardware.
3. **Docs consistency sweep.** Several files still describe a four-dashboard
   gallery; it is now one tabbed app (`simulation/dashboards/index.html`).
4. Bench stages 3 (SD) and 4 (fault recovery) — hardware, needs Tate.
5. Tate offered to send schematics for a wiring audit against `PINOUT.md`.

---

## Working with Tate

Prefers concise, direct answers. Moves fast, often changes direction
mid-thread (four dashboards → one tabbed app; keep the native window →
retire it) — follow the new direction, don't relitigate.

He is doing real hardware work with real failure modes, so **precision
matters more than reassurance**. He has been well served by being told
plainly when something is unverified, and poorly served by confident claims
that turned out to rest on untested code. When something is a guess, say so.

He is not a professional software engineer — explain *why* a thing matters,
not just what to type.
