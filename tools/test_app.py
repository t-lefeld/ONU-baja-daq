#!/usr/bin/env python3
"""
Ground station backend tests.

Covers the things that only bite once the app is a real .exe on someone else's
machine: path resolution when frozen, a settings file that has been hand-edited
into nonsense, the HTTP port already being taken, and the USB dongle being
unplugged mid-session.

    python tools/test_app.py

The serial tests use a fake pyserial that can be told to fail on demand, so the
reconnect path is exercised for real rather than reasoned about.
"""

from __future__ import annotations

import asyncio
import json
import socket
import sys
import tempfile
import time
import types
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "pc_app"))

failures: list[str] = []
checks = 0


def check(label: str, got, want) -> None:
    global checks
    checks += 1
    if got != want:
        failures.append(f"{label}\n     got  {got!r}\n     want {want!r}")


def ok(label: str, cond: bool) -> None:
    global checks
    checks += 1
    if not cond:
        failures.append(label)


# ====================================================================
# paths
# ====================================================================


def test_paths() -> None:
    print("-- paths: source vs frozen")
    from telemetry import paths

    ok("not frozen when run from source", not paths.is_frozen())
    check("static dir sits under pc_app", paths.static_dir().name, "static")
    ok("index.html is where the server will look", (paths.static_dir() / "index.html").exists())
    check("data dir is pc_app in dev", paths.data_dir().name, "pc_app")

    # Simulate a PyInstaller one-file build.
    with tempfile.TemporaryDirectory() as meipass, tempfile.TemporaryDirectory() as exedir:
        exe = Path(exedir) / "Telemetry.exe"
        exe.write_bytes(b"")

        old_frozen = getattr(sys, "frozen", None)
        old_mei = getattr(sys, "_MEIPASS", None)
        old_exe = sys.executable

        sys.frozen = True                     # type: ignore[attr-defined]
        sys._MEIPASS = meipass                # type: ignore[attr-defined]
        sys.executable = str(exe)

        try:
            ok("detects frozen", paths.is_frozen())
            check("resources come from _MEIPASS", paths.resource_dir(), Path(meipass))

            # The whole point of the split: writable data must NOT live in
            # _MEIPASS, which PyInstaller deletes when the process exits.
            check("data lives beside the exe", paths.data_dir(), Path(exedir))
            ok("data dir is not the temp bundle", paths.data_dir() != paths.resource_dir())
            ok("logs dir created beside the exe", paths.logs_dir().is_dir())
            check("logs are under data dir", paths.logs_dir().parent, Path(exedir))
        finally:
            if old_frozen is None:
                del sys.frozen                # type: ignore[attr-defined]
            else:
                sys.frozen = old_frozen       # type: ignore[attr-defined]
            if old_mei is None:
                del sys._MEIPASS              # type: ignore[attr-defined]
            else:
                sys._MEIPASS = old_mei        # type: ignore[attr-defined]
            sys.executable = old_exe

    ok("frozen flag cleaned up", not paths.is_frozen())


# ====================================================================
# settings
# ====================================================================


def test_settings() -> None:
    print("-- settings: round-trip and bad input")
    from telemetry import paths, settings

    with tempfile.TemporaryDirectory() as td:
        target = Path(td) / "settings.json"
        settings.settings_path = lambda: target          # type: ignore[assignment]
        paths.settings_path = lambda: target             # type: ignore[assignment]

        check("defaults when absent", settings.load()["baud"], 9600)
        check("port defaults to None", settings.load()["port"], None)

        ok("save works", settings.save({"port": "COM7", "baud": 19200}))
        check("port round-trips", settings.load()["port"], "COM7")
        check("baud round-trips", settings.load()["baud"], 19200)
        check("unset keys still default", settings.load()["http_port"], 8765)

        # A hand-edited file must not stop the app starting.
        target.write_text("{ this is not json", encoding="utf-8")
        check("corrupt file falls back to defaults", settings.load()["baud"], 9600)

        target.write_text('["a", "list"]', encoding="utf-8")
        check("wrong top-level type falls back", settings.load()["baud"], 9600)

        target.write_text(json.dumps({"baud": "fast", "http_port": 9000}), encoding="utf-8")
        loaded = settings.load()
        check("bad value type ignored", loaded["baud"], 9600)
        check("good value alongside it still read", loaded["http_port"], 9000)

        target.write_text(json.dumps({"nonsense": 1, "port": "COM3"}), encoding="utf-8")
        loaded = settings.load()
        check("unknown key ignored", "nonsense" in loaded, False)
        check("known key still read", loaded["port"], "COM3")

        settings.save({"port": "COM9", "baud": 9600})
        check("full key set always written",
              sorted(json.loads(target.read_text()).keys()),
              sorted(settings.DEFAULTS.keys()))

        # A partial save must not reset the keys it did not mention.
        settings.save({"http_port": 9100, "host": "0.0.0.0"})
        settings.save({"port": "COM11"})
        after = settings.load()
        check("partial save keeps other settings", after["http_port"], 9100)
        check("partial save keeps host", after["host"], "0.0.0.0")
        check("partial save applied its own change", after["port"], "COM11")


