"""
Storage layer for the pit-side telemetry logger.

Design goals (per the validation task):
- All I/O goes through one configurable base path (SessionManager.base_path).
  Point it at ./data/sd_mock for dev, or a real SD mount point later -- no
  code changes needed elsewhere.
- One file per session/run, named with a timestamp so runs never clobber
  each other.
- Writes are buffered briefly in SQLite's own transaction and flushed
  (committed) on a timer, not fsynced per-packet -- durability vs. throughput
  tradeoff appropriate for real telemetry rates.
- Thread-safe: a single sqlite3 connection per session, guarded by a lock,
  so concurrent threads/async tasks can append without corrupting the file
  or losing writes.

Format: SQLite. One .db file per session under the storage path. Chosen so
post-run analysis/dashboard replay can query with normal SQL (filter by
time range, CAN id, etc.) instead of parsing text line-by-line.
"""
from __future__ import annotations

import json
import os
import re
import sqlite3
import threading
import time
import uuid
from dataclasses import dataclass
from datetime import datetime, timezone
from typing import Any, Optional


SESSION_FILENAME_RE = re.compile(
    r"^session_(?P<timestamp>\d{8}_\d{6})_(?P<suffix>[0-9a-f]{8})(?:_(?P<name>[A-Za-z0-9_-]+))?\.db$"
)


def _utcnow() -> datetime:
    return datetime.now(timezone.utc)


def make_session_filename(name: Optional[str] = None, when: Optional[datetime] = None) -> str:
    """Timestamp-based, collision-proof filename for a new session.

    e.g. session_20260720_153045_ab12cd34.db
    An optional human-readable `name` (e.g. "endurance_run") is appended
    for readability but does not affect uniqueness.
    """
    when = when or _utcnow()
    ts = when.strftime("%Y%m%d_%H%M%S")
    suffix = uuid.uuid4().hex[:8]
    base = f"session_{ts}_{suffix}"
    if name:
        safe_name = re.sub(r"[^A-Za-z0-9_-]", "_", name)
        base = f"{base}_{safe_name}"
    return f"{base}.db"


class SessionLogger:
    """Owns a single SQLite file for one session/run.

    Thread-safe for concurrent append() calls (e.g. multiple asyncio tasks
    or threads reading from the LoRa USB receiver serial link). Not process-safe --
    one process should own a given session file at a time, which matches
    a single pit-side backend process.
    """

    def __init__(self, path: str):
        self.path = path
        self._lock = threading.RLock()
        self._closed = False
        self._conn = sqlite3.connect(path, check_same_thread=False)
        self._conn.execute("PRAGMA journal_mode=WAL;")
        # NORMAL is durable enough with WAL (fsync on checkpoint/commit
        # boundaries) while avoiding a full fsync on every single insert.
        self._conn.execute("PRAGMA synchronous=NORMAL;")
        self._conn.execute(
            """
            CREATE TABLE IF NOT EXISTS telemetry (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                recv_ts REAL NOT NULL,
                device_ts REAL,
                can_id TEXT,
                seq INTEGER,
                rssi REAL,
                payload TEXT NOT NULL
            )
            """
        )
        self._conn.execute(
            "CREATE INDEX IF NOT EXISTS idx_telemetry_recv_ts ON telemetry(recv_ts)"
        )
        self._conn.commit()
        self._dirty = False

    def append(self, packet: dict[str, Any]) -> int:
        """Insert one telemetry packet. Returns the assigned row id.

        Not committed immediately -- call flush() (done periodically by
        the background task, and always on session stop) so a burst of
        high-frequency writes doesn't pay a disk-sync cost per packet.
        """
        payload = packet.get("payload", {})
        with self._lock:
            if self._closed:
                raise RuntimeError("append() called on a closed SessionLogger")
            cur = self._conn.execute(
                """
                INSERT INTO telemetry (recv_ts, device_ts, can_id, seq, rssi, payload)
                VALUES (?, ?, ?, ?, ?, ?)
                """,
                (
                    time.time(),
                    packet.get("device_ts"),
                    packet.get("can_id"),
                    packet.get("seq"),
                    packet.get("rssi"),
                    json.dumps(payload),
                ),
            )
            self._dirty = True
            return cur.lastrowid

    def flush(self) -> None:
        """Commit any pending writes to disk. Safe to call frequently;
        it's a no-op if nothing changed since the last flush."""
        with self._lock:
            if self._closed:
                return
            if self._dirty:
                self._conn.commit()
                self._dirty = False

    def read_all(self, limit: int = 1000, offset: int = 0) -> list[dict[str, Any]]:
        with self._lock:
            cur = self._conn.execute(
                """
                SELECT id, recv_ts, device_ts, can_id, seq, rssi, payload
                FROM telemetry
                ORDER BY id ASC
                LIMIT ? OFFSET ?
                """,
                (limit, offset),
            )
            rows = cur.fetchall()
        return [
            {
                "id": r[0],
                "recv_ts": r[1],
                "device_ts": r[2],
                "can_id": r[3],
                "seq": r[4],
                "rssi": r[5],
                "payload": json.loads(r[6]),
            }
            for r in rows
        ]

    def count(self) -> int:
        with self._lock:
            cur = self._conn.execute("SELECT COUNT(*) FROM telemetry")
            return cur.fetchone()[0]

    def close(self) -> None:
        with self._lock:
            if self._closed:
                return
            if self._dirty:
                self._conn.commit()
            self._conn.close()
            self._closed = True

    @property
    def closed(self) -> bool:
        return self._closed


