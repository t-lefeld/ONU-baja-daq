"""
Frame sources: live serial, built-in simulator, and log replay.

Each source is an async iterator of active().Frame, so server.py does not care
where the data came from. That is what lets you develop the whole dashboard on
a laptop with no hardware plugged in and then switch to the real link by
changing one command-line flag.

Sources never raise out of frames() for recoverable conditions. A USB dongle
being unplugged is normal operation, not an error: the source reports itself
disconnected, keeps retrying, and resumes when the hardware comes back. The
server's job is to display that, not to crash on it.
"""

from __future__ import annotations

import asyncio
import logging
import math
import random
import time
from abc import ABC, abstractmethod
from pathlib import Path
from typing import AsyncIterator

from .protocols import active
from .proto import Frame  # noqa: F401 - type annotations only; runtime
                          # construction goes through active() below so a
                          # --proto v2 run builds v2 records, not v1 ones.

log = logging.getLogger("telemetry")

try:
    import serial
    import serial.tools.list_ports
except ImportError:  # pyserial is only needed for the live source
    serial = None  # type: ignore[assignment]


# Link states reported to the UI.
LINK_LIVE = "live"            # receiving, or expected to be
LINK_WAITING = "waiting"      # no port present, retrying
LINK_ERROR = "error"          # port present but failing
LINK_NA = "n/a"               # simulator / replay, nothing to connect to


class FrameSource(ABC):
    """Common interface. ``name`` is shown in the dashboard header."""

    name: str = "unknown"
    link_state: str = LINK_NA
    detail: str = ""

    @abstractmethod
    def frames(self) -> AsyncIterator[Frame]:
        ...

    async def close(self) -> None:
        return None

    def stats(self) -> dict:
        return {}


# ====================================================================
# Live serial
# ====================================================================


def list_ports() -> list[tuple[str, str]]:
    """(device, description) for every serial port currently present."""
    if serial is None:
        return []
    try:
        return [(p.device, p.description or "") for p in serial.tools.list_ports.comports()]
    except Exception as exc:  # noqa: BLE001 - enumeration can fail on odd drivers
        log.debug("Port enumeration failed: %s", exc)
        return []


def guess_port() -> str | None:
    """
    Best guess at which port the E22 USB dongle is on.

    The -U variant uses a CH340 bridge, which Windows reports as "USB-SERIAL
    CH340" and Linux exposes as /dev/ttyUSB*. Fall back to any port that is not
    an ST-Link, so we do not grab a Nucleo's debug VCP by mistake - which would
    "work" in the sense of opening, and then deliver nothing.
    """
    ports = list_ports()
    if not ports:
        return None

    for device, desc in ports:
        if "ch340" in desc.lower() or "ch910" in desc.lower():
            return device

    for device, desc in ports:
        low = desc.lower()
        if "st-link" not in low and "stlink" not in low:
            return device

    return ports[0][0]


