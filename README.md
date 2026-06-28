# Baja SAE — DAQ System

Data acquisition, CAN bus, and LoRa telemetry system for a university Baja SAE vehicle. Covers all sensor nodes, the firewall aggregator, real-time pit telemetry, and driver-facing display.

![Status](https://img.shields.io/badge/status-in%20progress-yellow)
![License](https://img.shields.io/badge/license-MIT-blue)
![CAN](https://img.shields.io/badge/CAN-500kbps-blue)
![LoRa](https://img.shields.io/badge/LoRa-915MHz-green)

---

## System Overview

```
 VEHICLE
 ┌─────────────────────────────────────────────────────┐
 │                                                     │
 │  [Front Node]   [Rear Node]   [eCVT Node]           │
 │  STM32 Blue Pill            STM32 Blue Pill         │
 │       │               │            │                │
 │       └───────────────┴────────────┘                │
 │                  CAN Bus (500kbps)                  │
 │                  Deutsch DT over                    │
 │                  Ethernet twisted pair              │
 │                       │                             │
 │              [Firewall Node]                        │
 │              Nucleo L476RG                          │
 │              SD logging + LoRa TX                   │
 │              Power distribution                     │
 └───────────────────────┬─────────────────────────────┘
                         │ LoRa 915MHz
                         │ 5-channel frequency hopping
                         ▼
 PIT
 ┌─────────────────────────────────────────┐
 │  [ESP32 Receiver]                       │
 │  WiFi AP + USB serial                   │
 │       │              │                  │
 │  [Dashboard]    [SQLite Log]            │
 │  Flask/FastAPI                          │
 │  PyQtGraph UI                           │
 └─────────────────────────────────────────┘

 DRIVER
 ┌─────────────────────────────┐
 │  [Nextion Display]          │
 │  Steering wheel panel       │
 │  Driver-facing live data    │
 └─────────────────────────────┘
```

---

## Subsystems

### Nodes (`/nodes/`)

| Node | MCU | Location | Key Sensors |
|------|-----|----------|-------------|
| Front | STM32 Blue Pill | Front suspension | Suspension travel, steering angle |
| Rear | STM32 Blue Pill | Rear suspension | Suspension travel, drivetrain |
| eCVT | STM32 Blue Pill | CVT | RPM, belt temp, actuator position |
| Firewall | Nucleo L476RG | Firewall | CAN aggregator, SD log, LoRa TX, power dist |

### CAN Bus (`/can/`)

- 500kbps, Deutsch DT connectors over Ethernet twisted-pair
- DBC file defines all message IDs, signals, and scaling
- Decoded with `cantools` in the dashboard and logging scripts

### Telemetry (`/telemetry/`)

- LoRa 915MHz, 5-channel frequency hopping for interference resilience
- Packet format: header + checksum + sequence number
- ESP32 receiver serves WiFi dashboard and USB serial to laptop
- SQLite for persistent cross-session logging

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
│   ├── ecvt/            ← eCVT node (blocked pending mech specs)
│   └── firewall/        ← Nucleo L476RG, SD + LoRa + power dist
├── telemetry/
│   ├── lora/            ← Radio config, packet format, freq hopping
│   ├── dashboard/       ← Flask/FastAPI + PyQtGraph pit dashboard
│   └── nextion/         ← Steering wheel display
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
cd telemetry/dashboard
pip install -r requirements.txt
python app.py
```

Dashboard served at `http://localhost:5000`. ESP32 receiver must be on the same network or connected via USB serial.

---

## Build Status

| Subsystem | Status | Notes |
|-----------|--------|-------|
| CAN bus architecture | ✅ Complete | 500kbps, DBC defined |
| Firewall node | 🔄 In progress | SD logging, LoRa TX |
| Front node | 🔄 In progress | Sensor integration |
| Rear node | 🔄 In progress | Sensor integration |
| eCVT node | ⏸ Blocked | Pending mech specs from team |
| LoRa telemetry | 🔄 In progress | Freq hopping, packet format |
| Pit dashboard | 🔄 In progress | Flask/FastAPI + PyQtGraph |
| Nextion display | 📋 Planned | |
| Suspension travel sensor | 🔄 In progress | Bellcrank CAD in progress |

---

## License

MIT — see [LICENSE](LICENSE)
