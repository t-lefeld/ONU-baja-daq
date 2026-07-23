# 04 — Pit Dashboard: Browser-Based Instead of PyQtGraph

**Date:** 2026-07
**Type:** Design decision (supersedes frontend portion of `02_lora_telemetry.md`)

## Overview

The pit dashboard frontend was originally planned as a Flask/FastAPI backend
serving a PyQtGraph desktop UI. Replaced the frontend with a single-file
browser dashboard using the Web Serial API instead.

## Why

- **Zero install on pit-side laptops.** PyQtGraph requires a Python
  environment with the right packages installed on whatever laptop shows up
  at competition. The browser dashboard is one HTML file — open it in Chrome
  and it works, including on a laptop that's never seen this repo before.
- **No backend required for live viewing.** The Web Serial API lets the
  browser talk to the ESP32's USB serial output directly, so there's no Flask
  process to start, no port to remember, nothing to crash independently of
  the UI.
- **Fully offline.** No CDN dependencies, no network calls of any kind —
  matters at competitions where venue WiFi/cell service is unreliable or
  banned in the pit area.
- **Cross-platform by default.** Runs anywhere Chrome or Edge runs, instead
  of depending on a Python + Qt install matching whoever's laptop is
  available that day.

## Trade-offs

- Web Serial is Chromium-only (Chrome/Edge) — no Firefox or Safari support.
  Acceptable since we control which laptop/browser is used in the pit.
- Loses PyQtGraph's native desktop performance for very high-density plots.
  Not a concern at our packet rates.
- The FastAPI + SQLite backend (`telemetry/dashboard/backend/`) is kept as an
  optional, separate persistent logging service rather than being deleted —
  it's still useful for crash-safe cross-session storage, just no longer the
  thing rendering the live UI.

## Status

Dashboard is built and includes gauges, alarms, a stale-data/link-health
indicator, CSV export, and a lap timer. The FastAPI/SQLite backend is built
and tested independently. Wiring the two together (forwarding serial data
into the backend for persistent server-side logging) is not yet done — see
`telemetry/dashboard/README.md`.

## Key Takeaways

- For a pit-side tool used by a rotating cast of people on whatever hardware
  is on hand, "runs in any browser" beats a more powerful but install-heavy
  desktop GUI.
- Keeping the persistent-logging backend decoupled from the live-view
  frontend turned out to be the right call anyway — they can now evolve (or
  fail) independently.
