#!/usr/bin/env python3
"""
Ground station entry point.

    python run.py                           # dashboards: find the dongle, else simulate
    python run.py --sim                     # force the simulator
    python run.py --list-ports              # see what is plugged in
    python run.py --port COM7               # a specific port
    python run.py --replay logs/LOG0001.TLM # replay an SD card log
    python run.py --proto v2 --replay x.tlm # decode the 24-channel v2 format
    python run.py --host 0.0.0.0            # let phones on the same wifi watch

Built as an .exe this is the double-click target, so running with no arguments
has to do something sensible on its own: detect the dongle, fall back to the
simulator if there isn't one, and open a window.

The UI is the browser dashboard set, opened as a standalone Edge/Chrome
"app mode" window (its own taskbar entry, not a tab) unless --tab says
otherwise. Four designs live in simulation/dashboards/ and are served at
/dashboards/; the original single view is still at /.

The native PySide6 window that used to be the default is retired - it only
ever spoke v1's 3-node channel set and pulled in a heavy GUI dependency for
one window. It is still reachable via --native if you need it, but nothing
tests it and it does not understand --proto v2.
"""

from __future__ import annotations

import argparse
import logging
import socket
import sys
import threading
import webbrowser
from datetime import datetime
from pathlib import Path

from telemetry import paths, protocols, settings
from telemetry.protocols import active
from telemetry.recorder import CsvRecorder
from telemetry.sources import (
    FrameSource,
    ReplaySource,
    SerialSource,
    SimulatorSource,
    guess_port,
    list_ports,
)

log = logging.getLogger("telemetry")


# --------------------------------------------------------------------
# Logging
# --------------------------------------------------------------------


def setup_logging(verbose: bool) -> Path | None:
    """Console always; a file too when frozen, since there may be no console."""
    level = logging.DEBUG if verbose else logging.INFO
    fmt = logging.Formatter("%(asctime)s %(levelname)-7s %(message)s", "%H:%M:%S")

    root = logging.getLogger()
    root.setLevel(level)

    console = logging.StreamHandler()
    console.setFormatter(fmt)
    root.addHandler(console)

    if not paths.is_frozen():
        return None

    try:
        log_path = paths.logs_dir() / "telemetry.log"
        handler = logging.FileHandler(log_path, encoding="utf-8")
        handler.setFormatter(
            logging.Formatter("%(asctime)s %(levelname)-7s %(message)s")
        )
        root.addHandler(handler)
        return log_path
    except OSError as exc:
        log.warning("Could not open log file (%s)", exc)
        return None


# --------------------------------------------------------------------
# Networking
# --------------------------------------------------------------------


def port_is_free(host: str, port: int) -> bool:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            s.bind((host, port))
            return True
        except OSError:
            return False


def find_free_port(host: str, preferred: int, tries: int = 20) -> int | None:
    """
    Preferred port, then the next few.

    A second copy of the app, or anything else on 8765, would otherwise make
    the exe exit immediately with a stack trace - and a double-clicked window
    closes before you can read it.
    """
    bind_host = "0.0.0.0" if host == "0.0.0.0" else host

    for offset in range(tries):
        candidate = preferred + offset
        if candidate > 65535:
            break
        if port_is_free(bind_host, candidate):
            return candidate

    return None


