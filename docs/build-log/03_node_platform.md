# 03 — Node Platform Selection

**Date:** 2026-06  
**Type:** Component selection

> **Superseded in part:** the eCVT node described below as a fourth STM32
> Blue Pill corner node was replaced by a commercial ODrive S1 assembly once
> mechanical specs came in — see `05_receiver_and_scope_corrections.md`.
> Front/Rear/Firewall platform selection below is still current.

## Overview

Selected MCU platforms for corner nodes and the firewall aggregator node.

## Corner Nodes: STM32 Blue Pill (STM32F103C8T6)

### Why STM32 over Arduino

The STM32F103 has a hardware CAN peripheral — no software CAN or external CAN controller needed. Arduino (ATmega328P) has no hardware CAN and would require an MCP2515 SPI bridge, adding latency, complexity, and a failure point.

The Blue Pill also runs at 72MHz vs 16MHz, has more ADC channels, and costs the same (~$2 bare board).

### Why Blue Pill over a custom STM32 board

Blue Pill is breadboard-friendly, widely available, and the team has existing toolchain experience with it (STM32CubeIDE + HAL). A custom board is planned for a future revision once sensor interfaces are finalized — the Blue Pill lets firmware development proceed in parallel with hardware design.

Tradeoffs accepted:
- 5V tolerant pins require level shifting for some 5V sensors — manageable
- CAN peripheral requires external SN65HVD230 transceiver — small, cheap, proven

## Firewall Node: STM32 Nucleo L476RG

### Why a Different Platform for Firewall

The firewall node has significantly more responsibilities than a corner node: CAN aggregation, SD logging, LoRa TX, and power distribution. The Nucleo L476RG provides:

- More SRAM (128KB vs 20KB on Blue Pill) — needed for CAN RX buffer and SD write buffer
- Hardware SD interface (SDIO) — faster and more reliable than SPI SD on Blue Pill
- Arduino-compatible header — simplifies LoRa module attachment
- On-board ST-Link debugger — no external programmer needed, important for field debugging

The Nucleo runs on a custom carrier PCB that breaks out the Deutsch DT connector interface and switched power outputs.

## Key Takeaways

- Hardware CAN peripheral is non-negotiable — software CAN introduces jitter that corrupts timing-sensitive data
- Platform selection should match responsibility: corner nodes are simple sensors, the firewall is an embedded computer
- Blue Pill as a development platform is a deliberate choice — custom PCB is the target for production, not the starting point
