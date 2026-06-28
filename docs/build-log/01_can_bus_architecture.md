# 01 — CAN Bus Architecture

**Date:** 2026-06  
**Type:** Design decision

## Overview

Defined the CAN bus topology, physical layer, connector system, and baud rate for the Baja DAQ system.

## Requirements

- Reliable multi-node communication in a high-vibration, high-EMI environment
- Simple wiring — the harness runs the length of the vehicle and must be field-serviceable
- Compatible with standard automotive CAN tooling for log analysis
- Budget-conscious — university team

## Decisions

### Baud Rate: 500kbps

500kbps is the automotive standard and well within the capability of the STM32 CAN peripheral. It provides ample bandwidth for all planned sensor data at the required sample rates with headroom for future expansion. 1Mbps was considered but offers no practical benefit at the node counts and cable lengths involved.

### Physical Layer: Ethernet CAT5e Twisted Pair

Dedicated CAN cable (e.g. Belden 9841) is the textbook choice but expensive and difficult to source in small quantities. CAT5e twisted pair provides the same differential pair geometry and characteristic impedance (~100Ω, slightly above the CAN spec of 120Ω but acceptable at 500kbps over short runs). It is cheap, available everywhere, and the stranded variant is flexible enough for a vehicle harness.

### Connectors: Deutsch DT Series

Deutsch DT connectors provide IP67 sealing, vibration resistance, and positive locking. They are the industry standard for off-road vehicle harnesses. The DT series handles the 12-20 AWG wire used for power and CAN trunk; the DTM series handles smaller-gauge sensor connections.

Alternative considered: Molex MX150 (automotive grade, cheaper) — rejected because DT is more serviceable in the field and the team has prior experience with the crimping tools.

### Termination

120Ω termination resistors at each end of the bus. The firewall node (one bus end) and the farthest corner node (other bus end) each have a terminator. Stub lengths at each node tap are kept under 30cm to minimize reflections at 500kbps.

## Key Takeaways

- CAT5e twisted pair is a practical substitute for dedicated CAN cable at low node counts and short bus lengths
- Deutsch DT connectors add cost but pay for themselves in field reliability and serviceability
- Static node IDs keep log files self-describing — no dynamic address negotiation needed at this scale
