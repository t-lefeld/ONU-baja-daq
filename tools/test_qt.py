#!/usr/bin/env python3
"""
FramePump and native-window tests.

    python tools/test_qt.py

Two halves:

  FramePump   - pure logic, no GUI toolkit involved. This is the code both
                the native window and the (opt-in) browser dashboard share,
                so it is tested once here independently of either front end.

  qt_app      - builds the actual PySide6 MainWindow headlessly
                (QT_QPA_PLATFORM=offscreen) and drives it through both a
                direct signal-handler call and a real worker thread with a
                real asyncio loop, to prove the cross-thread signal bridge
                actually delivers frames rather than just type-checking.

Skips the qt_app half cleanly if PySide6 is not importable, or if the
offscreen platform plugin cannot load (this happens in minimal Linux
environments missing system Mesa/EGL libraries; PySide6's Windows wheels
bundle everything they need, so this is a test-environment limitation, not
an application bug). The FramePump half always runs, since it has no such
dependency.
"""

from __future__ import annotations

import asyncio
import os
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "pc_app"))

from telemetry import proto  # noqa: E402
from telemetry.pump import FramePump  # noqa: E402
from telemetry.recorder import CsvRecorder  # noqa: E402
from telemetry.sources import FrameSource, SimulatorSource  # noqa: E402

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
# FramePump: meta / status shape
# ====================================================================


def test_pump_meta_and_status() -> None:
    print("-- pump: meta() and status() shape")

    pump = FramePump(SimulatorSource())

    meta = pump.meta()
    check("meta type", meta["type"], "meta")
    check("meta source", meta["source"], "simulator")
    check("meta node count", meta["node_count"], proto.NODE_COUNT)
    check("meta lists every node", len(meta["nodes"]), proto.NODE_COUNT)
    check("meta first node's channel count", len(meta["nodes"][0]["channels"]), proto.CH_PER_NODE)

    status = pump.status()
    check("status type", status["type"], "status")
    check("no frames seen yet", status["frames"], 0)
    ok("no error yet", status["error"] is None)
    ok("last_frame_age_s is None before any frame", status["last_frame_age_s"] is None)

    extra_status = pump.status({"clients": 3})
    check("status() merges extra fields", extra_status["clients"], 3)


# ====================================================================
# FramePump: consuming frames, recording, stats
# ====================================================================


def test_pump_run_records_and_tracks_stats() -> None:
    print("-- pump: run() consumes frames, records CSV, updates stats")

    with tempfile.TemporaryDirectory() as td:
        csv_path = Path(td) / "out.csv"
        recorder = CsvRecorder(csv_path)
        pump = FramePump(SimulatorSource(), recorder)

        received: list[dict] = []

        async def scenario():
            task = asyncio.create_task(pump.run(received.append))
            # The simulator ticks at FRAME_PERIOD_MS; wait for a few frames.
            await asyncio.sleep(proto.FRAME_PERIOD_MS / 1000.0 * 3.5)
            task.cancel()
            try:
                await task
            except asyncio.CancelledError:
                pass
            await pump.close()

        asyncio.run(scenario())

        ok("received at least 3 frames in 3.5 periods", len(received) >= 3)
        check("stats.frames_seen matches callback count", pump.stats.frames_seen, len(received))
        ok("last_frame_at was stamped", pump.stats.last_frame_at is not None)
        ok("no pump_error on a clean run", pump.stats.pump_error is None)

        payload = received[0]
        ok("frame payload carries host_epoch", "host_epoch" in payload)
        check("frame payload node count", len(payload["nodes"]), proto.NODE_COUNT)

        status = pump.status()
        check("status reflects csv path", status["csv"], str(csv_path))
        check("status csv_rows matches frames recorded", status["csv_rows"], len(received))

        ok("CSV file actually has rows", csv_path.read_text().count("\n") >= len(received))


def test_pump_status_loop() -> None:
    print("-- pump: status_loop() fires on its own timer")

    pump = FramePump(SimulatorSource())
    ticks: list[dict] = []

    async def scenario():
        task = asyncio.create_task(pump.status_loop(ticks.append, interval=0.05))
        await asyncio.sleep(0.23)
        task.cancel()
        try:
            await task
        except asyncio.CancelledError:
            pass

    asyncio.run(scenario())

    ok("status_loop fired multiple times independent of frame arrivals",
       len(ticks) >= 3)
    ok("every tick is a status message", all(t["type"] == "status" for t in ticks))


# ====================================================================
# FramePump: error handling
# ====================================================================


class _ExplodingSource(FrameSource):
    """A source whose frames() raises - the 'something genuinely
    unexpected happened' case run() is supposed to catch and record rather
    than propagate. Recoverable failures (unplugged USB, etc.) are the
    source's own job to absorb; this exercises the pump's last-resort net."""

    name = "exploding"
    link_state = "n/a"
    detail = ""

    async def frames(self):
        raise RuntimeError("simulated hardware fire")
        yield  # pragma: no cover - makes this an async generator


