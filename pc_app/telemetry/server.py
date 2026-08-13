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

from aiohttp import WSMsgType, web

from pathlib import Path

from .paths import static_dir
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
        app = web.Application()
        app.router.add_get("/", self.handle_index)
        app.router.add_get("/ws", self.handle_ws)
        app.router.add_get("/api/status", self.handle_status)

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
