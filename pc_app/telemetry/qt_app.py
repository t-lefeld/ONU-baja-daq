"""
Native PySide6 window: the default ground-station UI.

Deliberately plain QtWidgets - QLabel, QFrame, QGridLayout, and one custom
QPainter widget for the sparklines. No QtWebEngine, no embedded browser, no
Chrome/Edge subprocess anywhere in this file or anything it imports. That is
the whole point of this module existing: run.py used to open the dashboard as
an Edge/Chrome "app mode" window, which is still technically a browser tab
wearing a costume; this is a real native window instead.

Layout mirrors static/index.html on purpose (status bar, one panel per node,
one row per channel, a sparkline under each value) so the two front ends stay
recognizably the same product. Restyle freely later - every visual choice
lives in _apply_palette() and the widget constructors below, none of it
leaks into the frame-handling logic.

Threading model
----------------
FramePump.run()/status_loop() are asyncio coroutines. Qt has its own event
loop and does not share one with asyncio. TelemetryWorker (a QThread) runs a
private asyncio loop in the background and turns pump output into Qt signals;
MainWindow only ever touches the pump's data from signal handlers, which Qt
delivers on the GUI thread via a queued connection because the emitting
thread and the receiving thread differ. Nothing here reaches across threads
by any other path.
"""

from __future__ import annotations

import asyncio
import collections
import logging
import time
from typing import Any

from PySide6 import QtCore, QtGui, QtWidgets

from .pump import FramePump
from .recorder import CsvRecorder
from .sources import FrameSource

log = logging.getLogger("telemetry")

HISTORY = 240          # samples kept per sparkline, ~2 min at 2 Hz
RATE_WINDOW = 20        # arrivals kept for the rate readout

# Same palette as static/index.html's CSS variables, so the two front ends
# read as the same app rather than two different tools.
COLOR_BG = "#ffffff"
COLOR_FG = "#111111"
COLOR_MUTED = "#666666"
COLOR_BORDER = "#d4d4d4"
COLOR_PANEL = "#fafafa"
COLOR_ACCENT = "#0b62d0"
COLOR_OK = "#1a7f37"
COLOR_WARN = "#9a6700"
COLOR_BAD = "#c1121f"

LINK_COLORS = {
    "live": COLOR_OK,
    "waiting": COLOR_WARN,
    "error": COLOR_BAD,
    "down": COLOR_BAD,
    "n/a": COLOR_MUTED,
}


def fmt_value(v: float) -> str:
    a = abs(v)
    if a >= 1000:
        return f"{v:.0f}"
    if a >= 100:
        return f"{v:.1f}"
    if a >= 1:
        return f"{v:.2f}"
    return f"{v:.3f}"


# ========================================================================
# Sparkline
# ========================================================================


class Sparkline(QtWidgets.QWidget):
    """Autoscaled line, no fill, no axes. A dead-flat signal still draws a
    visible line through the middle rather than vanishing (see the min-band
    guard below) - the same rule static/index.html's drawSpark() uses."""

    def __init__(self, maxlen: int = HISTORY, parent: QtWidgets.QWidget | None = None) -> None:
        super().__init__(parent)
        self._data: collections.deque[float] = collections.deque(maxlen=maxlen)
        self.setMinimumHeight(28)
        self.setMaximumHeight(36)
        self.setSizePolicy(QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Fixed)

    def push(self, value: float) -> None:
        self._data.append(value)
        self.update()

    def clear(self) -> None:
        self._data.clear()
        self.update()

    def paintEvent(self, event: QtGui.QPaintEvent) -> None:  # noqa: N802 - Qt override
        painter = QtGui.QPainter(self)
        painter.setRenderHint(QtGui.QPainter.Antialiasing)
        w, h = self.width(), self.height()

        if len(self._data) < 2 or w <= 0 or h <= 0:
            painter.end()
            return

        lo, hi = min(self._data), max(self._data)
        if hi - lo < 1e-9:
            lo -= 0.5
            hi += 0.5
        pad = (hi - lo) * 0.1
        lo -= pad
        hi += pad

        n = len(self._data)
        points = [
            QtCore.QPointF(i / (n - 1) * w, h - (v - lo) / (hi - lo) * h)
            for i, v in enumerate(self._data)
        ]

        painter.setPen(QtGui.QPen(QtGui.QColor(COLOR_ACCENT), 1))
        painter.drawPolyline(QtGui.QPolygonF(points))
        painter.end()


