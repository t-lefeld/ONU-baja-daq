# System Architecture

## Design Philosophy

The DAQ system is designed around three priorities:

1. **Data is never lost.** The firewall node logs raw CAN frames to SD card continuously and independently of the LoRa link. Even a complete telemetry failure means post-session data is available.
2. **Nodes are independently recoverable.** Each corner node runs its own watchdog timer. A hung node resets itself and resumes CAN TX without taking down the rest of the bus.
3. **The system is maintainable by new team members.** The DBC file is the single source of truth for all signal definitions. Anyone with `cantools` can decode any log file without needing to read firmware.

## CAN Bus

500kbps over Ethernet twisted pair with Deutsch DT connectors. Chosen over a dedicated CAN cable for availability, cost, and the differential pair geometry (matched impedance, common-mode noise rejection).

Node IDs are statically assigned. No dynamic address negotiation — simplifies firmware and makes log files self-describing.

## LoRa Link

915MHz with 5-channel frequency hopping. The hopping pattern is deterministic (seeded pseudo-random), so TX and RX stay in sync without a back-channel. Sequence numbers allow the receiver to detect dropped packets and report link quality on the dashboard.

The LoRa link is best-effort. If it drops, SD logging on the firewall continues. The dashboard displays link quality as a first-class metric so the pit crew knows when to trust real-time data.

## SD Logging

ASC format (raw CAN frames with timestamps). Compatible with `cantools`, Vector CANalyzer, and most automotive CAN analysis tools. A new log file is created on each power cycle using a rolling index (`LOG_0001.asc`, `LOG_0002.asc`, etc.).

## Dashboard

Two-tier: a backend (Flask or FastAPI) that ingests serial/WiFi data from the ESP32 and writes to SQLite, and a PyQtGraph frontend that reads from the database. The database decouples the display update rate from the incoming packet rate and provides persistent logging across dashboard restarts.

## Power

All nodes powered from a common 12V switched rail distributed from the firewall node. The firewall controls the power rail via a switched output — corner nodes are only live when the firewall is powered and the rail is enabled. This prevents nodes from draining the vehicle battery when the system is idle.
