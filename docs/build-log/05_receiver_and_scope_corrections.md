# 05 — Receiver Hardware Correction, eCVT Clarification, Steering Wheel Descope

**Date:** 2026-07
**Type:** Architecture change / documentation correction

## Overview

A documentation audit against the team's actual current hardware decisions
(PCB schematics and enclosure design work done since `02_lora_telemetry.md`
and `03_node_platform.md` were written) turned up three places where the
docs no longer matched reality. This entry corrects all three and records
why, rather than silently rewriting the historical entries that recorded the
original (now-superseded) decisions.

## 1. Pit receiver: ESP32 → E22-900T22U USB LoRa dongle

`02_lora_telemetry.md` documented an ESP32 as the pit-side receiver — LoRa RX
over SPI, serving a WiFi AP for the dashboard plus a USB serial backup. That
was written against the original Flask/FastAPI + PyQtGraph dashboard plan.

Once `04_web_dashboard_pivot.md` moved the frontend to a browser-based
dashboard using the Web Serial API, the WiFi AP path stopped being needed —
the browser only ever needs a USB serial port to talk to. That removes the
ESP32's reason for existing: there's no longer any logic running at the
receiver that a plain LoRa module can't do on its own.

**Current hardware:** E22-900T22U — a USB-interface LoRa module. It plugs
directly into the pit laptop's USB port and enumerates as a serial device,
with no microcontroller or firmware layer in between. Simpler, one fewer
thing to flash or debug in the field.

Updated: root `README.md`, `docs/architecture.md`, `telemetry/lora/README.md`,
`telemetry/dashboard/README.md`, `telemetry/dashboard/backend/` (README +
code comments/docstrings), `docs/build-log/04_web_dashboard_pivot.md`.

## 2. eCVT: clarified as an ODrive S1 assembly, not a custom sensor node

`03_node_platform.md` and the original node table treated eCVT as a fourth
STM32 Blue Pill corner node (CAN ID 0x030), blocked pending mechanical specs.

Those specs have since come in, and the actual eCVT actuation design is a
commercial ODrive S1 motor controller assembly (ODrive S1 + D5065 motor with
built-in thermistor + Encoder OA1 + a 2Ω/50W brake resistor) — not a custom
PCB with ADC/GPIO sensor wiring. Primary/secondary RPM and actuator position
live inside the ODrive's own FOC telemetry rather than needing separate
sensors and firmware. CVT belt temperature is read by the **Rear** node's
MLX90614 instead, since it's physically closer to the belt.

Whether this assembly reports over the vehicle CAN bus (using the previously
reserved 0x030 ID) or stays on the ODrive's own native protocol is still an
open question for the team — not decided as part of this correction.

Updated: root `README.md` (diagram + node table + build status),
`nodes/ecvt/README.md` (full rewrite), `can/README.md` (marked 0x030 as
reserved/unconfirmed), `hardware/enclosures/README.md`.

Also brought `nodes/front/README.md` and `nodes/rear/README.md` up to date
with the actual sensor lists confirmed during enclosure/connector design
(wheel hub encoders, bellcrank pots, pressure transducers, MLX90614 belt
temp) — these had been left at an earlier, sparser placeholder state
("steering angle TBD", "drivetrain sensor TBD"). Also added GPS (HGLRC Mini
M100) and a 9DOF IMU (BNO085) to `nodes/firewall/README.md`, which were
part of the firewall's actual component list but had never been documented.

## 3. Steering wheel node / Nextion display: removed

The root README's system diagram, repository structure tree, and Build
Status table all referenced a driver-facing Nextion display node on the
steering wheel. This is no longer part of the plan — removed entirely
rather than marked as deferred, per direction from the team.

Updated: root `README.md` only (no `telemetry/nextion/` folder or code
existed yet, so there was nothing else to remove).

## Key Takeaways

- Docs drift fastest right at an architecture pivot (like the PyQtGraph→
  browser move) — a decision that removes a *reason* for a component to
  exist (WiFi AP) doesn't automatically get the component's other mentions
  cleaned up everywhere they appear.
- Historical build-log entries are left alone rather than rewritten when a
  decision changes — this entry supersedes the relevant parts of `02` and
  `03` in the same way `04` superseded the frontend part of `02`, and the
  index (`docs/build-log/README.md`) is what actually points readers at the
  current state.
