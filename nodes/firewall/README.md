# Firewall Node

**MCU:** STM32 Nucleo L476RG  
**Role:** CAN bus aggregator, SD data logger, LoRa transmitter, power distribution hub  
**Language:** C++  

## Responsibilities

- Receives all CAN messages from corner nodes
- Logs raw CAN frames to SD card (continuous, independent of LoRa link)
- Packetizes and transmits data over LoRa at 915MHz
- Distributes switched power to corner nodes
- Hosts main vehicle harness connector interface

## Hardware

- Nucleo L476RG on custom carrier PCB (see `/hardware/firewall-pcb/`)
- LoRa radio module (SX1276 or equivalent) on SPI
- SD card on SPI (separate CS from LoRa)
- Deutsch DT connectors for CAN bus in/out and power out
- PAHT-CF enclosure, IP66 rated (see `/hardware/enclosures/`)

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
