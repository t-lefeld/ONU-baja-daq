# Pit Dashboard

Real-time telemetry dashboard for pit use during competition and testing.

**Frontend:** Single-file browser dashboard (`baja_telemetry_dashboard.html`) — no install, no build step, 100% offline
**Backend:** FastAPI + SQLite (`backend/`) — optional persistent, cross-session logging service
**Input:** ESP32 receiver via USB serial (Web Serial API) or WiFi

> Originally planned as a Flask/FastAPI + PyQtGraph desktop app (see
> `docs/build-log/02_lora_telemetry.md`). Pivoted to a browser-based frontend —
> see `docs/build-log/04_web_dashboard_pivot.md` for why.

## Running the Dashboard

No install required:

```bash
open telemetry/dashboard/baja_telemetry_dashboard.html   # or just double-click it
```

Open in **Chrome or Edge** (desktop) — Web Serial isn't supported in Firefox
or Safari. Click **Connect LoRa** and pick the ESP32's USB serial port, or
click **Demo Mode** to try it with simulated data first.

## Layout

Tabbed interface:

| Tab | Contents |
|-----|----------|
| Overview | Cards for every channel, grouped by subsystem (engine/drivetrain, chassis/suspension, GPS) |
| Charts | Live strip charts + GPS trail map |
| Session | Lap timer, run log, CSV export |
| Debug | Raw incoming packet feed |

A pinned strip at the top always shows radial gauges for the most
safety-critical channels (RPM, engine/CVT temp, battery voltage) regardless of
which tab is open, along with any active alarms.

## Adding a Sensor

Everything needed to add a new channel lives in the `CONFIG` section at the
top of the `<script>` block in the HTML file — see the comment block there
for a step-by-step template. New channels get a card, gauge (optional),
strip chart (optional), settings-panel row, and CSV export column
automatically; no other editing required.

## Data Logging

Two independent options, not mutually exclusive:

- **Client-side (built into the dashboard):** every session's data lives in
  browser memory and exports to CSV on demand ("Export CSV" / "New Run").
  Works with zero setup, but data lives only in that browser tab.
- **Server-side (`backend/`):** a FastAPI service with SQLite storage for
  persistent, crash-safe, cross-session logging, independent of any one
  browser tab. See `backend/README.md` for setup and the API.

  **Wired into the dashboard:** open the Session tab, enter the backend URL
  (e.g. `http://localhost:8000`) under "Server Logging," and click Connect.
  The dashboard health-checks the URL, starts a backend session, and from
  then on forwards every parsed telemetry packet to it via `POST /telemetry`
  in addition to the local in-browser log — no server restart or dashboard
  reload needed. A status pill shows Connecting / Logging / Offline
  (auto-retrying), and clicking "New Run" starts a fresh backend session to
  match the local one. This is optional and additive: if the backend is
  unreachable or disabled, the dashboard works exactly as before, purely
  client-side.

## Architecture

```
[ESP32 RX] ──USB serial──► [Browser: Web Serial API] ──► [Dashboard UI]
                                                      ├──► [CSV export]
                                                      └──► [backend/ FastAPI] ──► [SQLite]
                                                           (optional, if Server Logging is enabled)
```

## Requirements

Dashboard: none — any modern Chrome/Edge.
Backend: see `backend/requirements.txt` (FastAPI, uvicorn).
