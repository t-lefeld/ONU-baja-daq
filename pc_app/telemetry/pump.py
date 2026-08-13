"""
FramePump: the frame-consuming logic shared by every front end.

Pulls frames from a FrameSource forever, writes them to a CsvRecorder if one is
attached, tracks the bookkeeping every UI wants (frame count, last-seen time,
link error text), and builds the small "status" dict that tells a UI the
difference between "the hardware is quiet" and "the app has died" - a
frame-only stream can't express that distinction, which is why status is
tracked and exposed independently of frames arriving.

This is UI-agnostic on purpose: it does not know about aiohttp, WebSockets, Qt
signals, or anything else that consumes it. server.py (the browser dashboard)
and qt_app.py (the native window) both wrap the same FramePump and differ only
in how they push its output onward - broadcast over a WebSocket, or emit as a
Qt signal into a window running in another thread. One implementation to keep
correct instead of two that can drift apart from each other.
"""

from __future__ import annotations

import asyncio
import logging
import time
from dataclasses import dataclass
from typing import Any, Callable

from .protocols import active
from .proto import Frame
from .recorder import CsvRecorder
from .sources import FrameSource

log = logging.getLogger("telemetry")

STATUS_INTERVAL = 1.0

# What a UI receives per frame. A plain callable rather than an event/signal
# type of its own, so both an asyncio-based consumer (server.py) and a
# thread-bridged Qt consumer (qt_app.py) can supply whatever hook shape suits
# them - a coroutine, a Qt signal's .emit, a queue.put, anything callable.
FrameSink = Callable[[dict], Any]
StatusSink = Callable[[dict], Any]


@dataclass
class PumpStats:
    started: float
    frames_seen: int = 0
    last_frame_at: float | None = None
    pump_error: str | None = None


class FramePump:
    """
    Owns a FrameSource and a CsvRecorder, and knows how to describe its own
    health. Does not own an event loop or a UI - call run() from whatever
    async context the front end provides.
    """

    def __init__(self, source: FrameSource, recorder: CsvRecorder | None = None) -> None:
        self.source = source
        self.recorder = recorder
        self.stats = PumpStats(started=time.time())

    # ---------------- description ----------------

    def meta(self) -> dict[str, Any]:
        """Static description of the bus: nodes, channels, units. Sent once."""
        return {
            "type": "meta",
            "source": self.source.name,
            "proto_version": active().PROTO_VERSION,
            "node_count": active().NODE_COUNT,
            "frame_period_ms": active().FRAME_PERIOD_MS,
            "nodes": [
                {
                    "node_id": n,
                    "label": active().NODE_LABELS[n],
                    "channels": [
                        {"name": c.name, "unit": c.unit} for c in active().CHANNELS[n]
                    ],
                }
                for n in range(active().NODE_COUNT)
            ],
        }

    def status(self, extra: dict[str, Any] | None = None) -> dict[str, Any]:
        """Link health and counters. Sent on its own timer, independent of frames."""
        base = {
            "type": "status",
            "source": self.source.name,
            "link": self.source.link_state,
            "detail": self.source.detail,
            "uptime_s": round(time.time() - self.stats.started, 1),
            "frames": self.stats.frames_seen,
            "last_frame_age_s": (
                round(time.time() - self.stats.last_frame_at, 2)
                if self.stats.last_frame_at else None
            ),
            "csv": str(self.recorder.path) if self.recorder else None,
            "csv_rows": self.recorder.rows if self.recorder else 0,
            "error": self.stats.pump_error,
            **self.source.stats(),
        }
        if extra:
            base.update(extra)
        return base

    # ---------------- ingest ----------------

    async def run(self, on_frame: FrameSink) -> None:
        """
        Pull frames forever, record them, hand each one (as a dict) to
        on_frame. Runs until cancelled or the source raises something a
        source is not supposed to raise (sources handle their own recoverable
        failures - see sources.py - so reaching the except here means
        something genuinely unexpected happened).
        """
        try:
            async for frame in self.source.frames():
                self._record(frame)

                payload = frame.to_dict()
                payload["host_epoch"] = self.stats.last_frame_at

                result = on_frame(payload)
                if asyncio.iscoroutine(result):
                    await result

        except asyncio.CancelledError:
            raise
        except Exception as exc:  # noqa: BLE001
            log.exception("Frame source stopped")
            self.stats.pump_error = f"{type(exc).__name__}: {exc}"

    async def status_loop(self, on_status: StatusSink, interval: float = STATUS_INTERVAL) -> None:
        """Call on_status(self.status()) every `interval` seconds, forever."""
        while True:
            await asyncio.sleep(interval)
            result = on_status(self.status())
            if asyncio.iscoroutine(result):
                await result

    def _record(self, frame: Frame) -> None:
        self.stats.frames_seen += 1
        self.stats.last_frame_at = time.time()
        self.stats.pump_error = None

        if self.recorder:
            try:
                self.recorder.write(frame)
            except OSError as exc:
                # Disk full, or the file was deleted out from under us. Losing
                # the CSV must not take the live view down with it.
                #
                # Close the handle before dropping the reference. Just setting
                # self.recorder = None leaves the file open until the garbage
                # collector happens to run - on POSIX that is invisible, but on
                # Windows an open handle blocks anyone from deleting or
                # reopening that path, so a "disabled" recorder can still lock
                # the file it failed to write. Closing may itself raise (the
                # underlying error is often still there on flush), which is
                # fine to swallow: we are already on the failure path and the
                # only goal left is releasing the handle.
                log.error("CSV write failed, disabling recorder: %s", exc)
                try:
                    self.recorder.close()
                except OSError:
                    pass
                self.recorder = None

    async def close(self) -> None:
        await self.source.close()
        if self.recorder:
            self.recorder.close()
