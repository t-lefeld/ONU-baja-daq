# ONU Baja — Data Acquisition (DAQ)

Sensors on the car send readings over a CAN bus to a central board, which radios them to a laptop in the pit over LoRa and writes a backup copy to an SD card. The laptop shows live gauges and records the session.

```
 Front node  ─┐
 Rear node   ─┼── CAN bus ──► Firewall node ──┬──► SD card (backup log)
 eCVT node   ─┤                               │
 Display node─┘                               └──► LoRa radio ~))) laptop in pit
                                                        live dashboard + CSV
```

---

## New here? Read this first.

**You do not need to know Git to use this repo.** Click any folder name on the GitHub page to look inside it. Click a `.md` file and GitHub displays it as a formatted page.

**Where do I go for...**

| I want to... | Go to |
|---|---|
| Understand what this system is | This page, the section below |
| Know what's actually built and working | [`PROJECT_STATUS.md`](PROJECT_STATUS.md) |
| Build and power up the boards for the first time | [`BRINGUP_GUIDE.md`](BRINGUP_GUIDE.md) |
| Work through bring-up board by board | [`V3_BRINGUP_CHECKLIST.md`](V3_BRINGUP_CHECKLIST.md) |
| Find a pin number | [`V3_CUBEMX_AND_SENSOR_INTEGRATION_GUIDE.md`](V3_CUBEMX_AND_SENSOR_INTEGRATION_GUIDE.md) |
| Know what to work on next | [`NEXT_STEPS.md`](NEXT_STEPS.md) |
| Take over this project from a graduating member | [`HANDOFF.md`](HANDOFF.md) |
| Check whether a reading on the dashboard is real | [`DATA_VERIFICATION_CHECKLIST.md`](DATA_VERIFICATION_CHECKLIST.md) |
| Find which sensor is on which node | The hardware table below, then [`nodes/`](nodes/) |
| Run the pit laptop software | "Ground station" below |
| Work on firmware | [`firmware/CUBEMX_SETUP.md`](firmware/CUBEMX_SETUP.md) |
| Add a real sensor to a node | [`firmware/NODE_INTEGRATION_V2.md`](firmware/NODE_INTEGRATION_V2.md) |

**Vocabulary you'll hit immediately**

- **CAN bus** — a two-wire network that all the boards share. Every board can talk on it; messages are tagged with an ID so boards know what they're hearing. Standard in cars.
- **LoRa** — long-range, low-speed radio. Gets data from the car to the pit, miles away. Low speed is why we send snapshots, not everything.
- **Node** — one board on the car doing one job.
- **Firewall node** — the central board. Named for where it mounts (the firewall between driver and engine), not for network security.
- **Bluepill** — a cheap, common STM32F103 microcontroller board. Used for the simpler nodes.
- **DBC file** — a text file listing every CAN message and how to decode its bytes into real numbers. The decoder ring for the bus.
- **Ground station** — the laptop software in the pit.
- **Bring-up** — powering up new hardware for the first time, in a careful order.

---

## Hardware: what's on the car

Five nodes share one 500 kbit/s CAN bus.

### Firewall node — the hub

Custom PCB based on the STM32L476RG (Nucleo-variant design, not a stock Nucleo board).

| Part | What it does |
|---|---|
| E22-900T22D LoRa module | Radios data to the pit |
| HCLRC M100 GPS + external active antenna | Position and speed |
| BNO080 9-DOF IMU | Orientation, acceleration, rotation (some docs say BNO085; interchangeable here) |
| CAN analyzer | Watches all bus traffic |
| SD card | Backup log, so a dropped radio packet costs nothing |

The firewall node also distributes power to the entire DAQ system. See "Power" below.

**Status:** V1 PCB is done, built for validation only — no ORing circuit, fewer on-board sensors than the final board. The voltage monitor comes in a later revision. Enclosure is designed. No temperature sensor on this node.

### Front node — in the toebox, mounted to the frame

STM32F103 Bluepill.

| Part | What it measures |
|---|---|
| Littelfuse 55505-00-02-A-ND gear tooth sensor ×2 | Wheel speed, one per front wheel |
| AS5600 Hall-effect encoder on the bellcrank ×2 | Suspension travel, one per corner (analog output) |
| Anfield T200/T201 pressure transducers | Brake pressure |
| Suspension pressure transducer | Shock nitrogen pressure (**testing only**) |

