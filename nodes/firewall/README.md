# Firewall Node

**MCU:** STM32 Nucleo L476RG  
**Role:** CAN bus aggregator, SD data logger, LoRa transmitter, GPS + IMU acquisition, power distribution hub  
**Language:** C++  

## Responsibilities

- Receives all CAN messages from corner nodes
- Logs raw CAN frames to SD card (continuous, independent of LoRa link)
- Reads GPS position/speed and 9DOF IMU (orientation/acceleration)
- Packetizes and transmits data (CAN + GPS + IMU) over LoRa at 915MHz
- Distributes switched power to corner nodes
- Hosts main vehicle harness connector interface
- Provides an internal CAN analyzer tap for diagnostics (lid-accessible)

## Hardware

- Nucleo L476RG on custom carrier PCB (see `/hardware/firewall-pcb/`)
- LoRa TX module: E32-900T30D, UART
- GPS: HGLRC Mini M100, UART
- 9DOF IMU: BNO085, I2C (SCL/SDA on PB6/PB7, INT on PA0, RST on PA1)
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

| Output | Voltage | Load |
|--------|---------|------|
| CAN nodes | 12V switched | Front, Rear, eCVT nodes |
| LoRa radio | 3.3V | From Nucleo onboard reg |
| SD card | 3.3V | From Nucleo onboard reg |

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
