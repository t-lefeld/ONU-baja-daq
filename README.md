# Baja SAE — DAQ System

Data acquisition, CAN bus, and LoRa telemetry system for a university Baja SAE vehicle. Covers all sensor nodes, the firewall aggregator, and real-time pit telemetry.

![Status](https://img.shields.io/badge/status-in%20progress-yellow)
![License](https://img.shields.io/badge/license-MIT-blue)
![CAN](https://img.shields.io/badge/CAN-500kbps-blue)
![LoRa](https://img.shields.io/badge/LoRa-915MHz-green)

---

## System Overview

```
 VEHICLE
 ┌─────────────────────────────────────────────────────┐
 │                                                      │
 │  [Front Node]        [Rear Node]                     │
 │  STM32 Blue Pill     STM32 Blue Pill                 │
 │  Wheel encoder,      Wheel encoder,                  │
 │  bellcrank pot,      bellcrank pot,                  │
 │  pressure xducers    E-CVT belt temp (MLX90614)      │
 │       │                    │                         │
 │       └────────────────────┘                         │
 │                  CAN Bus (500kbps)                   │
 │                  Deutsch DT over                     │
 │                  Ethernet twisted pair                │
 │                       │                              │
 │              [Firewall Node]                         │
 │              Nucleo L476RG                           │
 │              LoRa TX (E32-900T30D), GPS, 9DOF IMU,   │
 │              SD logging, CAN analyzer tap,           │
 │              power distribution                      │
 └───────────────────────┬──────────────────────────────┘
                         │ LoRa 915MHz
                         │ 5-channel frequency hopping
                         ▼
 PIT
 ┌──────────────────────────────────────────────────────┐
 │  [LoRa USB Receiver]                                  │
 │  E22-900T22U (USB, no MCU)                            │
 │       │                                               │
 │  [Browser Dashboard] ──optional──► [FastAPI/SQLite]   │
 │  Web Serial API          POST /telemetry              │
 └──────────────────────────────────────────────────────┘
```

**E-CVT actuation** (`nodes/ecvt/`) is a separate self-contained assembly, not a CAN
sensor node: an ODrive S1 controller drives the CVT actuator motor (ODrive D5065,
built-in thermistor) using its own onboard encoder (ODrive Encoder OA1) and a
2Ω/50W brake resistor for regen dumping. See `nodes/ecvt/README.md`.

---

## Subsystems

### Nodes (`/nodes/`)

| Node | MCU | Location | Key Sensors |
|------|-----|----------|-------------|
| Front | STM32 Blue Pill | Front suspension | Wheel hub encoder (Littelfuse 55075), bellcrank pot (Bourns 53AAA-B28-B15L), pressure transducers (Anfield T200/T201) |
| Rear | STM32 Blue Pill | Rear suspension | Wheel hub encoder (Littelfuse 55075), bellcrank pot (Bourns 53AAA-B28-B15L), E-CVT belt temp (MLX90614, I2C) |
| eCVT | ODrive S1 (onboard controller) | CVT assembly | Not a CAN sensor node — self-contained ODrive S1 + D5065 motor (built-in thermistor) + Encoder OA1 + 2Ω/50W brake resistor |
| Firewall | Nucleo L476RG | Firewall | CAN aggregator, SD log, LoRa TX (E32-900T30D), GPS (HGLRC Mini M100), 9DOF IMU (BNO085), CAN analyzer tap, power dist |

### CAN Bus (`/can/`)

- 500kbps, Deutsch DT connectors over Ethernet twisted-pair
- DBC file defines all message IDs, signals, and scaling
- Decoded with `cantools` in the dashboard and logging scripts

### Telemetry (`/telemetry/`)

- LoRa 915MHz, 5-channel frequency hopping for interference resilience
- Packet format: header + checksum + sequence number
- Pit receiver is a LoRa USB dongle (E22-900T22U) — no onboard MCU, appears
  directly as a USB serial port
- Browser-based dashboard (Web Serial API) reads it directly; optional
  FastAPI/SQLite backend for persistent cross-session logging

### Suspension Travel (`/suspension/`)

- Sealed Bourns rotary potentiometer on custom bellcrank linkage
- 6061-T6 aluminum bellcrank, parametric Fusion 360 CAD
- Linear shock-to-rotation relationship — ADC moving-average filter only

### Hardware (`/hardware/`)

- Firewall node: Nucleo L476RG carrier PCB
- Enclosures: PAHT-CF (IP66), designed for Bambu P1S
- Connectors: Deutsch DT (power/signal) and DTM (signal) throughout

---

## Repository Structure

```
baja-daq/
├── README.md
├── LICENSE
├── nodes/
│   ├── front/           ← STM32 Blue Pill, front corner firmware
│   ├── rear/            ← STM32 Blue Pill, rear corner firmware
│   ├── ecvt/            ← ODrive S1-based E-CVT actuation (not a CAN sensor node)
│   └── firewall/        ← Nucleo L476RG, SD + LoRa + GPS + IMU + power dist
├── telemetry/
│   ├── lora/            ← Radio config, packet format, freq hopping
│   └── dashboard/       ← Browser-based (Web Serial) pit dashboard + FastAPI/SQLite logging backend
├── can/
│   ├── baja.dbc         ← DBC message definitions
│   └── README.md        ← Bus topology, node IDs, signal list
├── hardware/
│   ├── firewall-pcb/    ← Nucleo carrier board files
│   ├── enclosures/      ← PAHT-CF print files, IP66 spec
│   └── connectors/      ← Deutsch DT/DTM pinout reference
├── suspension/
│   └── travel-sensor/   ← Bourns pot, bellcrank CAD notes
├── docs/
│   ├── architecture.md  ← Full system documentation
│   ├── build-log/       ← Dated decision and milestone entries
│   └── images/          ← Photos, scope captures, diagrams
└── scripts/
    └── can_decode.py    ← cantools DBC decode utility
```

---

## Quick Start

### Decoding CAN logs

```bash
pip install cantools
python scripts/can_decode.py --dbc can/baja.dbc --log <logfile.asc>
```

### Running the pit dashboard

```bash
open telemetry/dashboard/baja_telemetry_dashboard.html   # or just double-click it
```

Open in Chrome or Edge (desktop) and connect to the LoRa USB receiver (E22-900T22U) over USB serial, or click Demo Mode to try it with simulated data. No install, no build step — see `telemetry/dashboard/README.md` for details, including the optional FastAPI/SQLite persistent-logging backend.

---

## Build Status

| Subsystem | Status | Notes |
|-----------|--------|-------|
| CAN bus architecture | ✅ Complete | 500kbps, DBC defined |
| Firewall node | 🔄 In progress | SD logging, LoRa TX, GPS, IMU |
| Front node | 🔄 In progress | Sensor integration |
| Rear node | 🔄 In progress | Sensor integration |
| eCVT actuation | 🔄 In progress | ODrive S1 assembly, mech specs confirmed |
| LoRa telemetry | 🔄 In progress | Freq hopping, packet format |
| Pit dashboard | ✅ Wired | Browser dashboard (Web Serial) forwards to FastAPI/SQLite logger |
| Suspension travel sensor | 🔄 In progress | Bellcrank CAD in progress |

---

## License

MIT — see [LICENSE](LICENSE)