Brake pressure is read at the master cylinders, which live in the front and cover both front and rear brakes — so there is no brake sensor on the rear node. The transducer tees in alongside the brake switch, so **the brake light keeps working even if this board dies.** Keep it that way.

### Rear node — by the gas tank, mounted to the frame

STM32F103 Bluepill.

| Part | What it measures |
|---|---|
| Littelfuse 55505-00-02-A-ND gear tooth sensor ×1 | **Brake disc** speed — only one at the rear, and it does not read a wheel hub |
| AS5600 Hall-effect encoder on the bellcrank ×2 | Suspension travel, one per corner (analog output) |
| MLX90614 infrared temperature sensor | CVT and brake disc temperature |
| Suspension pressure transducer | Shock nitrogen pressure (**testing only**) |

### eCVT node — purchased, not built

| Part | Notes |
|---|---|
| ODrive S1 | Has its own STM32 and speaks CAN directly, so no custom board is needed |
| ODrive D5065 motor | Smaller motor, built-in thermistor |
| ODrive OA1 encoder | |
| Brake resistor | 2 Ω, 50 W |

Runs off its own stator-powered supply, separately rectified and filtered — **not** off the DAQ's DeWalt packs. A BLDC motor tolerates supply noise far better than the sensor nodes do, so its filtering can be simpler.

**The DAQ side of the eCVT is paused.** The S1 sits on the same CAN bus, but nothing is logging it yet. It should drop in easily next year. A PI control loop exists in [`firmware/control/`](firmware/control/) but has never touched a real ODrive, engine, or CVT.

### Display node — driver readout

STM32F103 Bluepill driving four daisy-chained MAX7219 LED matrix modules over SPI as a scrolling display. Listens on the CAN bus; does not transmit. Shows general data for the driver — exactly which channels is still being decided.

**Status:** PCB and enclosure are done. Not yet represented in this repo's firmware.

### Sensors that are testing-only

The **suspension (shock nitrogen) pressure transducers** are a bench and test-day instrument for correlating gas pressure against travel. **They do not go on the car at a race.** Don't design around them.

---

## Power

The whole DAQ system runs on **DeWalt 20V MAX packs** (5S, 15.0–21.0 V nominal range).

Every pack lands on the **firewall node**, always — it carries the power distribution for the entire system. This is deliberate: noise filtering is painful to get right, so it is done once, in one place, rather than per node. The corner nodes and display node take clean power from there.

The final firewall board combines two packs through an **ORing circuit**, with a voltage monitor reporting pack voltages, ORing behavior, and which pack is currently carrying the load. **None of that is on the V1 board** — V1 runs from a single supply.

A dedicated power distribution board is a separate, later project.

---

## CAN bus

- One bus, 500 kbit/s, shared by firewall, front, rear, display, and the ODrive S1.
- Front and rear nodes transmit sensor data; the display node listens only.
- Every node needs its own transceiver (SN65HVD230).
- **Exactly two 120 Ω terminators, at the two physical ends of the bus** — the firewall node and the farthest corner node. Not one per board. This is the single most common way to break a CAN bus.
- Debugging is done with a CANable.
- The ODrive has its own CAN addressing scheme — watch for ID collisions. See [`protocol/V2_DESIGN_NOTES.md`](protocol/V2_DESIGN_NOTES.md).

### Node IDs

| Node | CAN ID (base) |
|---|---|
| Firewall | 0x000 |
| Front | 0x010 |
| Rear | 0x020 |
| eCVT | 0x030 (reserved, unconfirmed) |
| Display | not yet assigned |

Whether the eCVT uses that reserved ID or the ODrive's own native CAN protocol is still to be confirmed.

### Physical layer and wiring

- Ethernet CAT5e twisted pair, one differential pair as CAN H/L. **Stranded, not solid-core.**
- Deutsch DT connectors throughout: pin A = CAN H, pin B = CAN L, pin C = GND reference.
- Keep stub lengths short at each node tap — 30 cm maximum.
- Shield drain wire tied to chassis GND **at the firewall end only** (single-point ground).

### CAN database (DBC) — read this before you trust a decode

There are **two** DBC files in this repo:

| File | Status |
|---|---|
| `protocol/telemetry.dbc` | **v1 — the wire format running today.** Generated, not hand-edited. |
| `protocol/telemetry_v2.dbc` | v2 scaffold — not on the wire yet. |