# ====================================================================
# port selection
# ====================================================================


def test_port_fallback() -> None:
    print("-- http port: fallback when busy")
    import run

    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as busy:
        busy.bind(("127.0.0.1", 0))
        busy.listen(1)
        taken = busy.getsockname()[1]

        ok("occupied port reported busy", not run.port_is_free("127.0.0.1", taken))

        chosen = run.find_free_port("127.0.0.1", taken)
        ok("a port was found", chosen is not None)
        ok("it is not the busy one", chosen != taken)
        ok("it is nearby", chosen is not None and taken < chosen <= taken + 20)

    # Once free, the preferred port is used again rather than drifting upward.
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        free = probe.getsockname()[1]
    check("free preferred port is chosen", run.find_free_port("127.0.0.1", free), free)


# ====================================================================
# serial reconnect
# ====================================================================


class FakeSerialPort:
    """Minimal pyserial stand-in whose reads can be made to fail on cue."""

    def __init__(self, port, baud, timeout=0.1):
        self.port = port
        self.baudrate = baud
        self.timeout = timeout
        self.is_open = True
        self.script: list = []          # bytes to hand out, or an Exception
        self._buf = b""

    def read(self, n=1):
        if not self._buf:
            if not self.script:
                # Real pyserial blocks for `timeout` when idle. Returning
                # instantly instead would spin the executor thread flat out.
                time.sleep(self.timeout)
                return b""
            item = self.script.pop(0)
            if isinstance(item, Exception):
                raise item
            self._buf = item

        out, self._buf = self._buf[:n], self._buf[n:]
        return out

    @property
    def in_waiting(self):
        return len(self._buf)

    def reset_input_buffer(self):
        self._buf = b""

    def close(self):
        self.is_open = False


def make_fake_serial(opened: list, fail_open_times: int = 0):
    state = {"fail": fail_open_times}

    def Serial(port, baud, timeout=0.1):  # noqa: N802 - mirrors pyserial's name
        if state["fail"] > 0:
            state["fail"] -= 1
            raise OSError("could not open port")
        p = FakeSerialPort(port, baud, timeout)
        opened.append(p)
        return p

    mod = types.SimpleNamespace()
    mod.Serial = Serial
    mod.SerialException = OSError
    mod.tools = types.SimpleNamespace(
        list_ports=types.SimpleNamespace(
            comports=lambda: [types.SimpleNamespace(device="COM_FAKE",
                                                    description="USB-SERIAL CH340")]
        )
    )
    return mod


