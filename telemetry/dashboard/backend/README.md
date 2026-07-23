# Baja Pit Telemetry Logger

Pit-side FastAPI backend that logs telemetry forwarded from the browser
dashboard (which itself receives CAN bus data over LoRa via a USB LoRa
receiver, E22-900T22U) to disk, with
storage treated as a swappable filesystem path rather than specific hardware.
Validated end-to-end (write, rotate, flush, read-back) against a plain
folder standing in for the SD card — no physical card needed for this pass.

## Storage abstraction

Everything reads/writes through `LOG_STORAGE_PATH` (see `app/config.py`).

- Default: `./data/sd_mock/` — a local folder for dev/testing.
- Production: set `LOG_STORAGE_PATH=/media/sdcard` (Linux) or a drive
  letter/path on Windows once the SD reader is plugged in. No code changes,
  just the env var / config value.

```bash
export LOG_STORAGE_PATH=/media/sdcard
export LOG_FLUSH_INTERVAL_SECONDS=0.5   # optional, default 0.5s
uvicorn app.main:app --host 0.0.0.0 --port 8000
```

## Format decision: SQLite

One `.db` file per session, named `session_<timestamp>_<random>[_<name>].db`
(e.g. `session_20260720_174517_9c4841be_endurance_run.db`) so runs never
collide or clobber each other even if started within the same second.

Chosen over CSV/JSON-lines because post-run analysis and dashboard replay
can filter/sort with plain SQL (by time range, CAN id, etc.) without
parsing text. **If the dashboard already expects CSV or JSON-lines, this
is the one piece to swap** — the `SessionLogger` class in `app/storage.py`
is the only place that would need a different backend; the API surface
and rotation/flush behavior stay the same.

## Telemetry schema (assumption — flag this)

The exact CAN signal layout wasn't specified, so incoming packets are:

```json
{
  "device_ts": 1721500000.123,   // optional, timestamp from receiver/STM32
  "can_id": "0x100",             // optional, CAN arbitration id
  "seq": 42,                     // optional, packet sequence number
  "rssi": -71.0,                 // optional, LoRa signal strength
  "payload": {"rpm": 4200, "speed_kph": 61.5}   // arbitrary decoded fields
}
```

`payload` is an open dict so any set of decoded signals can be logged
without a schema migration. Adjust once the real STM32/LoRa packet format
and the dashboard's expected fields are finalized.

## API

- `POST /sessions/start` `{"name": "optional_label"}` — rotates to a new
  session file, flushing/closing any previous one.
- `POST /sessions/stop` — flushes and closes the active session.
- `POST /telemetry` — appends one packet to the active session (409 if none).
- `GET /sessions` — lists session files on the current storage path.
- `GET /sessions/{session_id}` — row count + active flag.
- `GET /sessions/{session_id}/telemetry?limit=&offset=` — read-back.
- `GET /health` — configured storage path + active session id.

## Durability

Writes are inserted into SQLite immediately but committed (`flush()`) on a
background timer (`LOG_FLUSH_INTERVAL_SECONDS`, default 0.5s) rather than
per-packet, plus always on `/sessions/stop`. WAL journal mode +
`synchronous=NORMAL` bound data loss on a crash to roughly one flush
interval, without fsyncing every single high-rate packet.

## Tests

```bash
pip install -r requirements.txt --break-system-packages
python -m pytest -v
```

14 tests, all passing:

- `tests/test_storage.py` — unit tests against `SessionManager` directly,
  storage path from `tempfile.mkdtemp()`: file creation, append/read-back,
  rotation not clobbering a prior session, and a concurrent multi-threaded
  write burst (8 threads × 200 packets) checked for zero dropped writes.
- `tests/test_api.py` — integration tests via FastAPI `TestClient` against
  a temp-dir storage path: start/write/read-back roundtrip against known
  data, rotation via the API, a 10-worker × 100-packet concurrent write
  burst through the HTTP layer, and stop-session read-back.

## Explicitly out of scope (needs real hardware)

- SD card write speed under sustained load.
- Power-loss-mid-write corruption handling (affects whatever device
  ultimately holds the card on the vehicle, not this backend's logic).
- FAT filesystem capacity/rollover edge cases.

## Project layout

```
baja_logger/
  app/
    config.py     # LOG_STORAGE_PATH / flush interval
    storage.py    # SessionLogger (SQLite, thread-safe) + SessionManager (rotation)
    main.py        # FastAPI app factory + endpoints
  tests/
    test_storage.py
    test_api.py
  requirements.txt
```
