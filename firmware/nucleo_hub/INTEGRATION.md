# Nucleo-L476RG hub — integration

CubeMX settings live in **[../CUBEMX_SETUP.md](../CUBEMX_SETUP.md)**. Your
`.ioc` is untouched; you configure it yourself. This file covers the source
side and the hardware.

## Files

Already present in `firmware/projects/Nucleo CAN Bus Test/`:

```
Core/Inc + Core/Src:
  telemetry_proto.h/.c    shared wire format
  telemetry_hub.h/.c      CAN aggregation, snapshot framing, dispatch
  lora_e22.h/.c           E22 UART driver
  sd_log.h/.c             append-only frame logger
  fat32.h/.c              minimal FAT32 writer
  sd_spi.h/.c             SD card over SPI1, register level
  main.c                  rewritten
```

No FatFs, no SPI HAL, nothing that has to come out of CubeMX. See "the SD
card" below for why.

## Architecture

```
CAN RX interrupt ──push──> ring buffer ──drain──> latest-value table
                                                          │
                                          every 500 ms ────┤
                                                          ▼
                                                  build 42-byte frame
                                                          │
                                        ┌─────────────────┼─────────────────┐
                                        ▼                 ▼                 ▼
                                  LoRa (DMA)        SD card            debug UART
```

**Why the ring buffer.** The previous firmware did a blocking
`HAL_UART_Transmit` of a 64-character string from inside
`HAL_CAN_RxFifo0MsgPendingCallback` — about 5.5 ms in interrupt context per
frame at 115200 baud. It survived at 20 frames/second. It would not survive
adding SD writes, because a cheap card doing an internal garbage-collection
pass can block for 100 ms or more, and dropped CAN frames caused by that are
close to impossible to diagnose after the fact.

Now the ISR copies eight bytes and returns. Everything else happens in the main
loop, where it is allowed to take as long as it needs. `HUB_CAN_QUEUE_LEN` is
64 entries — a bit over 2 seconds of buffering at 30 frames/second, sized for
exactly that SD stall.

**Why snapshots instead of forwarding raw CAN.** The bus carries
3 nodes × 10 Hz × 8 bytes = 240 B/s of payload. The E22 at its default 2.4 kbps
air rate delivers roughly 200 B/s. No buffer size fixes a link narrower than
its source. Snapshotting the latest value from each node decouples the two
rates, and the SD log keeps the same frames, so a dropped radio packet costs
nothing.

**Hardware CAN filtering.** The old filter accepted everything. This one masks
to 0x100–0x103, so unrelated traffic on a shared bus never reaches the CPU.

## The LoRa module

**Check your part number.** `E22-900T22U` is a USB dongle — it belongs on the
PC. The hub needs `E22-900T22D` (DIP) or `E22-900T22S` (SMD), which expose
plain UART pins. A `-U` cannot be wired to the Nucleo; its serial lines go
through an onboard USB bridge.

### Wiring

```
E22 M0   -> PB0  (Arduino A3)  mode select bit 0
E22 M1   -> PA4  (Arduino A2)  mode select bit 1
E22 AUX  -> PA1  (Arduino A1)  busy indicator, open drain, needs pull-up
E22 RXD  -> PA9  (Arduino D8)  USART1_TX
E22 TXD  -> PA10 (Arduino D2)  USART1_RX
E22 GND  -> GND     common with the Nucleo
E22 VCC  -> its own 3.3V supply, see below
```

All five signal wires land on the Arduino Uno R3 header (CN5/CN8/CN9,
UM1724 Table 23). M1 and AUX used to sit on PB1/PB2, which aren't broken out
to that header at all - only to the ST Morpho connector - so the module needed
two connectors' worth of wiring. Moving them to PA4/PA1 gets the whole thing
onto one header.

### Power — the one that catches people

At 22 dBm the module pulls well over 600 mA in transmit bursts. The Nucleo's
3.3 V regulator, when the board is USB-powered, cannot supply that.

Give the E22 its own 3.3 V supply, put 470 µF or more of bulk capacitance right
at its pins, and tie the grounds together.

The symptom of getting this wrong is distinctive and misleading: everything
works on the bench with short test packets, then the link dies the moment you
raise transmit power or packet length, because the module browns out
mid-transmission and resets.

### Frequency — a legal issue, not a performance one