def test_pump_absorbs_unexpected_source_errors() -> None:
    print("-- pump: run() records a source's unexpected exception instead of crashing")

    pump = FramePump(_ExplodingSource())
    asyncio.run(pump.run(lambda payload: None))

    ok("pump_error was recorded", pump.stats.pump_error is not None)
    ok("pump_error names the exception", "simulated hardware fire" in pump.stats.pump_error)
    check("status surfaces the error", pump.status()["error"], pump.stats.pump_error)


def test_pump_disables_recorder_on_write_failure() -> None:
    print("-- pump: a CSV write failure disables the recorder, not the pump")

    with tempfile.TemporaryDirectory() as td:
        recorder = CsvRecorder(Path(td) / "out.csv")
        pump = FramePump(SimulatorSource(), recorder)

        def boom(*_a, **_k):
            raise OSError("disk full")

        recorder.write = boom  # type: ignore[method-assign]

        received: list[dict] = []

        async def scenario():
            task = asyncio.create_task(pump.run(received.append))
            await asyncio.sleep(proto.FRAME_PERIOD_MS / 1000.0 * 1.5)
            task.cancel()
            try:
                await task
            except asyncio.CancelledError:
                pass

        asyncio.run(scenario())

        ok("frames still flowed despite the CSV write failing", len(received) >= 1)
        ok("recorder was dropped after the failed write", pump.recorder is None)
        ok("no pump_error - a bad CSV must not look like a dead link",
           pump.stats.pump_error is None)


# ====================================================================
# qt_app: the actual PySide6 window, headless
# ====================================================================


def test_qt_app() -> int:
    """Returns 0 (ran, all checks recorded above), or a skip is printed and
    0 is still returned - PySide6 unavailability is not a repo defect."""
    print("-- qt_app: native window, headless")

    os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

    try:
        from PySide6 import QtCore, QtWidgets

        app = QtWidgets.QApplication.instance() or QtWidgets.QApplication([])
    except Exception as exc:  # noqa: BLE001 - missing package or missing system libs
        print(f"  SKIP: PySide6 window could not be created ({exc})")
        print("  (this is a Windows app - a Linux test box missing Mesa/EGL")
        print("   cannot run this half; the PySide6 wheels used on Windows")
        print("   bundle their own Qt runtime and do not have this problem)")
        return 0

    from telemetry.qt_app import MainWindow

    # ---- direct signal-handler call: layout + single-frame update ----
    pump = FramePump(SimulatorSource())
    window = MainWindow(pump)

    check("one panel per node", len(window.panels), proto.NODE_COUNT)
    for node_id in range(proto.NODE_COUNT):
        ok(f"panel for node {node_id} exists", node_id in window.panels)

    payload = {
        "seq": 1, "t_ms": 1000,
        "nodes": [{
            "node_id": 0, "label": "Environmental", "online": True,
            "stale": False, "fault": False, "can_seq": 5, "loss": 0,
            "channels": [
                {"name": "temp_c", "unit": "degC", "value": 22.5, "raw": 2250},
                {"name": "humidity", "unit": "%RH", "value": 50.0, "raw": 5000},
                {"name": "pressure", "unit": "kPa", "value": 100.5, "raw": 1005},
            ],
        }],
    }
    window._on_frame(payload)
    app.processEvents()

    check("channel value label updates from a frame",
          window.panels[0].rows["temp_c"].value_label.text(), "22.50 degC")
    check("frame counter increments", window.frames_label.text(), "1")

    window._on_status({"type": "status", "source": "simulator", "link": "n/a",
                        "detail": "synthetic data, no hardware", "error": None,
                        "crc_errors": 0})
    app.processEvents()
    check("link label reflects status", window.link_label.text(), "n/a")
    check("source label reflects status", window.source_label.text(), "simulator")

    window.worker.stop()
    window.worker.wait(3000)
    ok("worker thread stops on request", not window.worker.isRunning())

    # ---- real worker thread: actual asyncio loop, actual cross-thread signals ----
    pump2 = FramePump(SimulatorSource())
    window2 = MainWindow(pump2)  # starts its own real TelemetryWorker

    loop = QtCore.QEventLoop()
    timer = QtCore.QTimer()
    timer.setSingleShot(True)
    timer.timeout.connect(loop.quit)
    timer.start(int(proto.FRAME_PERIOD_MS * 3.5))
    loop.exec()

    ok("real worker thread delivered frames via Qt signals", window2.frame_count >= 2)
    ok("rate label was computed from real arrivals", window2.rate_label.text() != "-")

    window2.close()  # exercises closeEvent -> worker.stop() -> worker.wait()
    ok("worker thread stops on window close", not window2.worker.isRunning())

    return 0


# ====================================================================


def main() -> int:
    print("FramePump\n")

    for fn in (
        test_pump_meta_and_status,
        test_pump_run_records_and_tracks_stats,
        test_pump_status_loop,
        test_pump_absorbs_unexpected_source_errors,
        test_pump_disables_recorder_on_write_failure,
        # test_qt_app - RETIRED with the native PySide6 window (see run.py's
        # --native). It could never run in a Linux CI (no EGL) so it always
        # reported as a skip-that-looked-like-a-pass, and when it finally ran
        # on Windows it aborted the whole process via qFatal before a single
        # assertion executed. A test that can only be green by not running is
        # worse than no test. The function is left below, unreferenced, so it
        # can be revived if --native ever becomes supported again.
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