def test_serial_reconnect() -> None:
    print("-- serial: unplug and reconnect")
    from telemetry import proto, sources
    from telemetry.proto import Frame, NodeRecord

    good = proto.encode_frame(
        Frame(1, 1000, [NodeRecord(n, proto.NF_ONLINE, 0, 0, (n, -n, 100 * n))
                        for n in range(proto.NODE_COUNT)])
    )

    opened: list[FakeSerialPort] = []
    real = sources.serial
    sources.serial = make_fake_serial(opened)               # type: ignore[assignment]
    sources.SerialSource.RETRY_DELAY = 0.01

    try:
        src = sources.SerialSource(port="COM_FAKE", baud=9600)
        check("starts out waiting", src.link_state, sources.LINK_WAITING)

        async def scenario():
            got = []
            agen = src.frames()

            # First port: one good frame, then the cable is yanked.
            async def pump(n):
                for _ in range(n):
                    got.append(await asyncio.wait_for(agen.__anext__(), 2))

            # Prime the first connection by asking for a frame.
            task = asyncio.create_task(pump(1))
            await asyncio.sleep(0.05)
            opened[0].script = [good]
            await asyncio.wait_for(task, 2)

            check("link reports live", src.link_state, sources.LINK_LIVE)
            check("first frame decoded", len(got), 1)

            # Unplug.
            opened[0].script = [OSError("device disconnected")]
            task = asyncio.create_task(pump(1))
            await asyncio.sleep(0.3)

            ok("disconnect noticed", src.disconnects >= 1)
            ok("reopened after the drop", len(opened) >= 2)

            # Plug back in: the newest port object gets the data.
            opened[-1].script = [good]
            await asyncio.wait_for(task, 3)

            check("recovered and decoded again", len(got), 2)
            check("link live again", src.link_state, sources.LINK_LIVE)
            ok("connect count went up", src.connects >= 2)

            await agen.aclose()

        asyncio.run(scenario())

        st = src.stats()
        ok("stats report the port", st["port"] == "COM_FAKE")
        ok("stats report byte count", st["bytes_read"] > 0)

    finally:
        sources.serial = real                                # type: ignore[assignment]


def test_serial_open_failure() -> None:
    print("-- serial: port present but unopenable")
    from telemetry import sources

    opened: list[FakeSerialPort] = []
    real = sources.serial
    sources.serial = make_fake_serial(opened, fail_open_times=99)  # type: ignore[assignment]
    sources.SerialSource.RETRY_DELAY = 0.01

    try:
        src = sources.SerialSource(port="COM_BUSY", baud=9600)

        async def scenario():
            agen = src.frames()
            task = asyncio.create_task(agen.__anext__())
            await asyncio.sleep(0.2)
            task.cancel()
            try:
                await task
            except (asyncio.CancelledError, StopAsyncIteration):
                pass
            await agen.aclose()

        asyncio.run(scenario())

        # A port that exists but refuses to open (driver missing, already in
        # use by another program) must be reported as an error, not silently
        # look like "no hardware yet".
        check("reported as error", src.link_state, sources.LINK_ERROR)
        ok("error detail is not empty", bool(src.detail))
        check("nothing was opened", len(opened), 0)

    finally:
        sources.serial = real                                # type: ignore[assignment]


def test_replay_large_file() -> None:
    print("-- replay: a log bigger than one buffer's worth replays in full")
    from telemetry import protocols, proto_v2, sources

    # Regression test for a real bug found 2026-08-16: ReplaySource read a
    # whole .tlm file into memory but handed it to a StreamParser with the
    # default 4096-byte max_buffer (sized for a LIVE link fed in small
    # chunks, not a replay fed the whole file at once). feed() discards
    # excess from the FRONT before parsing a single frame, so any recording
    # longer than ~4096 bytes silently played only its last few seconds -
    # exactly the kind of failure that looks like "it worked" (frames DO
    # arrive and decode cleanly) right up until you notice most of the
    # session is missing. 80 bytes/frame * 100 frames = 8000 bytes, twice
    # the old cap, so a regression here reliably drops the first ~48 frames.
    real_proto = protocols.active_name()
    protocols.use(protocols.V2)
    try:
        n_frames = 100
        chunks = []
        for seq in range(n_frames):
            nodes = [
                proto_v2.NodeRecord(
                    node_id, proto_v2.NF_ONLINE, seq & 0xFF, 0,
                    tuple(0 for _ in proto_v2.CHANNELS[node_id]),
                )
                for node_id in range(proto_v2.NODE_COUNT)
            ]
            frame = proto_v2.Frame(seq, seq * 500, nodes)
            chunks.append(proto_v2.encode_frame(frame))
        data = b"".join(chunks)
        check("test log is bigger than the old 4096-byte default", len(data) > 4096, True)

        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "LOG0001.TLM"
            path.write_bytes(data)

            src = sources.ReplaySource(path, speed=0, loop=False)

            async def scenario():
                return [f async for f in src.frames()]

            got = asyncio.run(scenario())

            check("every frame in the file was decoded, not just the tail",
                  len(got), n_frames)
            check("first frame is really the first one (seq 0), "
                  "not one truncation left it starting mid-file",
                  got[0].seq if got else None, 0)
            check("no bytes silently discarded", src.parser.bytes_discarded, 0)
    finally:
        protocols.use(real_proto)