# ========================================================================
# Channel row / node panel
# ========================================================================


class ChannelRow(QtWidgets.QWidget):
    def __init__(self, name: str, unit: str, parent: QtWidgets.QWidget | None = None) -> None:
        super().__init__(parent)
        self.unit = unit

        outer = QtWidgets.QVBoxLayout(self)
        outer.setContentsMargins(0, 6, 0, 6)
        outer.setSpacing(2)

        top = QtWidgets.QHBoxLayout()
        top.setContentsMargins(0, 0, 0, 0)

        self.name_label = QtWidgets.QLabel(name)
        self.name_label.setStyleSheet(f"color: {COLOR_MUTED}; font-size: 11px;")

        self.value_label = QtWidgets.QLabel("--")
        self.value_label.setStyleSheet("font-family: Consolas, monospace; font-size: 16px;")
        self.value_label.setAlignment(QtCore.Qt.AlignRight | QtCore.Qt.AlignVCenter)

        top.addWidget(self.name_label)
        top.addStretch(1)
        top.addWidget(self.value_label)

        self.spark = Sparkline()

        outer.addLayout(top)
        outer.addWidget(self.spark)

    def set_value(self, value: float) -> None:
        self.value_label.setText(f"{fmt_value(value)} {self.unit}")
        self.spark.push(value)

    def reset(self) -> None:
        self.value_label.setText("--")
        self.spark.clear()


class NodePanel(QtWidgets.QFrame):
    def __init__(self, node_id: int, label: str, channels: list[dict],
                 parent: QtWidgets.QWidget | None = None) -> None:
        super().__init__(parent)
        self.node_id = node_id
        self.setFrameShape(QtWidgets.QFrame.StyledPanel)
        self.setStyleSheet(
            f"NodePanel {{ background: {COLOR_PANEL}; border: 1px solid {COLOR_BORDER}; }}"
        )

        layout = QtWidgets.QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(0)

        head = QtWidgets.QWidget()
        head.setStyleSheet(f"border-bottom: 1px solid {COLOR_BORDER};")
        head_layout = QtWidgets.QHBoxLayout(head)
        head_layout.setContentsMargins(12, 8, 12, 8)

        title = QtWidgets.QLabel(f"{label}  <span style='color:{COLOR_MUTED}; "
                                  f"font-weight:normal; font-size:11px;'>node {node_id}</span>")
        title.setStyleSheet("font-weight: 600;")

        self.state_label = QtWidgets.QLabel("waiting")
        self.state_label.setStyleSheet(f"color: {COLOR_MUTED}; font-weight: 600; font-size: 12px;")

        head_layout.addWidget(title)
        head_layout.addStretch(1)
        head_layout.addWidget(self.state_label)

        body = QtWidgets.QWidget()
        body_layout = QtWidgets.QVBoxLayout(body)
        body_layout.setContentsMargins(12, 0, 12, 4)
        body_layout.setSpacing(0)

        self.rows: dict[str, ChannelRow] = {}
        for i, ch in enumerate(channels):
            row = ChannelRow(ch["name"], ch["unit"])
            if i > 0:
                line = QtWidgets.QFrame()
                line.setFrameShape(QtWidgets.QFrame.HLine)
                line.setStyleSheet(f"color: {COLOR_BORDER};")
                body_layout.addWidget(line)
            body_layout.addWidget(row)
            self.rows[ch["name"]] = row

        layout.addWidget(head)
        layout.addWidget(body)

        self.set_stale(True)

    def set_stale(self, stale: bool) -> None:
        self.setGraphicsEffect(None)
        opacity = 0.5 if stale else 1.0
        effect = QtWidgets.QGraphicsOpacityEffect(self)
        effect.setOpacity(opacity)
        self.setGraphicsEffect(effect)

    def apply_record(self, rec: dict) -> None:
        self.set_stale(bool(rec.get("stale")))

        if rec.get("stale"):
            self.state_label.setText("offline")
            self.state_label.setStyleSheet(f"color: {COLOR_BAD}; font-weight: 600; font-size: 12px;")
        elif rec.get("fault"):
            self.state_label.setText("sensor fault")
            self.state_label.setStyleSheet(f"color: {COLOR_WARN}; font-weight: 600; font-size: 12px;")
        else:
            loss = rec.get("loss", 0)
            self.state_label.setText(f"{loss} lost" if loss else "live")
            self.state_label.setStyleSheet(f"color: {COLOR_OK}; font-weight: 600; font-size: 12px;")

        for ch in rec.get("channels", []):
            row = self.rows.get(ch["name"])
            if row is not None:
                row.set_value(ch["value"])