# --------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="CAN -> LoRa telemetry ground station",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )

    src = p.add_argument_group("source (default: auto-detect, else simulate)")
    src.add_argument("--port", help="serial port, e.g. COM7 or /dev/ttyUSB0")
    src.add_argument("--sim", action="store_true", help="force the built-in simulator")
    src.add_argument("--replay", type=Path, help="replay a .tlm log file")
    src.add_argument("--list-ports", action="store_true", help="list serial ports and exit")
    src.add_argument(
        "--no-reconnect",
        action="store_true",
        help="exit instead of retrying when the serial link drops",
    )

    ser = p.add_argument_group("serial options")
    ser.add_argument(
        "--baud",
        type=int,
        help="UART rate between PC and E22 dongle (default 9600, the module "
        "factory default). This is not the over-the-air rate.",
    )

    sim = p.add_argument_group("simulator options")
    sim.add_argument("--drop-rate", type=float, default=0.0,
                     help="fraction of CAN frames to pretend were lost, 0.0-1.0")
    sim.add_argument("--offline-node", type=int,
                     help="mark this node offline, to exercise the stale indicator")

    rep = p.add_argument_group("replay options")
    rep.add_argument("--speed", type=float, default=1.0, help="replay speed multiplier")
    rep.add_argument("--loop", action="store_true", help="restart at end of file")

    out = p.add_argument_group("recording")
    out.add_argument("--csv", nargs="?", const="auto",
                     help="write a CSV of engineering values; omit the path for a "
                          "timestamped name")
    out.add_argument("--raw", nargs="?", const="auto",
                     help="also capture the raw byte stream (serial source only)")

    w = p.add_argument_group("web dashboard (--web)")
    w.add_argument(
        "--web",
        action="store_true",
        help="deprecated no-op: the dashboards are the default UI now. "
        "Accepted so existing scripts and shortcuts keep working.",
    )
    w.add_argument(
        "--native",
        action="store_true",
        help="open the RETIRED PySide6 window instead (v1 channels only, "
        "needs PySide6, not covered by tests)",
    )
    w.add_argument("--http-port", type=int, help="default 8765, falls forward if busy")
    w.add_argument("--host", help="bind address; 0.0.0.0 to view from a phone")
    w.add_argument("--no-browser", action="store_true",
                   help="with --web: do not open anything, just serve")
    w.add_argument(
        "--tab",
        action="store_true",
        help="with --web: open in a normal browser tab instead of a standalone app window",
    )
    pr = p.add_argument_group("wire protocol")
    pr.add_argument(
        "--proto",
        choices=protocols.CHOICES,
        default=protocols.V1,
        help="wire format to decode (default v1 - what the hardware speaks "
        "today). v2 is the 24-channel multi-frame format; no firmware emits "
        "it yet, so use it with --replay or once a v2 node is flashed. The "
        "built-in --sim is v1-only; for a v2 synthetic feed run "
        "simulation/run_sim.py instead.",
    )

    p.add_argument("-v", "--verbose", action="store_true")

    return p


def timestamped(prefix: str, suffix: str) -> Path:
    return paths.logs_dir() / f"{prefix}_{datetime.now():%Y%m%d_%H%M%S}{suffix}"


def make_source(args, cfg) -> tuple[FrameSource | None, Path | None]:
    """Returns (source, raw_log_path). source is None if we cannot start."""
    raw_path = None
    if args.raw:
        raw_path = timestamped("raw", ".tlm") if args.raw == "auto" else Path(args.raw)

    if args.replay:
        try:
            return ReplaySource(args.replay, speed=args.speed, loop=args.loop), None
        except FileNotFoundError:
            log.error("Replay file not found: %s", args.replay)
            return None, None

    if args.sim:
        try:
            return SimulatorSource(drop_rate=args.drop_rate,
                                   offline_node=args.offline_node), None
        except RuntimeError as exc:
            # --proto v2 --sim: the built-in simulator is v1-shaped. Its own
            # message names the alternative, so print that rather than a
            # traceback, which reads like a crash for what is a usage error.
            for line in str(exc).splitlines():
                log.error("%s", line)
            return None, None

    baud = args.baud or cfg["baud"]
    port = args.port or cfg.get("port")

    # An explicitly requested port is pinned; otherwise the source re-detects
    # on every reconnect, so plugging the dongle into a different USB socket
    # mid-session just works.
    if port is None and guess_port() is None:
        log.warning("No serial port detected - starting the simulator instead.")
        log.warning("Plug in the E22 dongle and restart, or use --port to name one.")
        return SimulatorSource(), None

    try:
        source = SerialSource(
            port=port,
            baud=baud,
            raw_log=raw_path,
            reconnect=not args.no_reconnect,
        )
    except RuntimeError as exc:
        log.error("%s", exc)
        return None, None

    settings.remember_port(port, baud)
    return source, raw_path


def run_native(source: FrameSource, recorder: CsvRecorder | None) -> int:
    """
    RETIRED. The PySide6 window was the original UI and only ever spoke v1's
    3-node channel set; the browser dashboards replaced it and are what the
    project is built around now. Kept reachable behind --native purely so an
    existing install does not break, and so the code is one flag away if it
    turns out to be missed.

    Not covered by the test suite any more (the headless Qt test could not run
    in CI and was masking its own failures), and not wired to --proto v2.
    """
    try:
        from telemetry.qt_app import run_qt_app
    except ImportError as exc:
        log.error("PySide6 is not installed (%s).", exc)
        log.error("The native window is retired anyway - just run without --native")
        log.error("to get the browser dashboards, which need no PySide6 at all.")
        return 1

    log.warning("--native opens the RETIRED PySide6 window (v1 channels only).")
    log.warning("The browser dashboards are the supported UI; drop --native for them.")
    log.info("Source     : %s", source.name)
    log.info("Frame size : %d bytes every %d ms",
             active().FRAME_SIZE, active().FRAME_PERIOD_MS)

    return run_qt_app(source, recorder)