class SerialSource(FrameSource):
    """
    Reads the E22 USB dongle (or any UART carrying the frame stream).

    Reconnects on its own. pyserial is blocking, so reads run in the default
    thread executor; the read timeout keeps that thread from pinning a core
    while still letting a cancelled task shut down promptly.
    """

    RETRY_DELAY = 2.0

    def __init__(
        self,
        port: str | None = None,
        baud: int = 9600,
        raw_log: Path | None = None,
        reconnect: bool = True,
    ) -> None:
        if serial is None:
            raise RuntimeError("pyserial is not installed - run: pip install pyserial")

        self.requested_port = port      # None means auto-detect on each attempt
        self.baud = baud
        self.reconnect = reconnect
        self.parser = active().StreamParser()

        self.port: str | None = port
        self.name = f"serial {port or 'auto'} @ {baud}"
        self.link_state = LINK_WAITING
        self.detail = "not connected yet"

        self.connects = 0
        self.disconnects = 0
        self.bytes_read = 0
        self.last_byte_at: float | None = None

        self._ser = None
        self._raw_path = raw_log
        self._raw_log = None

    # ---------------- connection management ----------------

    def _open(self) -> bool:
        port = self.requested_port or guess_port()

        if port is None:
            self.link_state = LINK_WAITING
            self.detail = "no serial ports found"
            return False

        try:
            self._ser = serial.Serial(port, self.baud, timeout=0.1)
        except Exception as exc:  # noqa: BLE001 - pyserial raises several types
            self.link_state = LINK_ERROR
            self.detail = f"{port}: {exc}"
            self._ser = None
            return False

        self.port = port
        self.name = f"serial {port} @ {self.baud}"
        self.link_state = LINK_LIVE
        self.detail = "connected"
        self.connects += 1

        # Anything buffered while we were away is almost certainly a partial
        # frame. The parser would resync anyway, but discarding is cheaper and
        # keeps the CRC error count meaningful.
        try:
            self._ser.reset_input_buffer()
        except Exception:  # noqa: BLE001
            pass

        log.info("Serial connected: %s at %d baud", port, self.baud)

        if self._raw_path and self._raw_log is None:
            try:
                self._raw_log = self._raw_path.open("ab")
            except OSError as exc:
                log.warning("Could not open raw log %s (%s)", self._raw_path, exc)

        return True

    def _drop(self, why: str) -> None:
        if self._ser is not None:
            try:
                self._ser.close()
            except Exception:  # noqa: BLE001
                pass
            self._ser = None
            self.disconnects += 1
            log.warning("Serial disconnected: %s", why)

        self.link_state = LINK_WAITING if self.reconnect else LINK_ERROR
        self.detail = why

    def _read(self) -> bytes:
        # read(1) blocks up to the timeout, then in_waiting drains the rest in
        # one go. Avoids both busy-waiting and one-byte-per-call overhead.
        ser = self._ser
        if ser is None:
            return b""
        chunk = ser.read(1)
        pending = ser.in_waiting
        if pending:
            chunk += ser.read(pending)
        return chunk

    # ---------------- iteration ----------------

    async def frames(self) -> AsyncIterator[Frame]:
        loop = asyncio.get_running_loop()

        while True:
            if self._ser is None:
                if not self._open():
                    if not self.reconnect:
                        return
                    await asyncio.sleep(self.RETRY_DELAY)
                    continue

            try:
                data = await loop.run_in_executor(None, self._read)
            except asyncio.CancelledError:
                raise
            except Exception as exc:  # noqa: BLE001 - unplug surfaces variously
                self._drop(str(exc))
                if not self.reconnect:
                    return
                await asyncio.sleep(self.RETRY_DELAY)
                continue

            if not data:
                continue

            self.bytes_read += len(data)
            self.last_byte_at = time.time()

            if self._raw_log:
                try:
                    self._raw_log.write(data)
                    self._raw_log.flush()
                except OSError as exc:
                    log.warning("Raw log write failed (%s); disabling", exc)
                    self._raw_log = None

            for frame in self.parser.feed(data):
                yield frame

    async def close(self) -> None:
        if self._raw_log:
            self._raw_log.close()
            self._raw_log = None
        if self._ser is not None:
            try:
                self._ser.close()
            except Exception:  # noqa: BLE001
                pass
            self._ser = None

    def stats(self) -> dict:
        return {
            "port": self.port,
            "baud": self.baud,
            "connects": self.connects,
            "disconnects": self.disconnects,
            "bytes_read": self.bytes_read,
            "crc_errors": self.parser.crc_errors,
            "bytes_discarded": self.parser.bytes_discarded,
        }


# ====================================================================
# Simulator
# ====================================================================