**`can/README.md` tells you to parse a `can/baja.dbc`. That file does not exist in this repo.** Use `protocol/telemetry.dbc` instead.

The v1 file is generated:

```
python tools/gen_dbc.py
```

Edit `TLM_CHANNELS` in `protocol/telemetry_proto.c` (and the mirrored `CHANNELS` in `proto.py`), then regenerate. `tools/test_dbc.py` fails if you forget, and separately decodes 200 firmware-packed payloads through cantools to confirm the DBC and the firmware agree. That check is what catches a byte-order mistake — get `@1` vs `@0` wrong and every signal still decodes, just to plausible-looking nonsense.

---

## What's running vs. what's scaffolding

This repo holds two very different kinds of content, and mixing them up is the easiest way to get confused.

**Running today on real hardware, verified end to end (CAN → LoRa → SD → PC):**
the v1 wire format (3 channels × 3 nodes, `protocol/telemetry_proto.c`), `firmware/bluepill_node/`, `firmware/nucleo_hub/`, and `pc_app/`.

**Important:** v1 firmware does **not** read real sensors. Each Bluepill runs a built-in waveform generator (`sim_read_channels()`) in place of a physical ADC/I2C sensor. The hardware described above is the real, current design — the firmware just hasn't been wired to it yet. Swapping in a real sensor means editing that one function and one table (`TLM_CHANNELS`); nothing else in the v1 chain changes.

**Written and host-tested, but NOT wired in and NOT tested on any board:**
the v2 wire format (24 channels × 4 nodes, `protocol/telemetry_proto_v2.*`), the real sensor drivers (`firmware/sensors/`), and the E-CVT control loop (`firmware/control/`). Each has its own README saying so. See [`firmware/NODE_INTEGRATION_V2.md`](firmware/NODE_INTEGRATION_V2.md) for wiring one in.

If a folder isn't in the first list, assume it's scaffolding until its own README says otherwise.

### Naming quirks that will confuse you

The hardware docs were reconciled against the car on 2026-10-30. Two things
still look wrong and aren't:

- **`gps_neo_m8n.*` and `suspension_pot.*`** are named after parts that are no
  longer used (u-blox NEO-M8N, Bourns 53AAA pot). The files weren't renamed
  because the driver logic still applies: the M100 is read-only NMEA over UART
  like the M8N, and the AS5600 in analog mode is read through the ADC exactly
  like the pot was. Rename them next time you touch the code; don't let the
  filename convince you of the part.
- **The simulator carries a `wheel_speed_rr` channel the car doesn't have.**
  The rear has one gear tooth sensor, on the brake disc. The extra channel is
  a leftover of the symmetric 24-channel sim layout.

If you find any other doc still saying Littelfuse 55075, Bourns 53AAA,
NEO-M8N, E32-900T30D, or a FastAPI/SQLite backend, it was missed — those are
all superseded.

---

## Layout

```
protocol/     wire format (C + Python) and the DBC files        [v1 RUNNING / v2 SCAFFOLD]
firmware/
  bluepill_node/   CAN node source                              [v1 RUNNING]
  nucleo_hub/      CAN aggregation, LoRa TX, SD logging         [v1 RUNNING]
  sensors/         real sensor driver skeletons                 [SCAFFOLD]
  control/         E-CVT PI control loop                        [SCAFFOLD]
  projects/        CubeIDE projects with source already added
pc_app/       ground station: serial reader, dashboard, CSV     [v1 RUNNING]
simulation/   synthetic sensor feed + dashboards, no hardware needed
field_data/   real .TLM/.csv logs from actual test sessions
nodes/        per-node hardware notes: front, rear, ecvt, firewall, display
hardware/     connector and enclosure notes
can/          CAN topology notes (see drift warning above)
suspension/   bellcrank travel-sensor notes
telemetry/    LoRa notes
docs/         architecture writeup and dated build log
tools/        test suites and the log converter
scripts/      standalone cantools CAN log decoder
```

---

## Ground station (the pit laptop)

### Run it

```
cd pc_app
pip install -r requirements.txt
python run.py
```

With no arguments it looks for the E22 dongle, falls back to the simulator if there isn't one, picks a free HTTP port, and opens the dashboard. The simulator is a port of the firmware's waveform generator, so this is what the real system looks like.

