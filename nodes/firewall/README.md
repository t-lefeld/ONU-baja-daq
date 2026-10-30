# Firewall Node

**MCU:** STM32 Nucleo L476RG  
**Role:** CAN bus aggregator, SD data logger, LoRa transmitter, GPS + IMU acquisition, power distribution hub  
**Language:** C++  

## Responsibilities

- Receives all CAN messages from the corner nodes and the eCVT's ODrive S1
- Logs raw CAN frames to SD card (continuous, independent of LoRa link)
- Reads GPS position/speed and 9DOF IMU (orientation/acceleration)
- Packetizes and transmits data (CAN + GPS + IMU) over LoRa at 915MHz
- Distributes power to every other DAQ node from the DeWalt packs
- Hosts main vehicle harness connector interface
- Provides an internal CAN analyzer tap for diagnostics (lid-accessible)

## Hardware

- Custom STM32L476RG board (Nucleo-variant design, not a stock Nucleo)
- LoRa TX module: **E22-900T22D**, UART (the `-U` variant is the USB dongle at the pit end, not this one). Matches `firmware/nucleo_hub/lora_e22.c`
- GPS: HGLRC Mini M100, UART, with external active antenna. (Brand is
  **HGLRC** — "HCLRC" is a common transposition and finds nothing.) The M100
  is built on a 10th-generation u-blox chip, which is why older docs here
  referred to a u-blox part
- 9DOF IMU: BNO080, I2C (SCL/SDA on PB6/PB7, INT on PA0, RST on PA1). Some docs say BNO085; the two are interchangeable here — both speak SH-2 and the `imu_bno080` driver covers either
- SD card on SPI (separate CS from LoRa)
- CAN analyzer tap: 3-pin JST-XH/PH pigtail spliced onto the CAN bus wiring
  inside the enclosure — internal only, no environmental sealing needed since
  it's only accessed with the lid open. Keep the stub short; do not add a
  120Ω termination here unless this box is genuinely a bus end
- Deutsch DT connectors for CAN bus in/out and power out
- PAHT-CF enclosure, IP66 rated (see `/hardware/enclosures/`)
- Dual battery input with LTC4412 ideal-diode OR-ing (P-MOSFETs, TVS + fuse
  per input) feeding a buck converter for the regulated rail

## LoRa Packet Format

```
[ Header (2B) | Node ID (1B) | Sequence (2B) | Payload (nB) | Checksum (2B) ]
```

- Header: `0xDADA` sync word
- Sequence: rolling counter, increments per packet — used by receiver to detect drops
- Checksum: CRC16 over Node ID + Sequence + Payload

## SD Logging

- Logs raw CAN frames in ASC format (compatible with `cantools` and Vector CANalyzer)
- New file created on each power cycle: `LOG_NNNN.asc`
- Write buffer flushed every 500ms to prevent data loss on power cut

## Power Distribution

**The firewall node powers the entire DAQ system, and it always will.** Every
battery lands here. This is deliberate: noise filtering is painful to get
right, so it is done once, in one place, rather than repeated badly at every
node.

**Source:** DeWalt 20V MAX packs (5S, 15.0–21.0 V across the usable range).
Two packs combined through the LTC4412 ideal-diode ORing described above.

| Output | Voltage | Load |
|--------|---------|------|
| CAN nodes | 12V switched | Front, Rear, Display nodes |
| LoRa radio | 3.3V | Dedicated supply — see note |
| SD card | 3.3V | Onboard regulator |

The eCVT is **not** on this list. The ODrive S1 runs from its own
stator-powered supply, separately rectified and filtered. A BLDC motor
tolerates supply noise far better than these sensor nodes do, so its
filtering can be simpler — but keep the two supplies distinct.

**Give the E22 its own supply.** It draws over 600 mA in transmit bursts at
22 dBm, more than an onboard regulator will provide. 470 µF or more at the
module. Getting this wrong looks like a link that works on the bench and dies
when you raise power or packet length.

### V1 vs. final board

The **V1 board is built for validation only**: single supply, no ORing
circuit, and fewer on-board sensors than the final board. The voltage monitor
— pack voltages, ORing behavior, and which pack is currently carrying the
load — lands on a later revision, not V1.

## Firmware Structure

```
firewall/
└── src/
    ├── main.cpp
    ├── can_rx.cpp/.h       ← CAN receive and frame parsing
    ├── sd_log.cpp/.h       ← SD card write buffering
    ├── lora_tx.cpp/.h      ← LoRa packet assembly and transmit
    └── power.cpp/.h        ← Switched power rail control
```
