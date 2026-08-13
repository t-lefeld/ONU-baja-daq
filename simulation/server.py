"""
server.py - Standalone aiohttp server speaking the exact same WebSocket
contract as pc_app/telemetry/server.py (meta/status/history/frame), but
backed by the full-sensor-suite simulator in vehicle_data.py instead of a
real serial/CAN source.

Because the themed dashboards under pc_app/static/themes only ever talk to
that contract (see pc_app/static/js/telemetry-core.js) and build their UI
entirely from the meta message's node/channel list, they work here completely
unmodified - point one at this server instead of the real ground station and
it renders the full vehicle sensor set instead of the 3-node placeholder.

Every frame is round-tripped through the real v2 wire codec before it goes
out over the WebSocket:

    vehicle_data.py values -> encode_frame() -> raw bytes -> decode_frame()
                                                            -> JSON -> browser

instead of handing vehicle_data.py's dicts to the browser directly. That
turns this simulator into an actual end-to-end exercise of
pc_app/telemetry/proto_v2.py's int16/int32 quantization and CRC framing -
the same path real hardware will use - rather than a UI that has only ever
seen numbers that never touched the wire format. See
simulation/quantization_report.py for a measurement of how much (if any)
error that quantization visibly introduces.

Only pc_app/telemetry/proto_v2.py is imported, and only for its pure encode/
decode functions - read-only, nothing in pc_app/ is modified. This mirrors
how PC_APP_STATIC below already reaches into pc_app/static/ for the themed
HTML/JS/CSS. If pc_app/ is missing or the import fails for any reason, this
degrades to sending vehicle_data.py's dicts straight over the wire (the
original behavior) with a clear warning logged once at startup, rather than
crashing the simulator.
"""

from __future__ import annotations

import asyncio
import json
import logging
import sys
import time
from pathlib import Path

from aiohttp import WSMsgType, web

import vehicle_data as vd

log = logging.getLogger("vehicle_sim")

PC_APP_ROOT = Path(__file__).resolve().parent.parent / "pc_app"
PC_APP_STATIC = PC_APP_ROOT / "static"
DASHBOARDS_DIR = Path(__file__).resolve().parent / "dashboards"
FIELD_DATA_DIR = Path(__file__).resolve().parent.parent / "field_data"
HISTORY_MAX = 240  # ~2 minutes at 2 Hz, matching the real dashboard's client-side buffer

# ---------------------------------------------------------------------
# Reach into pc_app/telemetry/proto_v2.py for the real wire codec. Degrade
# to the old direct-JSON path (with a loud warning) rather than crash if
# pc_app/ isn't there - e.g. a checkout of simulation/ on its own.
# ---------------------------------------------------------------------
PROTO_V2 = None
try:
    if PC_APP_ROOT.is_dir():
        if str(PC_APP_ROOT) not in sys.path:
            sys.path.insert(0, str(PC_APP_ROOT))
        from telemetry import proto_v2 as PROTO_V2  # type: ignore[no-redef]
    else:
        log.warning(
            "pc_app/ not found at %s - simulator will send vehicle_data.py's "
            "dicts directly instead of round-tripping through the real v2 "
            "wire codec (proto_v2.py). Frames will NOT exercise the actual "
            "encode/decode/quantization path.",
            PC_APP_ROOT,
        )
except Exception:  # noqa: BLE001 - any import failure must not crash the sim
    log.warning(
        "failed to import pc_app/telemetry/proto_v2 - falling back to "
        "vehicle_data.py's dicts sent directly. Frames will NOT exercise "
        "the real v2 wire codec.",
        exc_info=True,
    )
    PROTO_V2 = None

WIRE_BYTES_PER_FRAME = PROTO_V2.FRAME_SIZE if PROTO_V2 is not None else None


def _roundtrip_via_v2(raw_frame: dict) -> dict:
    """
    Take one vehicle_data.sample_frame() dict, encode it to real v2 wire
    bytes, decode those bytes right back, and return the decoded frame as a
    dict in the same shape the dashboards already expect.

    vehicle_data.NODES and proto_v2.CHANNELS are index-for-index identical in
    name/unit/order (tools/test_channel_sync.py enforces this), so each
    node's channel list can be zipped straight against its ChannelDef list
    with no name lookup.
    """
    nodes = []
    for node in raw_frame["nodes"]:
        node_id = node["node_id"]
        defs = PROTO_V2.CHANNELS[node_id]
        raw_ints = tuple(
            d.to_raw(ch["value"]) for d, ch in zip(defs, node["channels"])
        )
        nodes.append(
            PROTO_V2.NodeRecord(
                node_id=node_id,
                flags=PROTO_V2.NF_ONLINE,
                epoch=raw_frame["seq"] & 0xFF,
                loss=node.get("loss", 0),
                raw=raw_ints,
            )
        )

    frame_obj = PROTO_V2.Frame(seq=raw_frame["seq"], t_ms=raw_frame["t_ms"], nodes=nodes)
    encoded = PROTO_V2.encode_frame(frame_obj)
    decoded = PROTO_V2.decode_frame(encoded)
    return decoded.to_dict()


