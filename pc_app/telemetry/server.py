"""
aiohttp server: serves the browser dashboard and pushes frames over a
WebSocket. This is the --web / remote-viewing path - see run.py's docstring
for why the native PySide6 window (qt_app.py) is the default instead.

All the frame-consuming logic (recording, stats, status) lives in
telemetry/pump.py and is shared with the native window; this module is only
responsible for fanning FramePump's output out over HTTP and WebSocket.
"""

from __future__ import annotations

import asyncio
import json
import logging
import time

from aiohttp import WSMsgType, web

from pathlib import Path

from .paths import static_dir
from .protocols import active
from .pump import FramePump
from .recorder import CsvRecorder
from .sources import FrameSource

log = logging.getLogger("telemetry")

# The 4 alternate dashboard designs live in simulation/dashboards/ (built
# against the synthetic full-sensor-suite feed - see simulation/README.md).
# They only talk to the same meta/status/history/frame WebSocket contract as
# this server, via telemetry-core.js, so serving them here too means the
# identical HTML/CSS/JS renders real hardware data instead of the simulator's
# synthetic data - no dashboard code needs to change either way.
#
# Dev-mode only: this path assumes running from source (telemetry/server.py
# -> telemetry/ -> pc_app/ -> repo root -> simulation/dashboards). A
# PyInstaller build would need this added to build.bat's --add-data to work.
DASHBOARDS_DIR = Path(__file__).resolve().parent.parent.parent / "simulation" / "dashboards"

# Real SD captures only - see field_data/README.md. Ported from
# simulation/server.py's handle_logs(), which is the same endpoint against
# synthetic data; this is the real-hardware half, previously missing here
# entirely (the "Logs" tab 404'd against a live ground station - only the
# simulator ever had this route).
FIELD_DATA_DIR = Path(__file__).resolve().parent.parent.parent / "field_data"


