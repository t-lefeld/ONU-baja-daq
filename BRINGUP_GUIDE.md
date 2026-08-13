
# Bring-up guide — 3-node CAN/LoRa telemetry system

Your source tree is fully updated and tested (2380 checks passing across all
8 suites). This guide walks through everything left to do at the bench:
building and flashing the four boards, wiring, configuring the two E22 LoRa
modules, and bringing the whole chain up in stages.

**A note on "automatic" flashing.** I can't hand you ready-made `.bin`/`.hex`
files — building STM32 firmware needs the ARM GCC cross-compiler, and my
sandbox has neither that toolchain installed nor open internet access to fetch
one (only an allowlisted set of domains, which doesn't include ARM's or
PlatformIO's download servers). What I *can* guarantee is that every source
file, `.ioc`, linker script and startup file already sitting in
`firmware/projects/` is correct and ready to build — so the CubeIDE steps
below are close to one-click: open the project, Build, Run. There's no code
to write or fix first.

---

## Part 1 — Build and flash the three Bluepills

Each of these already exists as a complete CubeIDE project under
`firmware/projects/`. Only `node_id.h` differs between them.

| Project folder | Physical board | NODE_ID | CAN ID |
|---|---|---|---|
| `1st Bluepill` | Environmental node | 0 | 0x100 |
| `2nd Bluepill` | Power node | 1 | 0x101 |
| `3rd Bluepill` | Motion node | 2 | 0x102 |

`4th Bluepill` is retired — leave it alone. It will fail to compile on
purpose (a compile-time guard rejects `NODE_ID >= TLM_NODE_COUNT`), so you
can't accidentally flash a stale 4-node image.

For each of the three active projects:

1. Open **STM32CubeIDE**.
2. **File → Open Projects from File System…**, browse to
   `firmware/projects/1st Bluepill` (then repeat for 2nd/3rd), click **Finish**.
   If it's already imported from before, just find it in the Project Explorer.
3. Right-click the project → **Build Project** (or select it and press the
   hammer icon). It should build clean — the source has already been
   validated against a host compiler at `-Werror` by the test suite.
4. Connect that Bluepill to your ST-Link (SWDIO/SWCLK/GND/3V3).
5. Right-click the project → **Run As → STM32 C/C++ Application**. CubeIDE
   flashes it over SWD and the target starts running immediately.
6. Disconnect, label the board (e.g. a piece of tape: "NODE 0"), move to the
   next Bluepill and repeat with the matching project.

**Do this for all three before wiring the CAN bus together** — it's much
easier to confirm each board flashed the right image while it's still alone
on the bench than after they're all wired to one bus.

## Part 2 — Build and flash the Nucleo hub

1. Import `firmware/projects/Nucleo CAN Bus Test` the same way.
2. Before building the first time, open
   `Core/Inc/telemetry_hub.h` and confirm:
   ```c
   #define HUB_DEBUG_UART 1   // mirrors frames as text on the ST-Link VCP
   #define HUB_ENABLE_SD  0   // leave OFF for the first bring-up stage
   ```
   (`HUB_ENABLE_SD` starts at 0 deliberately — you'll flip it to 1 in Part 6
   once CAN and the radio are both confirmed working.)
3. Build Project.
4. Connect the Nucleo over its onboard ST-Link USB (no external programmer
   needed — the L476RG's ST-Link is built into the board).
5. Right-click → **Run As → STM32 C/C++ Application**.

---

## Part 3 — Wiring recap

**CAN bus.** Every Bluepill and the Nucleo need a CAN transceiver on the bus.
Tie all CAN_H together and all CAN_L together. Exactly **two** 120 Ω
terminators, one at each physical end of the bus — not one per board. Common
ground across all four boards.

**E22 LoRa module → Nucleo** (already remapped to fit the Arduino Uno R3
header, per your earlier request):

| E22 pin | Nucleo pin | Arduino label | Function |
|---|---|---|---|
| M0 | PB0 | A3 | mode select bit 0 |
| M1 | PA4 | A2 | mode select bit 1 |
| AUX | PA1 | A1 | busy indicator (open-drain, needs pull-up) |
| RXD | PA9 | D8 | USART1_TX |
| TXD | PA10 | D2 | USART1_RX |
| GND | GND | — | common ground |
| VCC | *own 3.3 V supply* | — | see power note below |

**Power — don't skip this.** At 22 dBm the E22 pulls 600+ mA in transmit
bursts, more than the Nucleo's onboard regulator can supply over USB. Give it
its own 3.3 V supply with 470 µF+ of bulk capacitance right at the module's
pins, grounds tied together. Symptom of getting this wrong: works fine on the
bench with short packets, then dies the moment you raise power or packet
length as the module browns out mid-transmit.

**microSD → Nucleo:**

| SD pin | Nucleo pin | Function |
|---|---|---|
| CLK | PA5 | SPI1_SCK (also drives LD2 — it'll flicker with traffic) |
| DO | PA6 | SPI1_MISO |
| DI | PA7 | SPI1_MOSI |
| CS | PB6 | GPIO, idles high |
| VCC | 3.3V | 5V-tolerant breakout boards can take 5V instead |
| GND | GND | |

---

## Part 4 — Configuring the E22 modules (in depth)

You have two E22 modules to configure: the `-D`/`-S` module wired to the
Nucleo hub, and the `-U` USB dongle that plugs into the PC. **Both must be set
to the same channel** or they'll never hear each other.

### 4.1 — Get the configuration tool

Ebyte's official Windows utility is usually called **RF_Setting** (sometimes
labelled RF Setting / RF_Settingv3.x depending on the release you get). It
ships from the module manufacturer/reseller — the most reliable source is the
product page for your exact part number on **cdebyte.com** (search "E22-900T22D"
or "E22-900T22U" there and check the Downloads/Support tab), or the download
link your reseller (e.g. the AliExpress/Amazon listing) provided when you
bought the modules. Unzip it — no installer, it's usually a portable `.exe`.

If you'd rather not hunt for the Windows tool, there's also a cross-platform
open-source alternative (`ebyte-modules-setting` on GitHub) that talks the
same protocol, if you're on macOS/Linux or just prefer it.

### 4.2 — Understand the mode pins

The E22 has four operating modes selected by M0/M1:

| Mode | M1 | M0 | What it does |
|---|---|---|---|
| Normal (transparent) | 0 | 0 | UART ↔ radio open, this is how it runs day-to-day |
| WOR | 0 | 1 | Wake-on-radio transmit/receive |
| **Configuration** | **1** | **0** | Registers accessible over UART — **this is the mode you need** |
| Deep sleep | 1 | 1 | Lowest power, radio and UART both off |

So to configure a module: **M1 = HIGH (3.3V), M0 = LOW (GND)**. In this mode
the module's UART is fixed at **9600 baud, 8N1**, regardless of whatever data
rate you've configured it for in normal mode.

### 4.3 — Configure the DIP module (the one going on the Nucleo) first, on the bench

Do this **before** soldering it permanently into the circuit — it's much
easier with a dedicated USB-to-TTL adapter than trying to reuse the Nucleo's
UART while the hub firmware is also trying to talk to it.

1. Get a 3.3 V USB-to-TTL adapter (FTDI, CP2102, or CH340 board — just make
   sure it's set to 3.3 V logic, not 5 V, or use one with level shifting).
2. Wire the bare E22 module on a breadboard:
   - Module RXD ← adapter TXD
   - Module TXD → adapter RXD
   - Module GND ↔ adapter GND
   - Module VCC ← 3.3 V (from the adapter or a separate supply)
   - Module M0 → GND
   - Module M1 → 3.3V
   - Leave AUX unconnected for this step (or wire it if the tool wants to
     watch it — check its guidance if it reports "module not responding").
3. Plug the adapter into your PC. Note which COM port shows up in **Device
   Manager**.
4. Launch RF_Setting. Select that COM port, set baud to **9600**, 8N1.
5. Click **Read/Get** (wording varies by tool version) to pull the module's
   current settings. You should see fields like: module address (ADDH/ADDL),
   Network ID, Channel, Air Data Rate, UART baud rate, UART parity,
   Transmission Power, Transmission Mode (transparent vs fixed), RSSI options.
6. **Set the channel.** For the US 902–928 MHz ISM band, set the channel
   value to **`0x45` (decimal 69)**, which the module computes as
   `850.125 MHz + channel × 1 MHz = 919.125 MHz`. The factory default is
   channel `0x12` (18) → 868.125 MHz, the European band — wrong for US use.
7. Leave everything else at default unless you have a specific reason to
   change it (transparent transmission mode, default air rate, etc. — this
   firmware doesn't require anything exotic).
8. Click **Write/Set** to push the change to the module.
9. Click **Read** again to confirm the channel readback actually shows
   `0x45` / 919.125 MHz — don't trust the write silently succeeding.
10. Power down, set **M1 back to GND** (normal mode, M0=0/M1=0), and now wire
    the module into the final circuit per the table in Part 3.

### 4.4 — Configure the USB dongle (the one on the PC)

The `-U` dongle has USB built in, so there's no separate TTL adapter needed —
but you still need to get it into configuration mode:

1. Check your specific dongle's documentation/listing — many `-U` boards
   have small onboard jumpers or slide switches for M0/M1 (since there's no
   separate microcontroller driving those pins for you). Set them to
   **M1=HIGH, M0=LOW** the same as above. If yours doesn't expose M0/M1
   directly, check whether the seller's driver/tool sets mode via software.
2. Plug it into the PC, note the COM port in Device Manager.
3. Open RF_Setting, select that port at 9600 baud, **Read**.
4. Set the same **channel `0x45`** as the hub module. This is the part that
   actually matters — the two ends must match exactly, or the AUX pin will
   toggle happily on both sides while zero packets ever get through (a
   mismatched air rate or channel produces a link that "looks" healthy but
   silently delivers nothing).
5. Write, then Read back to confirm.
6. Set M0/M1 back to normal mode (0/0) so the dongle is ready for the ground
   station app to use it.

### 4.5 — If the tool reports "module not responding"

In rough order of likelihood: wrong COM port selected, M0/M1 not actually in
configuration mode (double-check with a multimeter — floating pins with weak
pull-ups can read wrong), baud rate not set to 9600 in the tool, TX/RX swapped
on the adapter, or the module isn't getting a clean 3.3V (this mode still
needs stable power even though transmit current is low).

---

## Part 5 — Prep the SD card

Format it FAT32 (any normal Windows "Format" dialog, default allocation size,
is fine — `fat32.c` in this firmware only needs a standard FAT32 volume, no
special partitioning). Nothing else needed; the firmware creates
`LOG0001.TLM`, `LOG0002.TLM`, etc. itself at each power-up.

---

## Part 6 — Staged bench test

Don't skip ahead — each stage isolates one thing.

1. **CAN only.** `HUB_ENABLE_SD` should still be 0, radio unplugged. Watch
   USART2 through the ST-Link Virtual COM Port at 115200 baud (any serial
   terminal — PuTTY, Tera Term, the Arduino Serial Monitor, whatever you have).
   You should see one line every 500 ms, all three nodes reporting. Unplug one
   Bluepill's power — that node should show `!` (stale) within about 1.5 s.
2. **Add the radio.** Plug the configured `-U` dongle into the PC:
   ```
   python run.py --port COMx --raw
   ```
   Frames should start appearing.
3. **Add the SD card.** In CubeIDE, flip `HUB_ENABLE_SD` to `1`, rebuild,
   reflash the hub. `LOG0001.TLM` should appear on the card and grow by 42
   bytes every 500 ms.
4. **Fault recovery.** Unplug the CAN bus mid-run, then reconnect. The hub
   should recover on its own — `busoff_events` in the debug UART stats should
   increment, not stay stuck.

---

## Part 7 — Run the ground station

```bash
cd pc_app
pip install -r requirements.txt
python run.py --port COMx --web
```

Open the dashboard in a browser and confirm all three nodes (Environmental,
Power, Motion) show live data with correct online/stale flags. Drop
`--web` if you'd rather use the native window (the default).

---

## Quick reference — files this guide touches

- `firmware/projects/{1st,2nd,3rd} Bluepill/Core/Inc/node_id.h` — already
  correct, one line each (`NODE_ID 0/1/2`), nothing to change.
- `firmware/projects/Nucleo CAN Bus Test/Core/Inc/telemetry_hub.h` — toggle
  `HUB_ENABLE_SD` between stages 1–3 of Part 6.
- Ebyte RF_Setting — external tool, not part of this repo; channel `0x45` on
  both the hub module and the USB dongle.

Sources for the E22 configuration details above:
- [Ebyte LoRa E22 configuration guide — Mischianti](https://mischianti.org/ebyte-lora-e22-device-for-arduino-esp32-or-esp8266-configuration-3/)
- [Ebyte LoRa E22 specs and basic usage — Mischianti](https://mischianti.org/ebyte-lora-e22-device-for-arduino-esp32-or-esp8266-specs-and-basic-usage-1/)
- [E22-900T22D User Manual](https://www.cdebyte.com/pdf-down.aspx?id=1463)
- [ebyte-modules-setting (cross-platform config tool) — GitHub](https://github.com/mosvov/ebyte-modules-setting)