```
python run.py --sim --drop-rate 0.15 --offline-node 2   # exercise loss and stale handling
python run.py --list-ports                              # see what's plugged in
python run.py --port COM7 --csv --raw                   # pin a port, record both formats
python run.py --replay logs/LOG0001.TLM --speed 4       # replay an SD log
python run.py --web --host 0.0.0.0                      # view from a phone on the network
```

### The dashboard

The **browser dashboard is the default.** `run.py` serves `pc_app/static/index.html` and opens it in Edge or Chrome `--app` mode — its own window, its own taskbar entry. Four dashboard designs live in `simulation/dashboards/` and are served at `/dashboards/`; the original single view is at `/`.

The PySide6 (Qt) native window is **retired**. It still works behind `--native`, but nothing tests it and it only speaks v1's 3-node channel set. It was retired because it pulled a heavy GUI dependency in for a single window, while the browser dashboard had to exist anyway for phone access.

Both front ends keep visuals separate from logic, so either can be restyled without touching how data flows. In the browser dashboard, all colors, spacing and fonts are CSS variables in one block at the top.

### Cleaning up old ground-station logs

Every run writes a rotating session log into `pc_app/logs/`, and nothing prunes them automatically.

```
python run.py --list-logs                            # inventory: name, size, age
python run.py --clean-logs                           # keep the 10 newest, then exit
python run.py --clean-logs --keep 20                 # keep the 20 newest instead
python run.py --clean-logs --older-than 30           # delete anything older than 30 days
python run.py --clean-logs --older-than 30 --dry-run # preview only, deletes nothing
```

There's also a **Ground station logs** panel in the `--web` dashboard with Preview/Delete buttons.

**This never touches `field_data/`, by design.** Those are real SD captures pulled off the car. A `logs/` file can be regenerated on the next launch; a lost field capture is gone for good.

### Build Telemetry.exe

```
cd pc_app
build.bat
```

Produces `dist\Telemetry.exe` — one file, no install on the target machine — and drops a **Telemetry** shortcut on your Desktop. The script installs PyInstaller and `PySide6-Essentials`, runs the wire-format tests, builds, smoke-tests the result, then creates the shortcut.

Two things worth knowing:

- Its closing message still describes the retired native window as the default launch mode. The build is fine; that text is stale.
- It deliberately does **not** pass PyInstaller's `--clean`. In a synced folder (OneDrive) or on a machine running HP Sure Click, something intermittently holds a handle on `build/telemetry/localpycs`; `--clean` treats a failed delete as fatal and kills the build with `WinError 5`. For a guaranteed-clean build, pause sync, delete `build\` by hand, re-run.

**Why `PySide6-Essentials` and not `PySide6`:** the umbrella package pulls in `PySide6-Addons`, which bundles QtWebEngine — a full embedded Chromium. The native window only imports QtCore/QtGui/QtWidgets, all in Essentials. `telemetry.spec` excludes the Addons modules by name as a second layer of defense.

**This has to run on Windows.** PyInstaller cannot cross-compile a Windows executable.

---

## Wire format

One 42-byte frame every 500 ms, carrying all three v1 nodes:

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

**Why snapshots instead of forwarding raw CAN frames:** the bus carries 3 nodes × 10 Hz × 8 bytes = 240 B/s of payload, while the E22 at its default 2.4 kbps air rate delivers roughly 200 B/s. Forwarding raw frames cannot work at any buffer size — the link is narrower than the bus. Snapshotting the latest value from each node decouples the two rates, and the SD log keeps the same frames so a dropped radio packet costs nothing.

The SD log and the radio stream are byte-identical, so `python run.py --replay` runs a recorded session through the exact code path that handled it live.

### v2 protocol (scaffold, not wired in)

`protocol/telemetry_proto_v2.h/.c` is a second, independent wire format built for the 24-channel layout the finished car will carry. v1's fixed-shape payload has no room to say "how many channels" or "how wide," so rather than change what v1's bytes mean, v2 gets its own version byte and a multi-frame ("paged") CAN scheme: a node with more than 3 channels sends up to 4 consecutive 8-byte CAN frames per cycle.

**Nothing in `firmware/` or `pc_app/` calls into this yet,** and there's no `proto_v2.py`, so the PC app cannot decode a v2 frame. Standalone host test:

