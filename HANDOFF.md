# Handoff — read this first

Written 2026-08-12, updated 2026-08-14 for whoever (or whatever) picks this
up next. Assume no memory of prior conversations. This file is the
orientation; the other docs are the detail.

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

**There are three versions now: v1, v2, v3. v2 is wired into real projects as
of 2026-08-14 but has never been flashed or run on the bench yet.**

| | v1 | v2 | v3 |
|---|---|---|---|
| Status | **Working on real hardware**, verified end to end | Wired into real CubeIDE projects, **not yet flashed/bench-tested** | Not started — real sensors on top of v2 |
| Channels | 9 placeholder | 24, but still **simulated in firmware**, not from real sensors | 24, real sensor reads |
| Nodes | 3 (all Bluepills) | 4 (Hub, Front, Rear, Motor — see board mapping below) | same as v2 |
| Frame | 42 bytes @ 2 Hz | 80 bytes @ 2 Hz | 80 bytes @ 2 Hz |
| What's flashed where | All 3 Bluepills + Hub | **2nd Bluepill = Front, 3rd Bluepill = Rear, Hub = Nucleo, 1st Bluepill = temporary Motor/E-CVT stand-in for the bench test.** | same boards as v2, minus the stand-in once the real ODrive replaces it |

**Board mapping for v2 (this was an open decision as of 08-12, settled 08-13,
then extended 08-14):** in the *final* design, node 0 (Hub/GPS/IMU) and node 3
(E-CVT/Motor) have no physical Bluepill — the hub originates both directly
(real GPS/IMU/ODrive in v3). Only Front and Rear are permanent Bluepill
roles.

**Temporary addition, 2026-08-14 — a 4th "board" for the v2 bench test only:**
the 1st Bluepill (previously the idle spare) is flashed with `node_id.h` set
to `NODE_ID 3` and now transmits real `tlm2_can_pack_pages()` bursts for the
Motor node, so the hub's reassembler gets exercised by an actual 4th CAN
transmitter instead of leaving node 3 hub-internal during the test. This is
**not** what node 3 looks like in the real system — the ODrive doesn't speak
this protocol at all, it speaks CAN Simple, addressed by `axis_node_id`, not
`TLM2_CAN_ID_FOR(3, page)`. Swapping in the real ODrive later means removing
this stand-in board from the bus entirely, not reflashing it to "the real
thing" — see `can_node_v2.h`'s header comment for the full reasoning. The
hub-side switch is `HUB2_SIMULATE_NODE3` in `telemetry_hub_v2.h`, set to `0`
in the Nucleo project's `main.c` right now specifically because this stand-in
is on the bus — **if you ever pull this board off the bus without also
flipping that back to `1` (or unset), node 3 goes stale/nothing shows up,
not "back to simulated."** The 1st Bluepill is not a working v1 fallback
spare while wearing this hat — see the note in its `node_id.h`.

**Two separate build systems, two separate exclusion lists — this bit us
2026-08-14.** Every project has both a CubeIDE `.cproject` and a
`platformio.ini`, and PlatformIO does **not** read `.cproject` at all — it
has its own `build_src_filter` that compiles everything under `Core/Src/` on
its own. Excluding a stale v1 file from one build system does nothing for
the other. Two exclusions were needed and are now both in place: the
Nucleo's `telemetry_hub.c` (duplicate `HAL_CAN_RxFifo0MsgPendingCallback` at
link time) and the 1st Bluepill's `can_node.c` (v1's `can_node.h` hard-errors
at compile time for `NODE_ID 3`, which is outside v1's valid 0-2 range, even
though nothing calls into that file). `tools/test_repo.py` now checks both
`platformio.ini` files for these exclusions specifically so this can't drift
back silently. **None of this has been compile-verified against the real ARM
toolchain from this end** — there's no `arm-none-eabi-gcc`/PlatformIO
available in the environment doing this work, only the host-side C tests
(hal_shim-mocked, not real HAL). The PlatformIO build output is the first
real compile check any of this has had — keep pasting errors if more show up.

**What "v2 wired in" actually means right now:** `can_node_v2.c`/`.h` and
`telemetry_hub_v2.c`/`.h` (all pre-existing, host-tested library code) are now
copied into the 2nd Bluepill, 3rd Bluepill, and Nucleo hub CubeIDE projects,
and each project's `main.c` calls the v2 init/task functions instead of v1's.
`telemetry_hub.c` is excluded from the hub project's build (both files define
a strong `HAL_CAN_RxFifo0MsgPendingCallback`, so linking both is a link
error). **None of this has been flashed to hardware or bench-tested yet** —
that's the next real milestone, and per the "verify, don't assert" rule nothing
above should be taken as proof it works until it's been run.