# ====================================================================
# server
# ====================================================================


def test_server() -> None:
    print("-- server: status, websocket, missing assets")
    from aiohttp import ClientSession, web

    from telemetry import proto, sources
    from telemetry.server import TelemetryServer

    async def scenario():
        srv = TelemetryServer(sources.SimulatorSource())
        runner = web.AppRunner(srv.build_app())
        await runner.setup()
        site = web.TCPSite(runner, "127.0.0.1", 8793)
        await site.start()

        try:
            async with ClientSession() as s:
                r = await s.get("http://127.0.0.1:8793/")
                html = await r.text()
                check("index served", r.status, 200)
                # The WebSocket connect logic lives in the shared
                # telemetry-core.js module now, not inline in index.html -
                # check the page actually wires up to it. The WS checks below
                # (meta/status/history/frame arriving) are what prove the
                # connection really works; this just confirms the page loads
                # the module that makes those calls.
                ok("index is the dashboard",
                   "Telemetry" in html and "telemetry-core.js" in html)

                # The check above only proves the string is present in the
                # HTML, not that the path it names actually resolves against
                # how the server mounts static assets - a relative
                # "js/telemetry-core.js" versus the server's "/static/"
                # mount is exactly the kind of thing that renders fine as
                # text but 404s in a real browser (bit us 2026-08-14, see
                # HANDOFF.md). Extract every local <script src> / <link
                # href> from the served HTML and actually fetch each one.
                import re as _re
                local_assets = [
                    m for m in _re.findall(r'(?:src|href)="([^"]+)"', html)
                    if not m.startswith(("http://", "https://", "data:"))
                ]
                ok("index references at least one local asset (e.g. the JS bundle)",
                   len(local_assets) > 0)
                for asset in local_assets:
                    r = await s.get(f"http://127.0.0.1:8793{asset}")
                    check(f"local asset resolves: {asset}", r.status, 200)

                r = await s.get("http://127.0.0.1:8793/api/status")
                st = await r.json()
                check("status source", st["source"], "simulator")
                check("simulator has no link to report", st["link"], sources.LINK_NA)
                ok("status has no error", st["error"] is None)

                async with s.ws_connect("http://127.0.0.1:8793/ws") as ws:
                    first = json.loads((await ws.receive(timeout=3)).data)
                    check("meta arrives first", first["type"], "meta")
                    check("meta node count", first["node_count"], proto.NODE_COUNT)

                    second = json.loads((await ws.receive(timeout=3)).data)
                    check("status arrives without waiting for a frame",
                          second["type"], "status")

                    # A status message every second regardless of frames is what
                    # lets the UI say "quiet" rather than "dead".
                    kinds = set()
                    for _ in range(4):
                        m = json.loads((await ws.receive(timeout=4)).data)
                        kinds.add(m["type"])
                    ok("frames flow", "frame" in kinds)
        finally:
            await runner.cleanup()

    asyncio.run(scenario())


def test_server_missing_static() -> None:
    print("-- server: static/ not bundled")
    from aiohttp import ClientSession, web

    from telemetry import paths, server, sources
    from telemetry.server import TelemetryServer

    real = server.static_dir

    async def scenario():
        with tempfile.TemporaryDirectory() as td:
            server.static_dir = lambda: Path(td) / "nope"   # type: ignore[assignment]

            srv = TelemetryServer(sources.SimulatorSource())
            runner = web.AppRunner(srv.build_app())
            await runner.setup()
            site = web.TCPSite(runner, "127.0.0.1", 8794)
            await site.start()

            try:
                async with ClientSession() as s:
                    r = await s.get("http://127.0.0.1:8794/")
                    body = await r.text()
                    check("reports a server error", r.status, 500)
                    # The message has to name the path and the likely cause,
                    # because this only ever happens in a built exe where
                    # there is no traceback to read.
                    ok("names the missing path", "nope" in body)
                    ok("points at the build step", "build.bat" in body)
            finally:
                await runner.cleanup()

    try:
        asyncio.run(scenario())
    finally:
        server.static_dir = real                             # type: ignore[assignment]
        _ = paths