class VehicleSimServer:
    def __init__(self) -> None:
        self.started = time.time()
        self.seq = 0
        self.history: list[dict] = []
        self.clients: set[web.WebSocketResponse] = set()

    def _next_frame(self) -> dict:
        elapsed = time.time() - self.started
        raw_frame = vd.sample_frame(self.seq, elapsed)
        self.seq += 1
        if PROTO_V2 is None:
            return raw_frame
        return _roundtrip_via_v2(raw_frame)

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

    async def _pump(self) -> None:
        period_s = vd.FRAME_PERIOD_MS / 1000.0
        while True:
            frame = self._next_frame()
            self.history.append(frame)
            if len(self.history) > HISTORY_MAX:
                del self.history[: len(self.history) - HISTORY_MAX]
            await self._broadcast({"type": "frame", "frame": frame})
            await asyncio.sleep(period_s)

    async def _status_loop(self) -> None:
        while True:
            await asyncio.sleep(1.0)
            await self._broadcast(self._status())

    def _status(self) -> dict:
        if PROTO_V2 is not None:
            detail = (
                f"synthetic full-sensor-suite feed, round-tripped through the "
                f"real v2 wire codec ({WIRE_BYTES_PER_FRAME} B/frame encode+decode)"
            )
        else:
            detail = "synthetic full-sensor-suite feed (direct JSON - v2 codec unavailable, see startup log)"
        return {
            "type": "status",
            "source": "vehicle-sim",
            "link": "ok",
            "detail": detail,
            "uptime_s": round(time.time() - self.started, 1),
            "frames": self.seq,
            "last_frame_age_s": 0.0,
            "clients": len(self.clients),
            "csv": None,
            "csv_rows": 0,
            "error": None,
            # Visible proof this is the real wire format, not raw floats -
            # a dashboard header showing "80 B/frame" only makes sense if a
            # frame actually got serialized to that many bytes and back.
            "frame_bytes": WIRE_BYTES_PER_FRAME,
            "wire_format": "v2" if PROTO_V2 is not None else "raw-json",
        }

    def _meta(self) -> dict:
        meta = vd.meta()
        # Sent once, immediately on connect - before the first status message
        # a second later - so the "this is real wire bytes" marker is on
        # screen from the first paint, not just after a 1s delay.
        meta["frame_bytes"] = WIRE_BYTES_PER_FRAME
        meta["wire_format"] = "v2" if PROTO_V2 is not None else "raw-json"
        if PROTO_V2 is not None:
            meta["proto_version"] = PROTO_V2.PROTO_VERSION
        return meta

    # ---------------- HTTP ----------------

    async def handle_index(self, request: web.Request) -> web.StreamResponse:
        # Landing page is the 4-design judging gallery, not the old pc_app
        # gallery - the fresh designs live in simulation/dashboards/.
        index = DASHBOARDS_DIR / "index.html"
        if not index.exists():
            return web.Response(status=500, text=f"index.html not found at {index}")
        return web.FileResponse(index)

    async def handle_status(self, request: web.Request) -> web.Response:
        return web.json_response(self._status())

    async def handle_logs(self, request: web.Request) -> web.Response:
        """
        Inventory of field_data/ for the log browser.

        Frame counts come from file size divided by the frame size, not by
        parsing - a .tlm is a flat run of fixed-size frames, so the division is
        exact, and a 40 MB session log should not have to be read end to end
        just to populate a list. A non-multiple size means the file is
        truncated (power lost mid-write, most likely), which is worth showing
        rather than hiding, so it is reported as `partial`.
        """
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

                if item["kind"] == "tlm" and WIRE_BYTES_PER_FRAME:
                    fs = WIRE_BYTES_PER_FRAME
                    item["frames"] = stat.st_size // fs
                    item["partial"] = (stat.st_size % fs) != 0
                    item["duration_s"] = round(
                        item["frames"] * vd.FRAME_PERIOD_MS / 1000.0, 1
                    )

                entries.append(item)

        return web.json_response({
            "root": str(FIELD_DATA_DIR),
            "exists": FIELD_DATA_DIR.is_dir(),
            "frame_size": WIRE_BYTES_PER_FRAME,
            "entries": entries,
        })

    async def handle_ws(self, request: web.Request) -> web.WebSocketResponse:
        ws = web.WebSocketResponse(heartbeat=20)
        await ws.prepare(request)
        self.clients.add(ws)
        try:
            await ws.send_str(json.dumps(self._meta()))
            await ws.send_str(json.dumps(self._status()))
            if self.history:
                await ws.send_str(json.dumps({"type": "history", "frames": self.history}))
            async for msg in ws:
                if msg.type == WSMsgType.ERROR:
                    break
        finally:
            self.clients.discard(ws)
        return ws

    def build_app(self) -> web.Application:
        # Browsers cache HTML aggressively and aiohttp's static handler only
        # sends Last-Modified, which Chrome/Edge are happy to skip revalidating
        # for a while. During development that means editing a dashboard and
        # reloading shows the OLD page with no indication anything is stale -
        # you end up debugging code that isn't the code running. Force
        # revalidation on every request for the things that change.
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

        if PC_APP_STATIC.is_dir():
            app.router.add_static("/static/", PC_APP_STATIC)
        else:
            log.warning("pc_app static dir not found at %s - dashboards will 500", PC_APP_STATIC)

        if DASHBOARDS_DIR.is_dir():
            app.router.add_static("/dashboards/", DASHBOARDS_DIR)
        else:
            log.warning("dashboards dir not found at %s", DASHBOARDS_DIR)

        async def on_startup(_: web.Application) -> None:
            app["pump_task"] = asyncio.create_task(self._pump())
            app["status_task"] = asyncio.create_task(self._status_loop())

        async def on_cleanup(_: web.Application) -> None:
            for key in ("pump_task", "status_task"):
                task = app.get(key)
                if task:
                    task.cancel()
                    try:
                        await task
                    except asyncio.CancelledError:
                        pass

        app.on_startup.append(on_startup)
        app.on_cleanup.append(on_cleanup)
        return app
