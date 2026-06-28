# Pit Dashboard

Real-time telemetry dashboard for pit use during competition and testing.

**Backend:** Flask / FastAPI  
**Frontend:** PyQtGraph (tabbed layout)  
**Database:** SQLite (persistent cross-session logging)  
**Input:** ESP32 receiver via WiFi or USB serial

## Running the Dashboard

```bash
cd telemetry/dashboard
pip install -r requirements.txt
python app.py
```

Dashboard available at `http://localhost:5000`.  
ESP32 must be on the same WiFi network or connected via USB serial.

## Layout

Tabbed PyQtGraph interface:

| Tab | Signals |
|-----|---------|
| Suspension | Front travel, rear travel (live + history) |
| eCVT | Primary RPM, secondary RPM, belt temp, actuator pos |
| System | Packet rate, dropped packets, link RSSI, session time |

## Data Logging

- SQLite database: `logs/session_YYYYMMDD_HHMMSS.db`
- New session file created on each dashboard launch
- All received packets written regardless of display state
- Export to CSV: `python scripts/export_csv.py --session <file.db>`

## Architecture

```
[ESP32 RX] ──serial/WiFi──► [receiver.py] ──► [SQLite]
                                   │
                             [Flask API]
                                   │
                           [PyQtGraph UI]
```

## Requirements

See `requirements.txt`. Key dependencies:

- `flask` or `fastapi` + `uvicorn`
- `pyqtgraph`
- `pyserial`
- `cantools`
- `sqlite3` (stdlib)