def test_server_log_scrub() -> None:
    print("-- server: log scrubber endpoint (/api/log/<rel>)")
    from aiohttp import ClientSession, web

    from telemetry import protocols, proto_v2, server, sources
    from telemetry.server import TelemetryServer

    real_field_data = server.FIELD_DATA_DIR
    real_proto = protocols.active_name()

    async def scenario(field_data_dir: Path):
        # SimulatorSource only speaks v1 (see sources.py) - build it while
        # v1 is still active, then switch to v2 afterward. meta()/the scrub
        # endpoint read the active protocol at call time, not at server
        # construction time, so this ordering is fine and doesn't need a
        # working v2 live source at all (the scrub endpoint never touches
        # `srv.pump.source`, only the file it's asked to decode).
        srv = TelemetryServer(sources.SimulatorSource())
        protocols.use(protocols.V2)
        runner = web.AppRunner(srv.build_app())
        await runner.setup()
        site = web.TCPSite(runner, "127.0.0.1", 8795)
        await site.start()

        try:
            async with ClientSession() as s:
                # Not found: no such log.
                r = await s.get("http://127.0.0.1:8795/api/log/nope.tlm")
                check("missing log reports 404", r.status, 404)

                # Path traversal: refused, not resolved against the real filesystem.
                r = await s.get("http://127.0.0.1:8795/api/log/../../../../etc/passwd")
                ok("path traversal rejected, not 200",
                   r.status in (400, 404))

                # A real log, bigger than one old-default parser buffer -
                # same regression this endpoint would otherwise inherit from
                # ReplaySource's 2026-08-16 bug (see test_replay_large_file).
                n_frames = 60
                chunks = []
                for seq in range(n_frames):
                    nodes = [
                        proto_v2.NodeRecord(
                            node_id, proto_v2.NF_ONLINE, seq & 0xFF, 0,
                            tuple(0 for _ in proto_v2.CHANNELS[node_id]),
                        )
                        for node_id in range(proto_v2.NODE_COUNT)
                    ]
                    chunks.append(proto_v2.encode_frame(proto_v2.Frame(seq, seq * 500, nodes)))
                data = b"".join(chunks)
                check("test log exceeds the old 4096-byte default", len(data) > 4096, True)

                sub = field_data_dir / "2026-08-16_scrub_test"
                sub.mkdir()
                (sub / "LOG0001.TLM").write_bytes(data)

                r = await s.get("http://127.0.0.1:8795/api/log/2026-08-16_scrub_test/LOG0001.TLM")
                check("scrub request succeeds", r.status, 200)
                body = await r.json()
                check("every frame decoded, none dropped to the old buffer cap",
                      len(body["frames"]), n_frames)
                check("no bytes discarded", body["bytes_discarded"], 0)
                check("first frame really is seq 0", body["frames"][0]["seq"], 0)
                check("last frame really is seq n-1", body["frames"][-1]["seq"], n_frames - 1)
                ok("meta describes the v2 channel set",
                   body["meta"]["node_count"] == proto_v2.NODE_COUNT)
        finally:
            await runner.cleanup()

    try:
        with tempfile.TemporaryDirectory() as td:
            field_data_dir = Path(td)
            server.FIELD_DATA_DIR = field_data_dir           # type: ignore[assignment]
            asyncio.run(scenario(field_data_dir))
    finally:
        server.FIELD_DATA_DIR = real_field_data              # type: ignore[assignment]
        protocols.use(real_proto)


# ====================================================================


def main() -> int:
    print("Ground station backend\n")

    for fn in (
        test_paths,
        test_settings,
        test_port_fallback,
        test_serial_reconnect,
        test_serial_open_failure,
        test_replay_large_file,
        test_server,
        test_server_missing_static,
        test_server_log_scrub,
    ):
        try:
            fn()
        except Exception as exc:  # noqa: BLE001
            import traceback
            failures.append(f"{fn.__name__} raised: {exc}\n{traceback.format_exc()}")

    print(f"\n  {checks - len(failures)}/{checks} checks passed")

    if failures:
        print(f"\n  {len(failures)} FAILURE(S):\n")
        for f in failures:
            print(f"   - {f}\n")
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