# ========================================================================
# Background worker: bridges FramePump (asyncio) into Qt signals
# ========================================================================


class TelemetryWorker(QtCore.QThread):
    """
    Owns a private asyncio event loop and runs FramePump on it. Qt signals
    are the only thing that crosses back to the GUI thread; nothing else
    here is safe to touch from outside this thread once run() has started.
    """

    frame_ready = QtCore.Signal(dict)
    status_ready = QtCore.Signal(dict)
    worker_stopped = QtCore.Signal()

    def __init__(self, pump: FramePump, parent: QtCore.QObject | None = None) -> None:
        super().__init__(parent)
        self.pump = pump
        self._loop: asyncio.AbstractEventLoop | None = None
        self._tasks: list[asyncio.Task] = []

    def run(self) -> None:  # noqa: N802 - QThread override
        self._loop = asyncio.new_event_loop()
        asyncio.set_event_loop(self._loop)
        try:
            self._loop.run_until_complete(self._main())
        finally:
            self._loop.close()
            self._loop = None
        self.worker_stopped.emit()

    async def _main(self) -> None:
        pump_task = asyncio.create_task(self.pump.run(self.frame_ready.emit))
        status_task = asyncio.create_task(self.pump.status_loop(self.status_ready.emit))
        self._tasks = [pump_task, status_task]

        try:
            await asyncio.gather(*self._tasks, return_exceptions=True)
        finally:
            await self.pump.close()

    def stop(self) -> None:
        """Ask the worker to unwind. Safe to call from the GUI thread."""
        loop = self._loop
        if loop is None:
            return
        for task in self._tasks:
            loop.call_soon_threadsafe(task.cancel)


# ========================================================================
# Main window
# ========================================================================