Factory default is **868.125 MHz**, the European ISM band. In the US the
licence-free band is **902–928 MHz**. Register 05H sets
`channel = frequency_MHz − 850.125`:

| Channel | Frequency | |
|---|---|---|
| 0x12 (18) | 868.125 MHz | factory default, EU band |
| **0x45 (69)** | **919.125 MHz** | inside US 902–928, use this |

Set the same channel on both the hub module and the USB dongle.

Easiest route is Ebyte's Windows configuration tool — it reads settings back,
so you can confirm they took. `lora_e22_configure()` does the same thing from
firmware, but verify the air-rate bit encoding against the manual revision that
shipped with your modules before trusting it: Ebyte has used more than one
register layout under the E22 name, and a mismatched air rate gives you a link
that looks perfectly healthy — AUX toggles, no errors — and never delivers a
packet.

### Air rate and airtime

Default 2.4 kbps. The 42-byte frame takes about 140 ms of airtime; at 2 Hz
that is roughly 28% duty cycle. Comfortable, with room to add channels.

If you later want faster snapshots, raise the air rate to 4.8 or 9.6 kbps on
**both** modules — you trade range for throughput.

## The SD card

### Wiring

```
SD CLK -> PA5     SPI1_SCK    (also drives LD2; it will flicker with traffic)
SD DO  -> PA6     SPI1_MISO
SD DI  -> PA7     SPI1_MOSI
SD CS  -> PB6     GPIO, idles high
SD VCC -> 3.3V
SD GND -> GND
```

If your breakout is a 5V Arduino-style board with a regulator and level
shifters, feed it 5V. If it is a bare 3.3V socket, wire it directly. Do not
feed 5V logic into PA6.

### Why there is no FatFs

Your project has neither `stm32l4xx_hal_spi.c` nor the FatFs middleware, and
both can only be added by regenerating from CubeMX with new middleware
selected. Rather than make that a prerequisite, the repo carries:

- `sd_spi.c` — SD/SDHC block driver written against the SPI1 registers
- `fat32.c` — append-only FAT32 writer, about 600 lines

`fat32.c` does FAT32 only, 512-byte sectors only, 8.3 names in the root
directory only, create and append only. That covers "open a new log file and
stream frames into it" completely, and produces a normal FAT32 volume that
Windows, macOS and Linux mount without complaint.

It is validated by `tools/test_fat32.py`, which formats real disk images with
`mkfs.vfat` across seven cluster-size and partition-layout combinations, runs
the writer, then checks the result with `fsck.fat` **and** with an independent
FAT32 parser that walks the cluster chain and compares every byte. 199 checks.
`fsck` alone is not enough — it validates structure, and would happily pass a
volume whose file is structurally perfect and full of garbage.

If you would rather have the real thing later, add FatFs in CubeMX and swap
`sd_log.c` back to `f_open`/`f_write`. Nothing above it cares.

### Log files

`LOG0001.TLM`, `LOG0002.TLM`, … lowest unused index at each power-up, so
nothing is ever overwritten. Contents are the exact bytes that went over the
air, which means:

```
python run.py --replay LOG0001.TLM     # through the live decoder
python tools/tlm_to_csv.py LOG0001.TLM # to a spreadsheet
```

`fat32_sync()` runs every 20 frames — a 10-second worst-case loss window on an
unclean power-down. Lower `SD_SYNC_EVERY_N_FRAMES` in `sd_log.c` if the hub is
likely to lose power abruptly; the cost is roughly three times the card writes.

## Bench-test order

Each stage has a clean pass/fail. Do not skip ahead.

1. **CAN only.** Set `HUB_ENABLE_SD 0` in `telemetry_hub.h`, leave the radio
   disconnected. Watch USART2 at 115200 through the ST-Link VCP. One line every
   500 ms, three nodes reporting.
2. **Pull one node's power.** That node should show `!` within 1.5 s.
3. **Add the radio.** Plug the `-U` dongle into the PC,
   `python run.py --port COMx --raw`. Frames appear on the dashboard.
4. **Add the SD card.** Set `HUB_ENABLE_SD 1`. `LOG0001.TLM` should appear and
   grow by 42 bytes every 500 ms.
5. **Unplug the CAN bus mid-run, then plug it back in.** The hub should recover
   on its own, with `busoff_events` incremented in `hub_get_stats()`.
