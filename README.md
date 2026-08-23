# CAN → LoRa Telemetry

Three STM32F103 (Bluepill) nodes push sensor data onto a 500 kbit/s CAN bus. An
STM32L476RG hub aggregates it, logs it to SD, and transmits snapshots over an
Ebyte E22 LoRa link. A Python ground station receives them on a USB dongle and
serves a live dashboard.

```
 Bluepill 0 ─┐                  ┌──► SD card  (LOGnnnn.TLM)
 Bluepill 1 ─┼── CAN 500k ──► L476RG hub
 Bluepill 2 ─┘                  └──► E22-900T22D ~))) E22-900T22U ──USB──► PC
                                        LoRa 919 MHz          dashboard + CSV
```

Sensor data is simulated for now — each Bluepill runs a built-in waveform
generator (`sim_read_channels()`) instead of reading a physical ADC/I2C
sensor. Swapping in real sensors means editing that one function and one
table (`TLM_CHANNELS`); nothing else in this v1 chain changes. (That's a
different thing from the `simulation/` folder described below, which is a
standalone synthetic-data server for dashboard development — it doesn't
touch this firmware at all.)

## What's running vs. what's scaffolding

This repo now has two very different kinds of content in it, and mixing them
up is the easiest way to get confused:

**Running today, on real hardware, verified end-to-end (CAN → LoRa → SD → PC
app):** everything described above — the v1 wire format (3 channels × 3
nodes, `protocol/telemetry_proto.c`), `firmware/bluepill_node/`,
`firmware/nucleo_hub/`, and `pc_app/`. The "Wire format", "CAN database",
"Tests", and "Hardware notes" sections below all describe this system, and
none of it changed as part of adding the folders below.

**Written and host-tested, but NOT wired into the running system and NOT
tested on any board:** the v2 wire format (24 channels × 4 nodes,
`protocol/telemetry_proto_v2.*`), the real sensor drivers
(`firmware/sensors/`), and the E-CVT control loop (`firmware/control/`).
Each has its own README that says so up front, and each host-tests clean
against a compiler — but nothing in `can_node.c` or `telemetry_hub.c` calls
into any of it yet. See `firmware/NODE_INTEGRATION_V2.md` for the process of
actually wiring one of these drivers in, once pins are assigned.

If a folder isn't in the first list, assume it's scaffolding for the next
phase of the project until its own README says otherwise.

## Layout

```
protocol/              wire format (C + Python) and telemetry.dbc          [v1 - RUNNING]
  telemetry_proto_v2.h/.c, V2_DESIGN_NOTES.md                              [v2 - SCAFFOLD]
firmware/
  CUBEMX_SETUP.md      <- verify the .ioc edits, then Generate Code
  NODE_INTEGRATION_V2.md   wiring a sensors/ driver in via v2              [v2 - SCAFFOLD]
  bluepill_node/       CAN node source                                     [v1 - RUNNING]
  nucleo_hub/          CAN aggregation, LoRa TX, SD logging                [v1 - RUNNING]
  sensors/             real sensor driver skeletons (GPS, IMU, encoders,   [SCAFFOLD]
                        suspension pots, brake pressure, CVT temp, ODrive)
  control/             E-CVT PI control loop + IMU hill feedforward        [SCAFFOLD]
  projects/            your five CubeIDE projects, with the source already added
pc_app/                ground station: serial reader, dashboard, CSV, build.bat  [v1 - RUNNING]
simulation/            synthetic full-sensor-suite feed + judging dashboards,
                        decoupled from firmware/protocol/pc_app entirely
field_data/            real .TLM/.csv logs from actual test sessions go here
tools/                 test suites and the log converter
```