class TelemetryServer:
    def __init__(
        self,
        source: FrameSource,
        recorder: CsvRecorder | None = None,
        history: int = 600,
    ) -> None:
        self.pump = FramePump(source, recorder)

        # Ring of recent frames, replayed to a browser the moment it connects
        # so a page refresh does not start you at an empty chart.
        # 600 frames at 2 Hz is five minutes.
        self.history: list[dict] = []
        self.history_max = history

        self.clients: set[web.WebSocketResponse] = set()

    # kept for backwards compatibility with anything reading these directly
    # (the test suite included) rather than going through pump.stats.
    @property
    def frames_seen(self) -> int:
        return self.pump.stats.frames_seen

    @property
    def source(self) -> FrameSource:
        return self.pump.source

    # ---------------- ingest ----------------

    async def _on_frame(self, payload: dict) -> None:
        self.history.append(payload)
        if len(self.history) > self.history_max:
            del self.history[: len(self.history) - self.history_max]

        await self._broadcast({"type": "frame", "frame": payload})

    async def _on_status(self, message: dict) -> None:
        message["clients"] = len(self.clients)
        await self._broadcast(message)

    async def _broadcast(self, message: dict) -> None:
        if not self.clients:
            return

        text = json.dumps(message)
        dead = []

        for ws in self.clients:
            try:
                await ws.send_str(text)
            except (ConnectionResetError, RuntimeError, asyncio.TimeoutError):
                dead.append(ws)

        for ws in dead:
            self.clients.discard(ws)

    # ---------------- HTTP ----------------

    async def handle_index(self, request: web.Request) -> web.StreamResponse:
        index = static_dir() / "index.html"
        if not index.exists():
            # Almost always a packaging mistake: static/ was not bundled.
            return web.Response(
                status=500,
                text=f"index.html not found at {index}\n\n"
                     "If this is a built .exe, static/ was not bundled - "
                     "check the --add-data line in build.bat.",
            )
        return web.FileResponse(index)

    async def handle_status(self, request: web.Request) -> web.Response:
        status = self.pump.status()
        status["clients"] = len(self.clients)
        return web.json_response(status)

    async def handle_logs(self, request: web.Request) -> web.Response:
        """
        Inventory of field_data/ for the log browser - real SD captures only,
        never synthetic data (see field_data/README.md). Ported unchanged
        from simulation/server.py's handle_logs(); frame size/period come
        from whichever protocol is active (v1 or v2) instead of always v2,
        since this server (unlike the simulator) can run either.

        Frame counts come from file size divided by the frame size, not by
        parsing - a .tlm is a flat run of fixed-size frames, so the division
        is exact. A non-multiple size means the file is truncated (power
        lost mid-write, most likely), reported as `partial` rather than
        hidden.
        """
        frame_size = active().FRAME_SIZE
        frame_period_ms = active().FRAME_PERIOD_MS

        entries = []
        if FIELD_DATA_DIR.is_dir():
            for path in sorted(FIELD_DATA_DIR.rglob("*")):
                if not path.is_file():
                    continue
                if path.suffix.lower() not in (".tlm", ".csv"):
                    continue

                stat = path.stat()
                item = {
                    "name": path.name,
                    "rel": str(path.relative_to(FIELD_DATA_DIR)).replace("\\", "/"),
                    "dir": str(path.parent.relative_to(FIELD_DATA_DIR)).replace("\\", "/"),
                    "bytes": stat.st_size,
                    "modified": time.strftime("%Y-%m-%d %H:%M", time.localtime(stat.st_mtime)),
                    "kind": path.suffix.lower().lstrip("."),
                    "frames": None,
                    "duration_s": None,
                    "partial": False,
                }

                if item["kind"] == "tlm" and frame_size:
                    item["frames"] = stat.st_size // frame_size
                    item["partial"] = (stat.st_size % frame_size) != 0
                    item["duration_s"] = round(item["frames"] * frame_period_ms / 1000.0, 1)

                entries.append(item)

        return web.json_response({
            "root": str(FIELD_DATA_DIR),
            "exists": FIELD_DATA_DIR.is_dir(),
            "frame_size": frame_size,
            "entries": entries,
        })

    async def handle_log_scrub(self, request: web.Request) -> web.Response:
        """
        Whole-file decode of one field_data/ log, for the dashboard's
        scrubber (drag a slider to any point in a recording) - a separate
        thing from --replay, which re-streams a file in real time to
        exercise the same code path a live link uses. The scrubber wants
        the opposite: everything at once, no pacing, so the browser can jump
        anywhere instantly.

        Decodes with whichever protocol is currently active on this server
        (v1 or v2) - same convention run.py's --replay already uses (you
        pick --proto to match the log), not auto-detected from the file.
        """
        rel = request.match_info["rel"]
        path = (FIELD_DATA_DIR / rel).resolve()

        # rel comes from the URL - refuse anything that resolves outside
        # field_data/ (e.g. "../../etc/passwd") rather than trusting a
        # client-supplied path straight into the filesystem.
        try:
            path.relative_to(FIELD_DATA_DIR.resolve())
        except ValueError:
            return web.json_response({"error": "path escapes field_data/"}, status=400)

        if not path.is_file():
            return web.json_response({"error": f"not found: {rel}"}, status=404)

        data = path.read_bytes()
        # Same reasoning as ReplaySource's fix (2026-08-16, see sources.py):
        # the whole file is already in memory, so the parser's own buffer
        # must be at least that big or feed() silently discards everything
        # but the tail before decoding a single frame.
        parser = active().StreamParser(max_buffer=len(data) + 4096)
        frames = [f.to_dict() for f in parser.feed(data)]

        meta = self.pump.meta()
        meta["source"] = f"scrub {path.name}"

        return web.json_response({
            "meta": meta,
            "frames": frames,
            "crc_errors": parser.crc_errors,
            "bytes_discarded": parser.bytes_discarded,
        })

    async def handle_ws(self, request: web.Request) -> web.WebSocketResponse:
        ws = web.WebSocketResponse(heartbeat=20)
        await ws.prepare(request)
        self.clients.add(ws)

        try:
            await ws.send_str(json.dumps(self.pump.meta()))

            status = self.pump.status()
            status["clients"] = len(self.clients)
            await ws.send_str(json.dumps(status))

            if self.history:
                await ws.send_str(json.dumps({"type": "history", "frames": self.history}))

            async for msg in ws:
                if msg.type == WSMsgType.ERROR:
                    break
                # No client-to-server protocol yet. When you add downlink
                # commands to the hub, this is where they arrive.
        finally:
            self.clients.discard(ws)

        return ws

    def build_app(self) -> web.Application:
        # Force revalidation on html/js/css so a stale/broken response (like
        # the 404 that used to come back for js/telemetry-core.js, fixed
        # 2026-08-16) doesn't keep getting served from the browser's cache
        # after the server-side fix lands - the standalone app window is
        # long-lived across `run.py` restarts, and without this a fix here
        # can look like it didn't work. Same fix simulation/server.py already
        # has - see the comment there for the full "mistakes already made"
        # writeup, this file just never got it.
        @web.middleware
        async def no_cache(request: web.Request, handler):
            resp = await handler(request)
            path = request.path.lower()
            if path.endswith((".html", ".js", ".css")) or path == "/":
                resp.headers["Cache-Control"] = "no-cache, must-revalidate"
                resp.headers["Pragma"] = "no-cache"
            return resp

        app = web.Application(middlewares=[no_cache])
        app.router.add_get("/", self.handle_index)
        app.router.add_get("/ws", self.handle_ws)
        app.router.add_get("/api/status", self.handle_status)
        app.router.add_get("/api/logs", self.handle_logs)
        app.router.add_get("/api/log/{rel:.*}", self.handle_log_scrub)

        static = static_dir()
        if static.is_dir():
            app.router.add_static("/static/", static)

        if DASHBOARDS_DIR.is_dir():
            app.router.add_static("/dashboards/", DASHBOARDS_DIR)
        else:
            log.info("simulation/dashboards not found at %s - skipping, only relevant in dev checkouts", DASHBOARDS_DIR)

        async def on_startup(_: web.Application) -> None:
            app["pump_task"] = asyncio.create_task(self.pump.run(self._on_frame))
            app["status_task"] = asyncio.create_task(self.pump.status_loop(self._on_status))

        async def on_cleanup(_: web.Application) -> None:
            for key in ("pump_task", "status_task"):
                task = app.get(key)
                if task:
                    task.cancel()
                    try:
                        await task
                    except asyncio.CancelledError:
                        pass
            await self.pump.close()

        app.on_startup.append(on_startup)
        app.on_cleanup.append(on_cleanup)
        return app