```
gcc -std=c99 -Wall -Wextra -Werror -I protocol protocol/telemetry_proto_v2.c tools/test_roundtrip_v2.c -o /tmp/test_roundtrip_v2 -lm
/tmp/test_roundtrip_v2
```

Full design writeup — CAN paging, the epoch-based torn-set detector, why GPS lat/lon needs `int32`, LoRa airtime budget, the ODrive CAN ID collision risk, and open questions — is in [`protocol/V2_DESIGN_NOTES.md`](protocol/V2_DESIGN_NOTES.md).

---

## Tests

```
python tools/run_all_tests.py
```

**2380 checks across eight suites.**

| Suite | What it proves |
|---|---|
| `test_roundtrip.py` | Compiles `telemetry_proto.c` with a host compiler at `-Werror` and compares it against `proto.py` byte for byte across 64 generated frames, plus CRC vectors, corruption rejection, stream resync, sequence wraparound. **359 checks.** |
| `test_fat32.py` | Formats real FAT32 images with `mkfs.vfat` across 7 cluster-size and partition layouts, runs the writer, validates with `fsck.fat` and an independent parser that walks the cluster chain byte by byte. **199 checks.** |
| `test_firmware.py` | Compiles the real `can_node.c` and `telemetry_hub.c` against a host HAL shim. Transmit cadence, mailbox exhaustion, bus-off recovery, aggregation, loss arithmetic across the 8-bit wrap, node timeouts, ID filtering, ring buffer overflow. **37 checks.** |
| `test_app.py` | Ground station backend: path resolution when frozen, a corrupted settings file, the HTTP port already taken, the USB dongle unplugged mid-session. **57 checks.** |
| `test_qt.py` | `FramePump` (shared by both front ends), then builds the PySide6 window headlessly and drives it through a real background worker thread. **40 checks.** |
| `test_dbc.py` | Decodes 200 firmware-packed CAN payloads through cantools and checks scale, offset, unit, sign, length and byte order on every signal. **1499 checks.** |
| `test_repo.py` | Structural audit: vendored copies byte-identical to canonical sources, node IDs unique and ordered, `.ioc` keys present, `main.c` wired up, and that packaging never pulls in QtWebEngine. **164 checks.** |
| `test_launcher.py` | App-window launch logic with registry, filesystem and process spawn faked. **25 checks.** |

The first suite is the one worth keeping green. The firmware and the ground station never see each other's source, so a disagreement about a struct offset or an endianness assumption doesn't crash — it produces a dashboard full of plausible-looking wrong numbers.

Needs a host C compiler and `dosfstools`. Suites skip cleanly if either is missing.

**Not covered:** `sd_spi.c` is register-level SD/SPI code that only means anything on silicon, and the LoRa link itself. Those get verified on the bench.

---

## Hardware gotchas — read before wiring

**The E22 part number matters.** `-U` is a USB dongle for the PC. The hub needs `-D` or `-S`, which have UART pins.

**Give the E22 its own supply.** Over 600 mA in transmit bursts at 22 dBm — more than the Nucleo's regulator provides over USB. 470 µF or more at the module. Getting this wrong looks like a link that works on the bench and dies when you raise power or packet length.

**US frequency.** Factory default is 868.125 MHz (EU band). Use channel 0x45 → 919.125 MHz to stay inside US 902–928 ISM. Same channel on both ends.

**CAN termination.** Exactly two 120 Ω terminators, at the two physical ends. Not one per board.

**SD card.** There's no FatFs or SPI HAL in the project, and neither can be added without regenerating with new middleware. So the repo carries `sd_spi.c` (register-level block driver) and `fat32.c` (append-only FAT32 writer). The card comes out readable on any OS.

---

## Issues found in the original CubeMX projects

Kept here because every one of these is a trap a future member could walk back into.

