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

Single-file browser dashboard (Web Serial API) reads directly from the pit-side
LoRa USB receiver (E22-900T22U) — no intermediate MCU, WiFi AP, or PyQtGraph
process. This replaced an earlier Flask/FastAPI + PyQtGraph design; see
`docs/build-log/04_web_dashboard_pivot.md` for why.

**The FastAPI + SQLite backend described here was dropped.** It lived in
`telemetry/dashboard/`, which is no longer in the repo. Persistent logging is
now `.TLM` + CSV written by `pc_app/`, plus the firewall's own SD card log.
See the "Ground station" section of the root `README.md`.

## Power

The whole DAQ system runs on **DeWalt 20V MAX packs** (5S, 15.0–21.0 V), not
the vehicle battery. Every pack lands on the firewall node, which carries the
power distribution for the entire system and feeds the other nodes a switched
12V rail. This is deliberate: noise filtering is done once, in one place,
rather than repeated at every node. Corner nodes are only live when the
firewall is powered and the rail is enabled.

The final firewall board ORs two packs through an LTC4412 ideal-diode
arrangement, with a voltage monitor for pack voltages and which pack is
carrying the load. **The V1 board has neither** — single supply, validation
only.

The eCVT is **not** on this rail: the ODrive S1 runs from its own
stator-powered supply, separately rectified and filtered.