class MainWindow(QtWidgets.QMainWindow):
    def __init__(self, pump: FramePump, parent: QtWidgets.QWidget | None = None) -> None:
        super().__init__(parent)
        self.pump = pump
        self.panels: dict[int, NodePanel] = {}
        self.frame_count = 0
        self.total_loss = 0
        self.last_frame_at: float | None = None
        self._recent: collections.deque[float] = collections.deque(maxlen=RATE_WINDOW)

        self.setWindowTitle("CAN → LoRa Telemetry")
        self.resize(1000, 700)
        self.setStyleSheet(f"background: {COLOR_BG}; color: {COLOR_FG};")

        self._build_ui(pump.meta())

        self.age_timer = QtCore.QTimer(self)
        self.age_timer.setInterval(200)
        self.age_timer.timeout.connect(self._tick_age)
        self.age_timer.start()

        self.worker = TelemetryWorker(pump, self)
        self.worker.frame_ready.connect(self._on_frame)
        self.worker.status_ready.connect(self._on_status)
        self.worker.start()

    # ---------------- layout ----------------

    def _build_ui(self, meta: dict[str, Any]) -> None:
        central = QtWidgets.QWidget()
        self.setCentralWidget(central)
        root = QtWidgets.QVBoxLayout(central)
        root.setContentsMargins(0, 0, 0, 0)
        root.setSpacing(0)

        root.addWidget(self._build_bar())

        scroll = QtWidgets.QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setFrameShape(QtWidgets.QFrame.NoFrame)

        grid_holder = QtWidgets.QWidget()
        grid = QtWidgets.QGridLayout(grid_holder)
        grid.setContentsMargins(12, 12, 12, 12)
        grid.setSpacing(12)

        nodes = meta.get("nodes", [])
        columns = 2 if len(nodes) > 1 else 1
        for i, node in enumerate(nodes):
            panel = NodePanel(node["node_id"], node["label"], node["channels"])
            self.panels[node["node_id"]] = panel
            grid.addWidget(panel, i // columns, i % columns)

        scroll.setWidget(grid_holder)
        root.addWidget(scroll, 1)

        self.detail_label = QtWidgets.QLabel("")
        self.detail_label.setStyleSheet(f"color: {COLOR_MUTED}; font-size: 12px; padding: 6px 16px;")
        root.addWidget(self.detail_label)

    def _build_bar(self) -> QtWidgets.QWidget:
        bar = QtWidgets.QWidget()
        bar.setStyleSheet(f"border-bottom: 1px solid {COLOR_BORDER};")
        layout = QtWidgets.QHBoxLayout(bar)
        layout.setContentsMargins(16, 10, 16, 10)
        layout.setSpacing(20)

        title = QtWidgets.QLabel("Telemetry")
        title.setStyleSheet("font-weight: 600; font-size: 14px;")
        layout.addWidget(title)

        self.link_label = QtWidgets.QLabel("connecting")
        self.link_label.setStyleSheet(f"color: {COLOR_MUTED}; font-weight: 600; font-size: 12px;")
        layout.addWidget(self.link_label)

        self.source_label = self._stat(layout, "source", "-")
        self.frames_label = self._stat(layout, "frames", "0")
        self.rate_label = self._stat(layout, "rate", "-")
        self.age_label = self._stat(layout, "last", "-")
        self.loss_label = self._stat(layout, "CAN loss", "0")
        self.crc_label = self._stat(layout, "CRC err", "0")

        layout.addStretch(1)
        return bar

    def _stat(self, layout: QtWidgets.QHBoxLayout, name: str, value: str) -> QtWidgets.QLabel:
        wrap = QtWidgets.QWidget()
        row = QtWidgets.QHBoxLayout(wrap)
        row.setContentsMargins(0, 0, 0, 0)
        row.setSpacing(4)

        label = QtWidgets.QLabel(name)
        label.setStyleSheet(f"color: {COLOR_MUTED}; font-size: 12px;")
        value_label = QtWidgets.QLabel(value)
        value_label.setStyleSheet("font-family: Consolas, monospace; font-size: 12px; font-weight: 600;")

        row.addWidget(label)
        row.addWidget(value_label)
        layout.addWidget(wrap)
        return value_label

    # ---------------- data ----------------

    def _on_frame(self, payload: dict) -> None:
        self.frame_count += 1
        now = time.monotonic()
        self.last_frame_at = now
        self._recent.append(now)

        for rec in payload.get("nodes", []):
            panel = self.panels.get(rec["node_id"])
            if panel is None:
                continue
            self.total_loss += rec.get("loss", 0)
            panel.apply_record(rec)

        self.frames_label.setText(str(self.frame_count))
        self.loss_label.setText(str(self.total_loss))

        if len(self._recent) > 2:
            span = self._recent[-1] - self._recent[0]
            if span > 0:
                hz = (len(self._recent) - 1) / span
                self.rate_label.setText(f"{hz:.2f} Hz")

    def _on_status(self, status: dict) -> None:
        self.source_label.setText(status.get("source") or "-")
        self.crc_label.setText(str(status.get("crc_errors", 0)))

        link = status.get("link") or "n/a"
        self.link_label.setText(link)
        self.link_label.setStyleSheet(
            f"color: {LINK_COLORS.get(link, COLOR_MUTED)}; font-weight: 600; font-size: 12px;"
        )

        error = status.get("error")
        self.detail_label.setText(f"error: {error}" if error else (status.get("detail") or ""))

    def _tick_age(self) -> None:
        if self.last_frame_at is None:
            return
        age = time.monotonic() - self.last_frame_at
        self.age_label.setText(f"{age:.1f}s")
        limit_ms = self.pump.meta().get("frame_period_ms", 500)
        limit = limit_ms / 1000 * 3
        self.age_label.setStyleSheet(
            "font-family: Consolas, monospace; font-size: 12px; font-weight: 600;"
            + (f" color: {COLOR_BAD};" if age > limit else "")
        )

    # ---------------- lifecycle ----------------

    def closeEvent(self, event: QtGui.QCloseEvent) -> None:  # noqa: N802 - Qt override
        self.age_timer.stop()
        self.worker.stop()
        # Give the worker a moment to unwind its asyncio loop and close the
        # source/recorder cleanly; do not hang the window forever if it does not.
        if not self.worker.wait(3000):
            log.warning("Telemetry worker did not stop within 3s; closing anyway")
        event.accept()


# ========================================================================
# Entry point
# ========================================================================


def run_qt_app(source: FrameSource, recorder: CsvRecorder | None = None) -> int:
    """Build the QApplication (if needed), show the window, run the event
    loop to completion, and return a process exit code."""
    app = QtWidgets.QApplication.instance() or QtWidgets.QApplication([])
    pump = FramePump(source, recorder)
    window = MainWindow(pump)
    window.show()
    return app.exec()