| Issue | Effect | Fix |
|---|---|---|
| `SYS: No_Debug` in all four Bluepill `.ioc` files | frees PA13/PA14, killing SWD after the first flash; recovery needs a BOOT0 jumper | Debug → Serial Wire |
| `AutoBusOff = DISABLE` on all five | one bus fault leaves a node permanently mute with no indication | Enable it |
| CAN sample point mismatch: 77.8% on Bluepills vs 87.5% on the Nucleo | both hit 500 kbit/s so it works on the bench; erodes margin for oscillator drift and cable length | Bluepill Prescaler 9 / BS1 6 / BS2 1 → 87.5% |
| blocking `HAL_UART_Transmit` inside the CAN RX ISR | ~5.5 ms in interrupt context per frame; survivable at 20 msg/s, breaks once SD writes are added | ISR pushes to a ring buffer, main loop does the work |
| `HAL_Delay(200)` in the node main loop | CPU parked in a busy wait, cannot service anything else | tick-driven, non-blocking |
| accept-everything CAN filter | unrelated bus traffic reaches the CPU | mask to 0x100–0x103 in hardware |
| no CRC, no timestamp, raw `float` on the wire | undetectable corruption over a lossy radio | CRC-16, hub uptime, scaled `int16` |

---

## Simulation & dashboards (no hardware needed)

`simulation/` generates synthetic data for the full 24-channel sensor suite, so dashboards can be designed and judged before sensors are wired up. It doesn't import from or modify `firmware/`, `protocol/`, or `pc_app/telemetry/`.

```
cd simulation
python run_sim.py
```

One single-page app at `simulation/dashboards/index.html`, seven tabs, all fed from one WebSocket:

| Tab | Key | What it shows |
|---|---|---|
| **Live** | `1` | Every channel as a large readout with sparkline and running min/max/avg |
| **Gauges** | `2` | Sweep-arc dials for the 22 channels with a sensible range |
| **Schematic** | `3` | Plan view of the car with each value where its sensor physically sits, so a bad corner reads as a position rather than a table row |
| **Charts** | `4` | Chart-recorder lanes on a shared time axis — a brake spike lining up with a wheel-speed drop |
| **Map** | `5` | Draws the course from GPS as you drive. No map tiles, no network, so it works at a track with no signal |
| **Session** | `6` | Min/max/average/current for every channel since the session started |
| **Logs** | `7` | Archive index of `field_data/` with a copy-to-clipboard replay command |

**Alarm limits** turn a channel red when it leaves a sane range. Thresholds live in `LIMITS` near the top of the script, and they are "something is wrong" bounds, not performance targets — a dashboard that cries wolf gets ignored. **Freeze** (`F`) holds the display while data keeps arriving behind it.

Every dashboard is built from the `meta` message's channel list and hardcodes no channel name or node count, so **any dashboard built against the synthetic feed renders unmodified against the real ground station, and vice versa.**

---

## Field data

`field_data/` holds real `.TLM`/`.csv` logs from test sessions — not synthetic data, not scratch bench output. One `YYYY-MM-DD_description/` folder per session with a short `notes.md` alongside. `.gitignore` carves out an explicit exception so these **do** get committed.

```
cd pc_app
python run.py --replay ../field_data/<session>/LOG0001.TLM
```

That runs a captured session back through the exact decoder the live app used. See [`DATA_VERIFICATION_CHECKLIST.md`](DATA_VERIFICATION_CHECKLIST.md) for confirming a reading is a genuine live measurement rather than stale or simulated.

---

## Other guides

- [**`BRINGUP_GUIDE.md`**](BRINGUP_GUIDE.md) — bench bring-up for v1: building and flashing the boards, wiring, configuring both E22 modules, staged power-up.
- [**`PLATFORMIO_BMP_GUIDE.md`**](PLATFORMIO_BMP_GUIDE.md) — building and flashing the same source from VS Code + PlatformIO with a Black Pill flashed as a Black Magic Probe, instead of CubeIDE + ST-Link.
- [**`DATA_VERIFICATION_CHECKLIST.md`**](DATA_VERIFICATION_CHECKLIST.md) — confirming the pipeline carries data faithfully.
- [**`V3_CUBEMX_AND_SENSOR_INTEGRATION_GUIDE.md`**](V3_CUBEMX_AND_SENSOR_INTEGRATION_GUIDE.md) — the canonical pinout, plus CubeMX and sensor integration. Cross-check its part numbers against the hardware table above.
- [**`V3_BRINGUP_CHECKLIST.md`**](V3_BRINGUP_CHECKLIST.md) — board-by-board bring-up order as a checkbox list. Doesn't repeat pin numbers; defers to the guide above.
- `docs/architecture.md`, `docs/build-log/` — early architecture writeup and dated log of build decisions. Historical: some of it describes approaches since dropped. Left as a record of decisions made at the time.

---

## License

MIT. See [`LICENSE`](LICENSE).