class SimulatorSource(FrameSource):
    """
    Generates the same waveforms the Bluepill firmware produces.

    Deliberately a port of sim_read_channels() in can_node.c rather than
    something prettier, so what you see with no hardware attached is what you
    will see with hardware attached. If the two diverge, the dashboard stops
    being a useful test of the real system.
    """

    def __init__(self, drop_rate: float = 0.0, offline_node: int | None = None) -> None:
        self.name = "simulator"
        self.link_state = LINK_NA
        self.detail = "synthetic data, no hardware"
        # This simulator is v1-shaped by construction: _channels() hardcodes
        # three engineering values per node, and NodeRecord is built with v1's
        # can_seq field, which v2 does not have (it uses a per-burst epoch).
        #
        # Rather than grow a second full-sensor-suite simulator here, point at
        # the one that already exists and is already tested. simulation/ builds
        # 24 channels across 4 nodes and round-trips them through the real v2
        # codec, and tools/test_channel_sync.py keeps its channel table locked
        # to the firmware's. A copy of that logic living here too would be a
        # second thing to keep in sync, which is the exact drift that test was
        # written to prevent.
        if active().NODE_COUNT != 3:
            raise RuntimeError(
                "the built-in simulator only produces v1 (3-node) frames.\n"
                "For a v2 synthetic feed use the dedicated simulator instead:\n"
                "    cd simulation && python run_sim.py\n"
                "--proto v2 here is for real v2 data over serial or --replay."
            )

        self.drop_rate = drop_rate
        self.offline_node = offline_node
        self._t0 = time.monotonic()
        self._seq = 0
        self._can_seq = [0] * active().NODE_COUNT

    def _channels(self, node: int, t: float) -> tuple[int, int, int]:
        defs = active().CHANNELS[node]
        n = lambda a: random.uniform(-a, a)  # noqa: E731

        if node == 0:
            eng = (
                22.5 + 7.0 * math.sin(t * 0.10) + n(0.15),
                50.0 + 18.0 * math.sin(t * 0.07 + 1.2) + n(0.4),
                100.5 + 1.2 * math.sin(t * 0.04) + n(0.05),
            )
        elif node == 1:
            phase = (t % 12.0) / 12.0
            current = 0.5 + 7.5 * phase + n(0.05)
            volts = 12.9 - 0.11 * current + n(0.01)
            eng = (volts, current, volts * current)
        else:  # node == 2
            eng = (
                0.45 * math.sin(t * 1.7) + n(0.01),
                0.45 * math.cos(t * 1.7) + n(0.01),
                1.00 + 0.08 * math.sin(t * 3.1) + n(0.01),
            )

        return tuple(d.to_raw(v) for d, v in zip(defs, eng))  # type: ignore[return-value]

    def _build(self) -> Frame:
        t = time.monotonic() - self._t0
        records = []

        for node in range(active().NODE_COUNT):
            if node == self.offline_node:
                records.append(
                    active().NodeRecord(node, active().NF_STALE, self._can_seq[node], 0, (0, 0, 0))
                )
                continue

            frames_expected = active().FRAME_PERIOD_MS // active().NODE_TX_PERIOD_MS
            lost = sum(1 for _ in range(frames_expected) if random.random() < self.drop_rate)
            self._can_seq[node] = (self._can_seq[node] + frames_expected) & 0xFF

            records.append(
                active().NodeRecord(
                    node_id=node,
                    flags=active().NF_ONLINE,
                    can_seq=self._can_seq[node],
                    loss=min(lost, 255),
                    raw=self._channels(node, t),
                )
            )

        frame = active().Frame(seq=self._seq & 0xFFFF, t_ms=int(t * 1000) & 0xFFFFFFFF, nodes=records)
        self._seq += 1
        return frame

    async def frames(self) -> AsyncIterator[Frame]:
        period = active().FRAME_PERIOD_MS / 1000.0
        next_at = time.monotonic()
        while True:
            next_at += period
            await asyncio.sleep(max(0.0, next_at - time.monotonic()))
            yield self._build()


# ====================================================================
# Replay
# ====================================================================


class ReplaySource(FrameSource):
    """
    Replays a .tlm file written by the hub's SD logger, or a raw capture.

    Runs through the same StreamParser as the live link, so a log recorded over
    a lossy radio replays with exactly the corruption it was recorded with -
    which is what makes it useful for debugging the decoder.
    """

    def __init__(self, path: Path, speed: float = 1.0, loop: bool = False) -> None:
        self.path = Path(path)
        self.name = f"replay {self.path.name}"
        self.link_state = LINK_NA
        self.detail = str(self.path)
        self.speed = speed
        self.loop = loop

        if not self.path.exists():
            raise FileNotFoundError(self.path)

        # StreamParser's default max_buffer (4096 bytes) is sized for a LIVE
        # link, where feed() is called repeatedly with small chunks as bytes
        # arrive and a bounded buffer is a deliberate memory-safety choice.
        # ReplaySource instead reads the WHOLE file and hands it to feed() in
        # one call (below) - with the default cap, any recording longer than
        # ~4096 bytes (about 25s at v2's 160 B/s) got silently truncated from
        # the FRONT before a single frame was even decoded, so a multi-minute
        # replay only ever played its last few seconds. bytes_discarded (see
        # stats() below) would have caught this if anyone was watching it,
        # but nothing surfaces that counter loudly. Since the whole file is
        # already in memory by the time feed() runs, there is no reason for
        # the parser's own buffer to be any smaller than the file.
        file_size = self.path.stat().st_size
        self.parser = active().StreamParser(max_buffer=file_size + 4096)

    async def frames(self) -> AsyncIterator[Frame]:
        while True:
            data = self.path.read_bytes()
            prev_t: int | None = None

            for frame in self.parser.feed(data):
                if prev_t is not None and self.speed > 0:
                    # t_ms is hub uptime, so gaps between frames are real even
                    # though the absolute value means nothing after a reset.
                    delta = (frame.t_ms - prev_t) & 0xFFFFFFFF
                    if 0 < delta < 60_000:
                        await asyncio.sleep(delta / 1000.0 / self.speed)
                prev_t = frame.t_ms
                yield frame

            if not self.loop:
                self.detail = "replay finished"
                return

    def stats(self) -> dict:
        return {
            "file": str(self.path),
            "crc_errors": self.parser.crc_errors,
            "bytes_discarded": self.parser.bytes_discarded,
        }