v1 files are untouched throughout — `protocol/telemetry_proto.{h,c}`,
`can_node.c`, `telemetry_hub.c`, and their mirrored copies in every project
are byte-identical to before. Reflashing any board back to v1 is always just
"go back to what main.c called before."

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
  bluepill_node/   can_node.c (v1), can_node_v2.c (v2 — now wired into the
                   2nd/3rd Bluepill projects' main.c, not yet bench-tested)
  nucleo_hub/      telemetry_hub.c (v1), lora_e22.c, sd_log.c, sd_spi.c,
                   fat32.c, telemetry_hub_v2.c (v2 — now wired into the
                   Nucleo project's main.c, not yet bench-tested)
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

1. ~~How node 3 (E-CVT/Motor) gets populated.~~ **Settled and implemented
   2026-08-14**: the hub originates nodes 0 and 3 itself (simulated for now),
   Front and Rear are the two real Bluepills, the 3rd (1st-numbered) Bluepill
   is the spare.
2. **ODrive `axis_node_id`** must not collide with the `0x200` block v2 uses
   for node pages. Still open — `HUB2_ODRIVE_AXIS_NODE_ID` defaults to 0 in
   `telemetry_hub_v2.h`; confirm against whatever you actually set on the
   ODrive with `odrivetool` before `HUB2_USE_REAL_ODRIVE` ever gets flipped on.
3. Whether to push to GitHub before or after the remaining cleanup — repo now
   has 4 commits locally (see below), still not pushed anywhere.

---

## A repo-corruption risk found and worked around, 2026-08-14

This repo lives inside OneDrive, and the "OneDrive syncing `.git` mid-op can
corrupt things" warning in `NEXT_STEPS.md` turned out to be real, just not in
the way expected: a sandboxed agent working on this repo could create git
lock files (`.git/index.lock`, `.git/HEAD.lock`, `.git/objects/*.lock`) but
**could not delete them afterward** — not a git problem, a general inability
to delete anything on this OneDrive mount from that environment. Every git
write left a stray lock behind that blocked the next one, and only Tate,
working locally, could clear it. One commit went through per "session" before
needing a manual unstick. If this happens again: close anything that might
hold a git handle (IDE, GitHub Desktop, a stray terminal), then delete the
`*.lock` files under `.git/` by hand. This is a good argument for finishing
the move off OneDrive (see NEXT_STEPS.md's "About OneDrive" section) sooner
rather than later.

## Immediate next steps, in order

1. **Bench-test v2.** This is the real milestone, not yet done: flash all
   four boards — 2nd Bluepill (Front), 3rd Bluepill (Rear), 1st Bluepill
   (temporary Motor/E-CVT stand-in, `NODE_ID 3`), and the Nucleo hub — with
   their now-v2 `main.c`. There is no v1 spare on the bus during this test.
   Run `python run.py --proto v2 --port COM9` from `pc_app/` and confirm all
   24 channels show up live, sourced from real hardware rather than the
   Python simulator — including the Motor node, which should now be coming
   from the 1st Bluepill's real CAN frames, not the hub's local simulation
   (`HUB2_SIMULATE_NODE3` is `0` for this test). `tools/run_all_tests.py` is
   green (15/15) at the library level, but that only proves the code is
   internally consistent — it says nothing about the real CAN bus, the real
   LoRa link, or real mailbox contention with 4 transmitters on one bus.
2. **Measure real LoRa throughput** once v2 is flashed. Everything about the
   80-byte frame fitting at 2 Hz rests on an unmeasured ~200 B/s assumption —
   see `protocol/V2_DESIGN_NOTES.md` for the ranked fallbacks if it doesn't.
3. **Commit and push.** 4 commits exist locally now (baseline, hub v2, the
   sensor-header/CubeMX v3 start, and the .gitattributes line-ending fix);
   still nothing on GitHub. `.gitignore` already excludes
   `pc_app/.browser-profile/` — verify with
   `git ls-files --cached | findstr browser-profile` → must print nothing —
   before pushing.
4. Bench stages 3 (SD) and 4 (fault recovery) on v1 — hardware, needs Tate,
   still not done regardless of the v2 work above.
5. Once v2 is proven on the bench, v3 starts: wire real sensors into
   `can_node_v2.c`'s `sim_read_channels()` and `telemetry_hub_v2.c`'s
   `hub2_originate_node0/3()` one at a time, per `firmware/NODE_INTEGRATION_V2.md`.
6. Tate offered to send schematics for a wiring audit against `PINOUT.md`.

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