`firmware/projects/` holds copies of your four active projects: the three
Bluepill nodes and the Nucleo hub. A 4th Bluepill project existed early on
(the system was originally 4 CAN nodes) but was deleted 2026-08-16 once the
system settled on 3 nodes and v2 bench testing confirmed it wasn't needed.
The originals in `Documents\` are untouched.

## Ground station

### Run from source

```bash
cd pc_app
pip install -r requirements.txt
python run.py
```

With no arguments it looks for the E22 dongle, falls back to the simulator if
there isn't one, picks a free HTTP port, and opens the dashboard. The simulator
is a port of the firmware's waveform generator, so this is what the real
system looks like.

```bash
python run.py --sim --drop-rate 0.15 --offline-node 2   # exercise loss and stale handling
python run.py --list-ports                              # see what's plugged in
python run.py --port COM7 --csv --raw                   # pin a port, record both formats
python run.py --replay logs/LOG0001.TLM --speed 4       # replay an SD log
python run.py --web --host 0.0.0.0                      # view from a phone on the network
```

### Cleaning up old ground-station logs

Every run writes a new rotating session log into `pc_app/logs/` (or next to
`Telemetry.exe` if you're running the frozen build), and nothing ever prunes
them on its own — left alone they just accumulate. Five flags handle it:

```bash
python run.py --list-logs                             # inventory: name, size, age
python run.py --clean-logs                             # prune, keeping the 10 newest, then exit
python run.py --clean-logs --keep 20                   # keep the 20 newest instead
python run.py --clean-logs --older-than 30              # delete anything older than 30 days
python run.py --clean-logs --older-than 30 --dry-run    # preview only, deletes nothing
```

`--keep` and `--older-than` can combine; with neither given, `--clean-logs`
defaults to `--keep 10`. There's also a **Ground station logs** panel in the
`--web` dashboard with Preview/Delete buttons, for anyone who'd rather not
use the command line.

This deliberately only ever touches `logs_dir()` — the ground station's own
disposable session logs. It has no path into `field_data/` at all, by design:
those are real SD-card captures pulled off the car, and unlike a log file
`run.py` can regenerate on the next launch, a lost field capture is gone for
good.

### The dashboard, and why it isn't the Qt window any more

**The browser dashboard is the default.** `run.py` serves
`pc_app/static/index.html` from a small aiohttp server and opens it in Edge or
Chrome's `--app` mode — its own window, its own taskbar entry, its own browser
profile so it doesn't hijack a tab in whatever browser you already have open.
Four dashboard designs live in `simulation/dashboards/` and are served at
`/dashboards/`; the original single view is at `/`. See
`telemetry/launcher.py` for the detection and fallback chain, and
`telemetry/server.py` for the WebSocket server.

```bash
python run.py                          # dashboard, app-mode window
python run.py --host 0.0.0.0           # reachable from a phone on the same wifi
python run.py --tab                    # a normal browser tab instead of app-mode
```

**The PySide6 (Qt) native window is retired.** It used to be the default, and
`telemetry/qt_app.py` still works behind `--native`, but nothing tests it and
it only ever spoke v1's 3-node channel set — it does not understand
`--proto v2`. It was retired for one reason worth recording: it pulled a heavy
GUI dependency into the build for a single window, while the browser dashboard
already had to exist anyway for phone access. Keeping both meant maintaining
two UIs against a channel table that was actively changing, and only one of
them had tests.

`telemetry/pump.py` — the frame-consuming logic both UIs share — is unchanged
and still covered by `tools/test_qt.py`.

### Build Telemetry.exe

```
cd pc_app
build.bat
```

Produces `dist\Telemetry.exe` — one file, no install needed on the target
machine — and drops a **Telemetry** shortcut on your Desktop pointing at it.
The script installs PyInstaller and `PySide6-Essentials` (deliberately not the
`PySide6` umbrella package — see below), runs the wire-format tests, builds,
smoke-tests the result both ways (the app comes up and stays running; `--web`
serves the dashboard and answers `/api/status`), then creates the shortcut.

Two things about this script worth knowing:

- Its closing message still describes the retired native window as the default
  launch mode. The build itself is fine; that text is stale.
- It deliberately does **not** pass PyInstaller's `--clean`. When this tree
  lives in a synced folder (OneDrive) or on a machine running HP Sure Click,
  something intermittently holds an open handle on
  `build/telemetry/localpycs`; `--clean` treats failing to delete that scratch
  directory as fatal and kills the whole build with `WinError 5`. A
  best-effort `rmdir` replaces it, so a transient lock costs you a slightly
  less pristine build instead of a failed one. For a guaranteed-clean build,
  pause sync, delete `build\` by hand, and re-run.

**Why `PySide6-Essentials` and not `PySide6`:** the umbrella package pulls in
`PySide6-Addons`, which bundles `QtWebEngine` — a full embedded Chromium. The
native window only imports `QtCore`/`QtGui`/`QtWidgets`, all of which live in
Essentials, so Addons is never installed and there is nothing Chromium-shaped
anywhere in this app. `telemetry.spec` excludes the Addons modules by name as
a second layer of defense in case a build machine happens to have the full
`PySide6` installed anyway.

**This has to run on Windows.** PyInstaller freezes the interpreter of the
machine it runs on; it cannot cross-compile a Windows executable.

Just want the shortcut re-created (moved the exe, deleted the shortcut,
whatever) without a full rebuild:

```
cd pc_app
make_shortcut.bat
```

You can also skip the shortcut entirely and drag `dist\Telemetry.exe` straight
onto your Desktop — it's fully self-contained, single-file, and will happily
run from wherever you put it; its logs, CSVs and `settings.json` just end up
next to it, wherever that is.

Logs, CSVs and `settings.json` are written next to the `.exe`, not into the
temp directory PyInstaller unpacks into — see `telemetry/paths.py`. The console
window stays open deliberately: it's where you find out which port it opened
and whether the dongle was detected.

### The UI is a starting point

Both front ends are intentionally plain, and both keep their visuals separate
from their logic so either can be restyled without touching how data flows.

**Native window** (`telemetry/qt_app.py`, the default): every colour is one of
the `COLOR_*` constants near the top of the file, and each visual piece is its
own small widget class (`Sparkline`, `ChannelRow`, `NodePanel`) with no frame-
handling logic mixed in — that all lives in `telemetry/pump.py` and the
`_on_frame`/`_on_status` handlers, which you shouldn't need to touch to
restyle. Ordinary Qt stylesheets (`setStyleSheet(...)`) work throughout.

**Browser dashboard** (`pc_app/static/index.html`, `--web`): all colours,
spacing and fonts are CSS variables in one block at the top, and the markup
uses stable class names (`.bar`, `.node`, `.chan`, `.spark`) that the script
targets. Set `SPARKLINES = false` near the top of the script to drop the
charts.

## Firmware

**Start with [`firmware/CUBEMX_SETUP.md`](firmware/CUBEMX_SETUP.md).** The
`.ioc` files have been edited (CAN timing, bus-off recovery, Serial Wire,
USART1 + DMA, GPIO). Open each project in CubeMX and run the short verification
list there before generating — I can't run CubeMX, so I can't confirm it parses
every key, and an unrecognised key is dropped silently.

The source files are already in place in each project under
`firmware/projects/`, along with `main.c`, MSP and interrupt files that build
and run as-is if you never open CubeMX. Regenerating replaces those with
CubeMX's own equivalents, which is fine — the calls the firmware needs live in
`USER CODE` blocks and survive.

Per-board integration notes:
[bluepill_node](firmware/bluepill_node/INTEGRATION.md) ·
[nucleo_hub](firmware/nucleo_hub/INTEGRATION.md)

This is all v1 — three boards, running today. For the v2 scaffold (real
sensor drivers, the 24-channel protocol, the E-CVT controller), see
"Real sensor drivers & E-CVT control" and "v2 protocol" below, and
[`firmware/NODE_INTEGRATION_V2.md`](firmware/NODE_INTEGRATION_V2.md) for the
step-by-step of actually wiring a driver in.

## Wire format

One 42-byte frame every 500 ms, carrying all three nodes:

```
off  size  field
  0     2  sync    0xA5 0x5A
  2     1  version
  3     1  payload length
  4     2  frame sequence
  6     4  hub uptime, ms
 10    30  3 × node record (id, flags, can_seq, loss, ch0, ch1, ch2)
 40     2  CRC-16/CCITT-FALSE over bytes 2..39
