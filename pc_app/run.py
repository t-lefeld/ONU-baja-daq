#!/usr/bin/env python3
"""
Ground station entry point.

    python run.py                           # dashboards: find the dongle, else simulate
    python run.py --sim                     # force the simulator
    python run.py --list-ports              # see what is plugged in
    python run.py --port COM7               # a specific port
    python run.py --replay logs/LOG0001.TLM # replay an SD card log
    python run.py --vcp                     # wired USB straight to the hub
    python run.py --proto v1                # the old 9-channel format
    python run.py --host 0.0.0.0            # let phones on the same wifi watch
    python run.py --list-logs               # inventory of logs_dir(): name, size, age
    python run.py --clean-logs              # keep the 10 newest logs, delete the rest
    python run.py --clean-logs --keep 5     # keep the 5 newest instead
    python run.py --clean-logs --older-than 30   # delete anything older than 30 days
    python run.py --clean-logs --older-than 30 --dry-run  # preview, deletes nothing

Built as an .exe this is the double-click target, so running with no arguments
has to do something sensible on its own: detect the dongle, fall back to the
simulator if there isn't one, and open a window.

That is also why --proto defaults to v2 (changed 2026-08-23). v1 was the
default for as long as it was the only thing any board was flashed with; that
stopped being true once Front/Rear/Hub were cut over, and the stale default
meant a double-clicked .exe silently decoded a v2 stream with a v1 decoder.
That failure is quiet rather than loud - you get a dashboard full of
plausible-looking wrong numbers, not an error - so the default now matches
what the hardware actually speaks. Pass --proto v1 for the bench spare.

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
import tempfile
import threading
import webbrowser
from datetime import datetime
from pathlib import Path

from telemetry import cleanup, paths, protocols, settings
from telemetry.protocols import active
from telemetry.recorder import CsvRecorder
from telemetry.sources import (
    FrameSource,
    ReplaySource,
    SerialSource,
    SimulatorSource,
    SimulatorSourceV2,
    VcpSource,
    guess_port,
    guess_vcp_port,
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
        "--vcp",
        action="store_true",
        help="read the hub over its ST-Link USB port instead of the radio "
        "(wired, full rate; auto-detects the port)",
    )
    src.add_argument(
        "--can",
        metavar="CHANNEL",
        help="tap the CAN bus directly through a USB-CAN adapter, e.g. "
        "--can COM7 or --can can0. Requires python-can.",
    )
    src.add_argument(
        "--no-reconnect",
        action="store_true",
        help="exit instead of retrying when the serial link drops",
    )

    canopt = p.add_argument_group("USB-CAN adapter options (--can)")
    canopt.add_argument(
        "--can-interface",
        default="slcan",
        help="python-can backend: slcan (CANable), pcan, kvaser, socketcan, "
        "ixxat, vector. Default: slcan",
    )
    canopt.add_argument(
        "--can-bitrate",
        type=int,
        help="bus bit rate in bit/s. Defaults to the project's bus rate; only "
        "set this if you are tapping a bus running something else.",
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
        default=protocols.V2,
        help="wire format to decode (default v2 - the 24-channel, 4-node, "
        "80-byte format the Front/Rear/Hub boards are flashed with today). "
        "Pass --proto v1 for the older 9-channel 42-byte format, which is "
        "still what the 1st Bluepill bench spare emits. --sim honours this "
        "flag: it picks the v2 simulator here and the v1 one under --proto v1.",
    )

    cl = p.add_argument_group(
        "log cleanup",
        "manage the timestamped CSVs and telemetry.log that pile up in "
        "logs_dir() over time. Never touches field_data/.",
    )
    cl.add_argument(
        "--clean-logs",
        action="store_true",
        help="prune old logs, then exit without starting the app. With "
        "neither --keep nor --older-than, defaults to --keep 10.",
    )
    cl.add_argument(
        "--keep",
        type=int,
        metavar="N",
        help="with --clean-logs: keep the N newest logs, delete the rest",
    )
    cl.add_argument(
        "--older-than",
        type=float,
        metavar="DAYS",
        help="with --clean-logs: delete logs older than this many days",
    )
    cl.add_argument(
        "--dry-run",
        action="store_true",
        help="with --clean-logs: print what would be deleted, delete nothing",
    )
    cl.add_argument(
        "--list-logs",
        action="store_true",
        help="print the logs_dir() inventory (name, size, age) and exit",
    )

    p.add_argument("-v", "--verbose", action="store_true")

    return p


def timestamped(prefix: str, suffix: str) -> Path:
    return paths.logs_dir() / f"{prefix}_{datetime.now():%Y%m%d_%H%M%S}{suffix}"


# --------------------------------------------------------------------
# Log cleanup
# --------------------------------------------------------------------


def human_bytes(n: int) -> str:
    """1536 -> '1.5 KB'. Only the units this app's logs actually reach."""
    size = float(n)
    for unit in ("B", "KB", "MB", "GB"):
        if size < 1024 or unit == "GB":
            return f"{size:.0f} {unit}" if unit == "B" else f"{size:.1f} {unit}"
        size /= 1024
    return f"{size:.1f} GB"  # unreachable, keeps type-checkers happy