def run_web(args, source: FrameSource, recorder: CsvRecorder | None, cfg: dict) -> int:
    """The dashboards. Default UI - no PySide6, no native window involved."""
    from aiohttp import web

    from telemetry.launcher import find_app_browser, open_app_window
    from telemetry.server import TelemetryServer

    host = args.host or cfg["host"]
    preferred = args.http_port or cfg["http_port"]

    http_port = find_free_port(host, preferred)
    if http_port is None:
        log.error("No free TCP port near %d. Is another copy already running?", preferred)
        return 1
    if http_port != preferred:
        log.warning("Port %d busy, using %d instead", preferred, http_port)

    server = TelemetryServer(source, recorder)
    app = server.build_app()

    shown_host = "localhost" if host in ("127.0.0.1", "0.0.0.0") else host
    url = f"http://{shown_host}:{http_port}/"

    log.info("Source     : %s", source.name)
    log.info("Frame size : %d bytes every %d ms", active().FRAME_SIZE, active().FRAME_PERIOD_MS)
    log.info("Dashboard  : %s", url)
    if host == "0.0.0.0":
        log.info("Bound to all interfaces - reachable from other devices on this network.")

    open_browser = not args.no_browser and cfg["open_browser"]
    if open_browser:
        if args.tab:
            log.info("Opening in your default browser tab (--tab)")
            threading.Timer(0.8, lambda: webbrowser.open(url)).start()
        else:
            # Logged up front, before the window actually opens on the timer,
            # so the console tells you what to expect even if the window takes
            # a moment or a popup blocker gets in the way.
            found = find_app_browser()
            if found:
                log.info("Opening as a standalone window (%s) - "
                        "not a browser tab, its own taskbar entry", found[1])
            else:
                log.info("No Edge or Chrome found - opening a browser tab instead")

            profile_dir = paths.data_dir() / ".browser-profile"
            threading.Timer(0.8, lambda: open_app_window(url, profile_dir)).start()

    try:
        web.run_app(app, host=host, port=http_port, print=None)
    except KeyboardInterrupt:
        pass

    log.info("Stopped after %d frames", server.frames_seen)
    return 0


def main() -> int:
    args = build_parser().parse_args()
    log_file = setup_logging(args.verbose)

    if args.list_ports:
        ports = list_ports()
        if not ports:
            print("No serial ports found.")
            print("If the E22 dongle is plugged in, you may need the CH340 driver.")
            return 1
        print(f"{'PORT':<14} DESCRIPTION")
        for device, desc in ports:
            print(f"{device:<14} {desc}")
        return 0

    protocols.use(args.proto)
    if args.proto != protocols.V1:
        log.info("Wire format: %s (%d-byte frames, %d nodes)",
                 args.proto, active().FRAME_SIZE, active().NODE_COUNT)

    cfg = settings.load()
    log.debug("Paths: %s", paths.describe())
    if log_file:
        log.info("Logging to %s", log_file)

    source, raw_path = make_source(args, cfg)
    if source is None:
        return 1

    recorder = None
    want_csv = args.csv or (cfg["csv"] and not args.replay)
    if want_csv:
        csv_path = (timestamped("telemetry", ".csv")
                    if args.csv in (None, "auto") else Path(args.csv))
        try:
            recorder = CsvRecorder(csv_path)
            log.info("Recording CSV to %s", csv_path)
        except OSError as exc:
            log.error("Could not open CSV %s (%s); continuing without it", csv_path, exc)

    if raw_path:
        log.info("Recording raw stream to %s", raw_path)

    if args.native:
        return run_native(source, recorder)
    return run_web(args, source, recorder, cfg)


if __name__ == "__main__":
    try:
        code = main()
    except Exception:  # noqa: BLE001
        logging.getLogger("telemetry").exception("Unhandled error")
        code = 1

    # A double-clicked .exe closes its console the instant the process exits,
    # taking the error message with it. Hold the window open so there is
    # something to read.
    if code != 0 and paths.is_frozen():
        try:
            input("\nPress Enter to close...")
        except (EOFError, KeyboardInterrupt):
            pass

    sys.exit(code)