```

Why snapshots rather than forwarding raw CAN frames: the bus carries
3 nodes × 10 Hz × 8 bytes = 240 B/s of payload, while the E22 at its default
2.4 kbps air rate delivers roughly 200 B/s. Forwarding raw frames cannot work
at any buffer size — the link is simply narrower than the bus. Snapshotting the
latest value from each node decouples the two rates, and the SD log keeps the
same frames so a dropped radio packet costs nothing.

The SD log and the radio stream are byte-identical, which means
`python run.py --replay` runs a recorded session through the exact code path
that handled it live.

## v2 protocol (scaffold, not wired in)

`protocol/telemetry_proto_v2.h/.c` is a second, independent wire format
(`TLM2_`/`tlm2_` namespaced, zero symbol overlap with v1) built for the
24-channel, 4-node layout the finished car will actually carry (see
`simulation/vehicle_data.py`). v1's 8-byte CAN payload and 10-byte radio
record are fixed-shape with no room to say "how many channels" or "how
wide", so rather than change what the bytes mean on the proven v1 format,
v2 gets its own version byte and a multi-frame ("paged") CAN scheme: a node
with more than 3 channels sends up to 4 consecutive 8-byte CAN frames
("pages") per transmission cycle instead of one.

**Nothing in `firmware/` or `pc_app/` calls into this yet.** It does have a
standalone host test, run directly with a C compiler (not part of
`tools/run_all_tests.py` — see below):

```bash
gcc -std=c99 -Wall -Wextra -Werror -I protocol protocol/telemetry_proto_v2.c tools/test_roundtrip_v2.c -o /tmp/test_roundtrip_v2 -lm
/tmp/test_roundtrip_v2
```

That's 1499 checks, all passing, covering the full pack → CAN pages →
reassemble → snapshot → radio-encode → decode pipeline, torn bursts, epoch
wraparound loss counting, CRC corruption/resync, and GPS int32 precision.
There is no `proto_v2.py` yet, so the PC app cannot decode a v2 frame even
if one showed up on the wire.

Full design writeup — CAN paging, the epoch-based torn-set detector, why
GPS lat/lon needs `int32`, LoRa airtime budget at the larger 80-byte frame
size, the CAN ID collision risk with the ODrive's own addressing, and a list
of open questions — is in `protocol/V2_DESIGN_NOTES.md`. For the actual
mechanics of taking a driver from `firmware/sensors/` and wiring it through
this protocol onto a CAN node, see `firmware/NODE_INTEGRATION_V2.md`.

## CAN database (DBC)

`protocol/telemetry.dbc` describes the bus for SavvyCAN, BusMaster, Vector
tools, python-can/cantools, and anything else that reads a DBC.

One file, not one per node. A DBC describes a *network*: it lists the nodes
(`BU_`) and every message on the bus (`BO_`), each tagged with its sender. Three
nodes means three messages in one file — splitting it per node would produce
three fragments each describing a bus with one participant, which no tool wants.

```
BU_: NODE0_ENV NODE1_PWR NODE2_MOT HUB