def cmd_list_logs() -> int:
    files = cleanup.list_logs()
    if not files:
        print(f"No logs found in {paths.logs_dir()}")
        return 0

    print(f"Logs in {paths.logs_dir()} (newest first):")
    print(f"{'NAME':<40} {'SIZE':>10}  AGE")
    total = 0
    for lf in files:
        total += lf.bytes
        print(f"{lf.path.name:<40} {human_bytes(lf.bytes):>10}  {lf.age_days:.1f}d")
    print(f"\n{len(files)} file(s), {human_bytes(total)} total")
    return 0


def cmd_clean_logs(args, log_file: Path | None) -> int:
    """
    Handles --clean-logs. Runs and exits without touching the source /
    dashboard machinery at all - this is a maintenance command, not a
    startup flag.
    """
    keep = args.keep
    older_than = args.older_than

    if keep is None and older_than is None:
        # A bare --clean-logs is far more likely to mean "tidy up" than
        # "I have thought carefully and want zero filters applied" - and
        # prune_logs() itself refuses to delete anything when both are None,
        # so silently doing nothing here would be a confusing no-op. Default
        # to something useful instead, and say so.
        keep = 10
        print("--clean-logs given with no --keep or --older-than; "
              "defaulting to --keep 10")

    # Never delete the log file this very process is writing to - setup_logging()
    # already has the handle open, and unlinking it out from under a live
    # FileHandler on Windows would either fail (locked) or silently break
    # further logging. Protecting it explicitly means that either way, it is
    # never even attempted.
    protect = (log_file,) if log_file is not None else ()

    result = cleanup.prune_logs(
        keep=keep,
        older_than_days=older_than,
        dry_run=args.dry_run,
        protect=protect,
    )

    verb = "Would delete" if result.dry_run else "Deleted"
    print(f"{verb} {len(result.deleted)} file(s), "
          f"freeing {human_bytes(result.freed_bytes)}")
    print(f"Kept {len(result.kept)} file(s)")
    if result.errors:
        print(f"{len(result.errors)} error(s):")
        for err in result.errors:
            print(f"  {err}")

    return 1 if result.errors else 0


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
        # Which simulator depends on the active protocol, not on a separate
        # flag: --sim means "no hardware", and the shape of synthetic data has
        # to match whatever wire format the rest of the app was told to speak.
        # Making the user pick both would let them pick an impossible pair.
        sim_cls = SimulatorSourceV2 if active().NODE_COUNT == 4 else SimulatorSource
        try:
            return sim_cls(drop_rate=args.drop_rate,
                           offline_node=args.offline_node), None
        except RuntimeError as exc:
            # Usage or environment problem (e.g. simulation/ missing from a
            # frozen build), not a crash - print the message the source wrote
            # rather than a traceback, which reads like a bug for something
            # the user can fix.
            for line in str(exc).splitlines():
                log.error("%s", line)
            return None, None

    if args.can:
        # Direct USB-CAN adapter tap. Imported here rather than at module
        # scope so that python-can - which is optional and usually absent -
        # is only required by people who actually pass --can.
        from telemetry.can_adapter import CanAdapterSource
        try:
            source = CanAdapterSource(
                channel=args.can,
                interface=args.can_interface,
                bitrate=args.can_bitrate,
                reconnect=not args.no_reconnect,
            )
        except RuntimeError as exc:
            for line in str(exc).splitlines():
                log.error("%s", line)
            return None, None
        return source, raw_path

    baud = args.baud or cfg["baud"]
    port = args.port or cfg.get("port")

    if args.vcp:
        # Wired link to the hub's ST-Link USB serial port. Same byte stream as
        # the radio, so this is SerialSource with a different port and baud -
        # see VcpSource. Full frame rate, no radio in the path.
        vcp_port = args.port or guess_vcp_port()
        if vcp_port is None:
            log.error("No ST-Link Virtual COM Port found.")
            log.error("Plug the Nucleo hub in over USB, or name the port with --port.")
            return None, None
        try:
            source = VcpSource(
                port=vcp_port,
                baud=args.baud or 115200,
                raw_log=raw_path,
                reconnect=not args.no_reconnect,
            )
        except RuntimeError as exc:
            log.error("%s", exc)
            return None, None
        return source, raw_path

    # An explicitly requested port is pinned; otherwise the source re-detects
    # on every reconnect, so plugging the dongle into a different USB socket
    # mid-session just works.
    if port is None and guess_port() is None:
        log.warning("No serial port detected - starting the simulator instead.")
        log.warning("Plug in the E22 dongle and restart, or use --port to name one.")
        # Same protocol-driven choice as the --sim branch above: falling back
        # to a v1 simulator while the app is decoding v2 would show a dashboard
        # with the wrong channels on it.
        fallback = SimulatorSourceV2 if active().NODE_COUNT == 4 else SimulatorSource
        return fallback(), None

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

            # A FRESH temp directory every launch, not a persistent one in
            # the repo's data dir - two problems that used to share one root
            # cause. (1) A persistent Chromium profile keeps its own disk
            # cache across restarts; the no-cache response headers added
            # 2026-08-16 stop *future* staleness, but an entry fetched
            # before that fix (e.g. the old broken js/telemetry-core.js
            # 404) could still be served from the old profile's cache until
            # something evicted it - "restart the app, still see the old
            # bug" is confusing to debug blind. A one-shot temp profile
            # means there is never anything to have gone stale. (2) The old
            # persistent .browser-profile/ was the 1000+-file git-hygiene
            # wart NEXT_STEPS.md carved a .gitignore exception around
            # (Chromium's own Login Data/Vpn Tokens databases, empty but
            # awkward to have in a public repo at all) - a temp dir is
            # outside the repo entirely, so that carve-out is no longer
            # load-bearing (left in .gitignore, harmless if unused).
            # The OS is responsible for eventually cleaning temp dirs; not
            # deleting it ourselves on exit is deliberate; the app window
            # is often still open when this process exits (Ctrl+C leaves
            # the window up), and deleting a profile out from under a
            # running Chromium process is its own bug to debug.
            profile_dir = Path(tempfile.mkdtemp(prefix="telemetry-app-"))
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

    if args.list_logs:
        return cmd_list_logs()

    if args.clean_logs:
        return cmd_clean_logs(args, log_file)

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
    # Logged unconditionally, not just for the non-default case. Picking the
    # wrong wire format is the single most confusing failure this app has -
    # a v1 decoder pointed at a v2 stream doesn't error, it just shows stale
    # or garbage channels - and the frame size in this line ("42 bytes" vs
    # "80 bytes") is the fastest way to spot it in a log after the fact.
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
