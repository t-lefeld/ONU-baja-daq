# 02 — LoRa Telemetry Architecture

**Date:** 2026-06  
**Type:** Design decision

## Overview

Defined the full telemetry stack from vehicle to pit: LoRa radio link, ESP32 receiver, WiFi dashboard, and SQLite logging.

## Requirements

- Real-time data visible in the pit during testing and competition
- Must work at competition where 2.4GHz WiFi is heavily congested
- Data must not be lost if the radio link drops
- Cross-session logging — data persists between dashboard restarts
- Low cost and easy to field-debug

## Decisions

### Radio: LoRa 915MHz

915MHz ISM band with LoRa modulation. Selected over:
- **2.4GHz WiFi direct**: congested at competition, range limited
- **XBee**: higher cost, more complex configuration
- **Bluetooth**: range inadequate for pit-to-vehicle distance

LoRa provides 100m+ reliable range at competition distances and operates in a less congested band than 2.4GHz.

### Frequency Hopping: 5 Channels

5-channel pseudo-random hopping across 915.0–917.0MHz. The hop pattern is deterministic (seeded at powerup), so TX (firewall) and RX (ESP32) stay synchronized without a back-channel. If the receiver detects a sequence number gap it re-syncs on the next valid packet header — no manual intervention required.

Hopping was added after early testing showed interference from other teams' equipment at a regional competition.

### Receiver: ESP32

ESP32 handles LoRa RX via SPI, serves a local WiFi access point for the pit laptop, and exposes USB serial as a backup. Two parallel outputs (WiFi + USB) means the dashboard still works if either path fails.

### Backend: Flask / FastAPI + SQLite

The receiver feeds packets to a Flask or FastAPI backend which writes to SQLite. SQLite was chosen over a flat file for indexed querying and to support the dashboard reading while the backend is writing. Cross-session logging (new session file per launch) means competition data accumulates across the day without manual file management.

### Frontend: PyQtGraph

PyQtGraph provides fast real-time plotting without a browser dependency. Tabbed layout separates suspension, eCVT, and system health views. The dashboard reads directly from SQLite, decoupling display update rate from incoming packet rate.

## Key Takeaways

- SD logging on the firewall is the safety net — LoRa link quality is a convenience, not a data integrity requirement
- Sequence numbers are essential for detecting dropped packets and reporting link health as a first-class metric
- SQLite is underrated for this use case — handles concurrent read/write, persists across crashes, and is trivially queryable