BO_ 256 NODE0_ENV_DATA: 8 NODE0_ENV
 SG_ temp_c : 0|16@1- (0.01,0) [-327.68|327.67] "degC" HUB
 ...
```

It is **generated**, not hand-maintained:

```bash
python tools/gen_dbc.py
```

Edit `TLM_CHANNELS` in `protocol/telemetry_proto.c` (and the mirrored
`CHANNELS` in `proto.py`), then regenerate. `tools/test_dbc.py` fails if you
forget, and separately decodes 200 firmware-packed payloads through cantools to
confirm the DBC and the firmware agree. That last check is what catches a byte
order mistake — get `@1` vs `@0` wrong and every signal still decodes, just to
plausible-looking nonsense.

Status bits are broken out as named signals (`sensor_fault`, `startup`,
`can_error`) rather than one opaque byte, so a trace viewer shows you what
happened instead of `status = 5`.

## Tests

```bash
python tools/run_all_tests.py
```

**2380 checks, all passing.** Eight suites:

| Suite | What it proves |
|---|---|
| `test_roundtrip.py` | Compiles `telemetry_proto.c` with a host compiler at `-Werror` and compares it against `proto.py` byte for byte across 64 generated frames, plus CRC vectors, corruption rejection, stream resync, sequence wraparound. **359 checks.** |
| `test_fat32.py` | Formats real FAT32 images with `mkfs.vfat` across 7 cluster-size and partition layouts, runs the writer, validates with `fsck.fat` **and** an independent parser that walks the cluster chain and compares every byte. **199 checks.** |
| `test_firmware.py` | Compiles the real `can_node.c` and `telemetry_hub.c` against a host HAL shim with `HAL_GetTick()` under test control. Transmit cadence, mailbox exhaustion, bus-off recovery, aggregation, loss arithmetic across the 8-bit wrap, node timeouts, ID filtering, ring buffer overflow. **37 checks.** |
| `test_app.py` | Ground station backend: path resolution when frozen, a settings file hand-edited into nonsense, the HTTP port already taken, and the USB dongle unplugged mid-session against a fake pyserial that fails on cue. **57 checks.** |
| `test_qt.py` | `FramePump` (shared by both front ends): meta/status shape, frame consumption + CSV recording + stats bookkeeping against a real asyncio loop, a source that raises unexpectedly, a CSV write that fails mid-run. Then builds the actual PySide6 `MainWindow` headlessly (`QT_QPA_PLATFORM=offscreen`) and drives it through a real background worker thread with a real asyncio loop, proving frames cross into the GUI thread via Qt signals rather than just type-checking the plumbing. Skips the window half cleanly if PySide6 or its offscreen platform plugin isn't available. **40 checks.** |
| `test_dbc.py` | Decodes 200 firmware-packed CAN payloads through cantools and compares against the channel table; checks scale, offset, unit, sign, length and byte order on every signal, plus the encode direction. **1499 checks.** |
| `test_repo.py` | Structural audit: vendored copies byte-identical to canonical sources, node IDs unique and ordered, `.ioc` keys present with CRLF intact, `main.c` wired up, old firmware removed, your originals untouched, and that the PySide6 packaging (`requirements.txt`, `build.bat`, `telemetry.spec`) never pulls in `QtWebEngine`. **164 checks.** |
| `test_launcher.py` | App-window launch logic (used by `--web`) with the registry, filesystem and process spawn all faked: browser search order, a stale registry entry pointing at a deleted install, a launch that fails outright, non-Windows fallback. **25 checks.** |

The first suite is the one worth keeping green. The firmware and the ground
station never see each other's source, so a disagreement about a struct offset
or an endianness assumption doesn't crash — it produces a dashboard full of
plausible-looking wrong numbers.

Needs a host C compiler and `dosfstools`. Suites skip cleanly if either is
missing.

**Not covered:** `sd_spi.c` is register-level SD/SPI code that only means
anything on silicon, and the LoRa link itself. Those get verified on the bench
— see the staged test order at the end of the hub's INTEGRATION.md.

**Two more host tests exist for the v2/scaffold code, deliberately not in
this manifest:** `tools/test_ecvt_control.py` (builds `ecvt_control.c`
against gcc/clang, no HAL — flat-ground convergence, a 15%-grade
feedforward step, sensor-dropout handling; **17 checks**, all passing) and
`tools/test_roundtrip_v2.c` (v2 wire format round-trip, run directly with a
C compiler — see the "v2 protocol" section above; **1499 checks**, all
passing). Both are standalone by design because the code they test hasn't
run on real hardware yet — see `firmware/control/README.md` and
`protocol/V2_DESIGN_NOTES.md` for why folding them into
`run_all_tests.py` is left as a follow-up rather than done here.

## Issues found in the original projects

| Issue | Effect | Fix |
|---|---|---|
| `SYS: No_Debug` in all four Bluepill `.ioc` files | frees PA13/PA14, killing SWD after the first flash; recovery needs a BOOT0 jumper | Debug → Serial Wire |
| `AutoBusOff = DISABLE` on all five | one bus fault leaves a node permanently mute with no indication | Enable it |
| CAN sample point mismatch: 77.8 % on Bluepills vs 87.5 % on the Nucleo | both hit 500 kbit/s so it works on the bench; erodes margin for oscillator drift and cable length | Bluepill Prescaler 9 / BS1 6 / BS2 1 → 87.5 % |
| blocking `HAL_UART_Transmit` inside the CAN RX ISR | ~5.5 ms in interrupt context per frame; survivable at 20 msg/s, breaks once SD writes are added | ISR pushes to a ring buffer, main loop does the work |
| `HAL_Delay(200)` in the node main loop | CPU parked in a busy wait, cannot service anything else | tick-driven, non-blocking |
| accept-everything CAN filter | unrelated bus traffic reaches the CPU | mask to 0x100–0x103 in hardware |
| no CRC, no timestamp, raw `float` on the wire | undetectable corruption over a lossy radio | CRC-16, hub uptime, scaled `int16` |

## Hardware notes

**The E22 part number matters.** `-U` is a USB dongle for the PC. The hub needs
`-D` or `-S`, which have UART pins.

**Give the E22 its own supply.** Over 600 mA in transmit bursts at 22 dBm, more
than the Nucleo's regulator provides over USB. 470 µF or more at the module.
Getting this wrong looks like a link that works on the bench and dies when you
raise power or packet length.

**US frequency.** Factory default is 868.125 MHz (EU band). Use channel 0x45 →
919.125 MHz to stay inside US 902–928 ISM. Same channel on both ends.

**CAN bus.** Every node needs a transceiver. Exactly two 120 Ω terminators, at
the two physical ends — not one per board.

**SD card.** No FatFs and no SPI HAL in your project, and neither can be added
without regenerating with new middleware. So the repo carries `sd_spi.c`
(register-level block driver) and `fat32.c` (append-only FAT32 writer, ~600
lines). The card comes out readable on any OS. Swap back to FatFs later if you
want the full filesystem — `sd_log.c` is the only file that would change.

## Real sensor drivers & E-CVT control (scaffold, not wired in)

`firmware/sensors/` has driver skeletons for the full sensor suite the
finished car will carry: GPS (u-blox NEO-M8N), 9DOF IMU (BNO080), wheel
encoders (Littelfuse 55075 ×4), suspension pots (Bourns 53AAA-B28-B15L ×4),
a brake pressure transducer (Anfield T200/T201), CVT temperature
(MLX90614), and ODrive S1 motor telemetry over CAN. Every driver reads its
sensor and hands back a plain `float` in engineering units (mph, mm, psi,
degC, g, deg/s) — none of them know anything about the CAN wire format, and
**none of them are called from `can_node.c` or `telemetry_hub.c` yet**, on
purpose: pins aren't assigned, and `firmware/sensors/README.md` deliberately
leaves that decision for you rather than guessing. Every file has `TODO`
comments marking exactly what needs a real pin, calibration constant, or
datasheet check.

Not all drivers carry equal confidence — `firmware/sensors/README.md` rates
each one. The ADC/GPIO drivers (wheel encoder, suspension pot, pressure
transducer) and the MLX90614 are high-confidence, standard patterns.
`odrive_can`'s three core CAN command IDs are from the ODrive protocol
reference and should be solid; its `Get_Temperature` command ID is an
educated guess that needs checking against the ODrive's own `can_simple.dbc`
before trusting `motor_temp`, and `brake_resistor_w` is a rough estimate
from regen current, not a real measurement. `imu_bno080` is the most
involved sensor here — the SH-2 protocol plumbing is structurally correct,
but its Q-point scaling constants are the first thing to re-check if
readings come back off by a clean factor like 2x or 4x.

`firmware/control/` is a PI control loop (`ecvt_control.c`) that holds
engine RPM at a target by commanding the CVT's ODrive ratio actuator, with
an IMU-based feedforward term that anticipates a hill before RPM actually
sags. It's pure math — HAL-free, no CAN/I2C/ADC — and every gain in
`ecvt_control_config_t` has a `TODO` and a placeholder value, not a tuned
one. **Nothing here has touched a real ODrive, engine, or CVT.** There's
also no engine RPM driver in this repo yet (`measured_rpm` is just a `float`
parameter), no wheel-slip detection for the grade estimator, and no
actuator-position confirmation — see "What this does NOT handle yet" in
`firmware/control/README.md` for the full, deliberately honest list.

Once pins are assigned and you're ready to actually wire a driver into a
node, see **[`firmware/NODE_INTEGRATION_V2.md`](firmware/NODE_INTEGRATION_V2.md)**
— it walks through CubeMX peripheral setup, filling in a driver's TODOs, and
getting a reading onto the v2 wire format, with a full worked example.

## Simulation & dashboards (synthetic data, no hardware needed)

`simulation/` is a synthetic data generator for the full 24-channel sensor
suite above — not the 3-channel placeholder protocol the firmware actually
speaks today. It exists so dashboards can be designed and judged before
every sensor is physically wired up, and it does not import from or modify
`firmware/`, `protocol/`, or `pc_app/telemetry/` at all — it only reads
static HTML/CSS/JS out of `pc_app/static/` to render against.

```bash
cd simulation
python run_sim.py
```

The UI is **one single-page app** — `simulation/dashboards/index.html`, in the
Pit Wall style (dark instrument panels, amber readouts, riveted bezels). Seven
tabs, all fed from one WebSocket and one shared store, so switching tabs never
drops history or reconnects:

| Tab | Key | What it shows |
|---|---|---|
| **Live** | `1` | Every channel as a large amber readout, with an inline sparkline and running min/max/avg per channel. |
| **Gauges** | `2` | Sweep-arc dials for the 22 channels that have a sensible range. |
| **Schematic** | `3` | Plan view of the car with each value drawn where its sensor physically sits, so a bad corner reads as a position rather than a table row. |
| **Charts** | `4` | Chart-recorder lanes on a shared time axis, for correlations a grid of numbers hides — a brake spike lining up with a wheel-speed drop. |
| **Map** | `5` | Draws the course from GPS as you drive it. Local equirectangular projection and a scale bar; no map tiles and no network, so it works at a track with no signal. |
| **Session** | `6` | Min / max / average / current for every channel since the session started, with each channel's alarm threshold. |
| **Logs** | `7` | Archive index of `field_data/`, from the server's `/api/logs` endpoint: size, frame count, duration, and a copy-to-clipboard replay command. |

Beyond the tabs:

- **Alarm limits** — channels outside a sane range (CVT or motor overheating,
  pack voltage out of band, GPS fix degraded) turn red and raise a header
  chip. Thresholds live in `LIMITS` near the top of the script; they are
  "something is wrong" bounds, not performance targets, because a dashboard
  that cries wolf gets ignored.
- **Freeze** (`F`) holds the display so you can read a value that is moving,
  while data keeps arriving behind it.
- **Export CSV** writes the whole session — every channel, every frame — to a
  file from the browser, no server round trip.
- **Reset session** clears stats and history without reconnecting.
- The selected tab persists across reloads.

Frame counts on the Logs tab come from file size divided by frame size rather
than parsing, so a large log lists instantly — and a file whose size is not a
whole number of frames is flagged as truncated rather than quietly rounded,
since that is the signature of losing power mid-write.

The five earlier themed dashboards under `pc_app/static/themes/` are still
reachable directly off the same server (`/static/themes/modern.html`,
`orange.html`, `cyan.html`, `black.html`, `white.html`).

`simulation/server.py` speaks the identical meta/status/history/frame
WebSocket contract as the real `pc_app/telemetry/server.py`. Every dashboard
here (new or old) is built purely from the `meta` message's node/channel
list and hardcodes no channel name, node count, or label — see
`pc_app/static/js/telemetry-core.js` — so **any dashboard built against this
synthetic feed renders unmodified against the real ground station's `--web`
server, and vice versa.** `--host 0.0.0.0` lets teammates pull it up on
their own phones for side-by-side judging.

See `simulation/README.md` for the full sensor → node → channel mapping and
what's expected to happen once real sensors replace this.

## Field data

`field_data/` is where real captured `.TLM`/`.csv` logs from actual test
sessions go — not synthetic data (that's `simulation/`), and not scratch
bench-test output. Suggested layout is one `YYYY-MM-DD_description/`
subfolder per session, with a short `notes.md` alongside the log. Unlike the
rest of the repo, `.gitignore` carves out an explicit exception so
`field_data/**/*.tlm` and `field_data/**/*.csv` *do* get committed by
default. See `field_data/README.md`.

`python run.py --replay ../field_data/<session>/LOG0001.TLM` (from `pc_app`)
runs a captured session back through the exact decoder the live app used —
see `DATA_VERIFICATION_CHECKLIST.md` for how to confirm a given reading is a
genuine live measurement rather than stale or simulated data, once real
sensors are wired in.

## Other guides in this repo

- **[`BRINGUP_GUIDE.md`](BRINGUP_GUIDE.md)** — bench bring-up for the v1
  system: building and flashing all four boards, wiring, configuring the two
  E22 LoRa modules, and staged power-up.
- **[`PLATFORMIO_BMP_GUIDE.md`](PLATFORMIO_BMP_GUIDE.md)** — an alternative
  to STM32CubeIDE + ST-Link: building/flashing/debugging the same v1 source
  from VS Code + PlatformIO with a Black Pill flashed as a Black Magic Probe.
- **[`DATA_VERIFICATION_CHECKLIST.md`](DATA_VERIFICATION_CHECKLIST.md)** —
  how to confirm the pipeline is carrying data faithfully (applies now, even
  with the built-in simulator), and, later, how to confirm a given sensor
  reading is a genuine live measurement rather than stale or miswired.

## Mechanical / hardware notes and early design docs

This repo absorbed a second, earlier GitHub history on 2026-08-16 that
carried hardware and mechanical documentation this repo didn't otherwise
have — kept because it's real reference material, even though its firmware
and dashboard proposals are superseded by everything described above.

- **`nodes/`** — per-node hardware notes (Front, Rear, eCVT, Firewall):
  sensor part numbers (Littelfuse 55075 wheel encoders, Bourns
  53AAA-B28-B15L bellcrank pots, Anfield T200/T201 pressure transducers,
  MLX90614 CVT belt temp), connector types, and mechanical context. Cross-
  check part numbers here against `V3_CUBEMX_AND_SENSOR_INTEGRATION_GUIDE.md`
  before trusting either in isolation — they were written independently.
- **`can/`** — CAN bus topology notes and a `baja.dbc`. This repo's actual
  running DBCs are `protocol/telemetry.dbc` (v1) and
  `protocol/telemetry_v2.dbc` (v2) — treat `can/baja.dbc` as historical/
  planning reference, not the wire format actually on the bus today.
- **`hardware/`** — PCB, enclosure (PAHT-CF, IP66, Bambu P1S), and Deutsch
  DT/DTM connector notes for the physical build.
- **`suspension/`** — bellcrank travel-sensor CAD and linkage notes.
- **`docs/architecture.md`, `docs/build-log/`** — early system architecture
  writeup and a dated log of build decisions (CAN bus architecture, LoRa
  telemetry, node platform choice, a web-dashboard pivot, and receiver/scope
  corrections). Historical — some of it (the web-dashboard pivot log in
  particular) describes a FastAPI/SQLite browser dashboard that was dropped
  in this merge in favor of `pc_app/`'s native Qt window (see "Ground
  station" above) — the log entry itself is left as-is since it's a record
  of a decision made at the time, not a claim about what's running now.
- **`scripts/can_decode.py`** — a standalone `cantools`-based CAN log
  decoder, independent of `pc_app/` and `tools/`.
- **`LICENSE`** — MIT.

**Not carried over:** the old `telemetry/dashboard/` (a browser dashboard +
FastAPI/SQLite persistent-logging backend) was dropped entirely — `pc_app/`
is this repo's one real dashboard going forward, per the "Ground station"
section above.