@dataclass
class SessionInfo:
    session_id: str  # filename without extension, used as the public id
    filename: str
    path: str
    active: bool


class SessionManager:
    """Coordinates session (run) lifecycle over a configurable base path.

    base_path is the ONE place hardware-specific storage location matters --
    it's a plain filesystem path, defaulting to a local mock folder, and
    swappable for a real SD card mount point via config with no other code
    changes.
    """

    def __init__(self, base_path: str):
        self.base_path = base_path
        os.makedirs(self.base_path, exist_ok=True)
        self._lock = threading.RLock()
        self._active: Optional[SessionLogger] = None
        self._active_filename: Optional[str] = None

    # -- session lifecycle -------------------------------------------------

    def start_session(self, name: Optional[str] = None) -> SessionInfo:
        """Rotate to a brand new session file. If a session is already
        active, it is flushed and closed first (never clobbered -- each
        session gets its own uniquely timestamped filename)."""
        with self._lock:
            if self._active is not None:
                self._active.flush()
                self._active.close()

            filename = make_session_filename(name)
            full_path = os.path.join(self.base_path, filename)
            self._active = SessionLogger(full_path)
            self._active_filename = filename
            return SessionInfo(
                session_id=filename[:-3],  # strip .db
                filename=filename,
                path=full_path,
                active=True,
            )

    def stop_session(self) -> Optional[SessionInfo]:
        with self._lock:
            if self._active is None:
                return None
            self._active.flush()
            self._active.close()
            info = SessionInfo(
                session_id=self._active_filename[:-3],
                filename=self._active_filename,
                path=os.path.join(self.base_path, self._active_filename),
                active=False,
            )
            self._active = None
            self._active_filename = None
            return info

    def append_telemetry(self, packet: dict[str, Any]) -> int:
        with self._lock:
            if self._active is None:
                raise RuntimeError("no active session -- call /sessions/start first")
            logger = self._active
        return logger.append(packet)

    def flush_active(self) -> None:
        with self._lock:
            logger = self._active
        if logger is not None:
            logger.flush()

    @property
    def active_session_id(self) -> Optional[str]:
        with self._lock:
            if self._active_filename is None:
                return None
            return self._active_filename[:-3]

    # -- read-back / listing -----------------------------------------------

    def list_sessions(self) -> list[dict[str, Any]]:
        with self._lock:
            active_filename = self._active_filename
        results = []
        if not os.path.isdir(self.base_path):
            return results
        for filename in sorted(os.listdir(self.base_path)):
            if not SESSION_FILENAME_RE.match(filename):
                continue
            full_path = os.path.join(self.base_path, filename)
            try:
                size = os.path.getsize(full_path)
            except OSError:
                size = None
            results.append(
                {
                    "session_id": filename[:-3],
                    "filename": filename,
                    "size_bytes": size,
                    "active": filename == active_filename,
                }
            )
        return results

    def _resolve_path(self, session_id: str) -> str:
        # session_id is the filename without extension; guard against path
        # traversal since it may come from a URL path parameter.
        filename = f"{session_id}.db"
        if not SESSION_FILENAME_RE.match(filename):
            raise ValueError("invalid session id")
        full_path = os.path.join(self.base_path, filename)
        if not os.path.isfile(full_path):
            raise FileNotFoundError(session_id)
        return full_path

    def read_session(
        self, session_id: str, limit: int = 1000, offset: int = 0
    ) -> list[dict[str, Any]]:
        with self._lock:
            if session_id == self.active_session_id:
                self._active.flush()
                return self._active.read_all(limit=limit, offset=offset)

        full_path = self._resolve_path(session_id)
        # Read-only connection for a session that's already closed / not
        # the active one -- avoids interfering with an in-progress writer.
        conn = sqlite3.connect(full_path)
        try:
            cur = conn.execute(
                """
                SELECT id, recv_ts, device_ts, can_id, seq, rssi, payload
                FROM telemetry
                ORDER BY id ASC
                LIMIT ? OFFSET ?
                """,
                (limit, offset),
            )
            rows = cur.fetchall()
        finally:
            conn.close()
        return [
            {
                "id": r[0],
                "recv_ts": r[1],
                "device_ts": r[2],
                "can_id": r[3],
                "seq": r[4],
                "rssi": r[5],
                "payload": json.loads(r[6]),
            }
            for r in rows
        ]

    def session_row_count(self, session_id: str) -> int:
        with self._lock:
            if session_id == self.active_session_id:
                return self._active.count()
        full_path = self._resolve_path(session_id)
        conn = sqlite3.connect(full_path)
        try:
            cur = conn.execute("SELECT COUNT(*) FROM telemetry")
            return cur.fetchone()[0]
        finally:
            conn.close()

    def shutdown(self) -> None:
        with self._lock:
            if self._active is not None:
                self._active.flush()
                self._active.close()
                self._active = None
                self._active_filename = None
