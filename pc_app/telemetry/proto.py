"""
Python mirror of protocol/telemetry_proto.h.

Every constant, offset and scale factor here must match the C header exactly.
tools/test_roundtrip.py compiles the C encoder and compares its output with
this module byte for byte, so drift between the two shows up as a test failure
rather than as a silently wrong number on a dashboard.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field
from typing import Iterator, NamedTuple

# --------------------------------------------------------------------
# Versioning and topology
# --------------------------------------------------------------------

PROTO_VERSION = 0x01

NODE_COUNT = 3
CH_PER_NODE = 3

# --------------------------------------------------------------------
# CAN layer
# --------------------------------------------------------------------

CAN_BITRATE = 500_000
CAN_DATA_BASE_ID = 0x100
CAN_DLC = 8
NODE_TX_PERIOD_MS = 100

ST_OK = 0x00
ST_SENSOR_FAULT = 0x01
ST_STARTUP = 0x02
ST_CAN_ERROR = 0x04

# --------------------------------------------------------------------
# Frame layout
# --------------------------------------------------------------------

SYNC0 = 0xA5
SYNC1 = 0x5A
SYNC = bytes((SYNC0, SYNC1))

FRAME_PERIOD_MS = 500

REC_SIZE = 10
HDR_SIZE = 10
CRC_SIZE = 2
PAYLOAD_SIZE = NODE_COUNT * REC_SIZE          # 30
FRAME_SIZE = HDR_SIZE + PAYLOAD_SIZE + CRC_SIZE  # 42

OFF_SYNC0 = 0
OFF_SYNC1 = 1
OFF_VER = 2
OFF_LEN = 3
OFF_SEQ = 4
OFF_TMS = 6
OFF_PAYLOAD = 10

NF_ONLINE = 0x01
NF_STALE = 0x02
NF_FAULT = 0x04

NODE_TIMEOUT_MS = 1500

# --------------------------------------------------------------------
# Channel table  (keep in step with TLM_CHANNELS in telemetry_proto.c)
# --------------------------------------------------------------------


class ChannelDef(NamedTuple):
    name: str
    unit: str
    scale: float
    offset: float

    def to_eng(self, raw: int) -> float:
        return raw * self.scale + self.offset

    def to_raw(self, value: float) -> int:
        raw = round((value - self.offset) / self.scale)
        return max(-32768, min(32767, raw))


CHANNELS: tuple[tuple[ChannelDef, ...], ...] = (
    # node 0 - environmental
    (
        ChannelDef("temp_c", "degC", 0.01, 0.0),
        ChannelDef("humidity", "%RH", 0.01, 0.0),
        ChannelDef("pressure", "kPa", 0.10, 0.0),
    ),
    # node 1 - power
    (
        ChannelDef("batt_v", "V", 0.001, 0.0),
        ChannelDef("current_a", "A", 0.001, 0.0),
        ChannelDef("power_w", "W", 0.10, 0.0),
    ),
    # node 2 - motion
    (
        ChannelDef("accel_x", "g", 0.001, 0.0),
        ChannelDef("accel_y", "g", 0.001, 0.0),
        ChannelDef("accel_z", "g", 0.001, 0.0),
    ),
)

NODE_LABELS = ("Environmental", "Power", "Motion")

# --------------------------------------------------------------------
# CRC-16/CCITT-FALSE
# --------------------------------------------------------------------


def _build_table() -> list[int]:
    table = []
    for byte in range(256):
        crc = byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
        table.append(crc)
    return table


_CRC_TABLE = _build_table()


def crc16(data: bytes) -> int:
    """poly 0x1021, init 0xFFFF, no reflection, no final xor. crc16(b"123456789") == 0x29B1."""
    crc = 0xFFFF
    for byte in data:
        crc = ((crc << 8) & 0xFFFF) ^ _CRC_TABLE[((crc >> 8) ^ byte) & 0xFF]
    return crc


# --------------------------------------------------------------------
# Data model
# --------------------------------------------------------------------


@dataclass
class NodeRecord:
    node_id: int
    flags: int
    can_seq: int
    loss: int
    raw: tuple[int, int, int]

    @property
    def online(self) -> bool:
        return bool(self.flags & NF_ONLINE)

    @property
    def stale(self) -> bool:
        return bool(self.flags & NF_STALE)

    @property
    def fault(self) -> bool:
        return bool(self.flags & NF_FAULT)

    def values(self) -> dict[str, float]:
        """Raw counts converted to engineering units, keyed by channel name."""
        defs = CHANNELS[self.node_id]
        return {d.name: d.to_eng(r) for d, r in zip(defs, self.raw)}

    def to_dict(self) -> dict:
        defs = CHANNELS[self.node_id]
        return {
            "node_id": self.node_id,
            "label": NODE_LABELS[self.node_id],
            "online": self.online,
            "stale": self.stale,
            "fault": self.fault,
            "can_seq": self.can_seq,
            "loss": self.loss,
            "channels": [
                {
                    "name": d.name,
                    "unit": d.unit,
                    "value": d.to_eng(r),
                    "raw": r,
                }
                for d, r in zip(defs, self.raw)
            ],
        }


@dataclass
class Frame:
    seq: int
    t_ms: int
    nodes: list[NodeRecord] = field(default_factory=list)

    def to_dict(self) -> dict:
        return {
            "seq": self.seq,
            "t_ms": self.t_ms,
            "nodes": [n.to_dict() for n in self.nodes],
        }


# --------------------------------------------------------------------
# Encode / decode
# --------------------------------------------------------------------

class DecodeError(ValueError):
    pass


_REC = struct.Struct("<BBBBhhh")
_HDR = struct.Struct("<BBBBHI")

# The 8-byte CAN payload, mirroring tlm_can_pack/tlm_can_unpack in
# telemetry_proto.c. The ground station never sees raw CAN frames - the hub
# aggregates them - but the DBC generator and the test suite do, and having one
# definition here keeps protocol/telemetry.dbc honest.
_CAN = struct.Struct("<hhhBB")


def encode_can(channels: tuple[int, int, int], seq: int, status: int = 0) -> bytes:
    """Build the 8 data bytes of a NODE_DATA frame."""
    return _CAN.pack(*channels, seq & 0xFF, status & 0xFF)


def decode_can(data: bytes) -> tuple[tuple[int, int, int], int, int]:
    """Inverse of encode_can. Returns ((ch0, ch1, ch2), seq, status)."""
    if len(data) != CAN_DLC:
        raise DecodeError(f"CAN payload must be {CAN_DLC} bytes, got {len(data)}")
    c0, c1, c2, seq, status = _CAN.unpack(data)
    return (c0, c1, c2), seq, status


def encode_frame(frame: Frame) -> bytes:
    body = bytearray(
        _HDR.pack(SYNC0, SYNC1, PROTO_VERSION, PAYLOAD_SIZE, frame.seq & 0xFFFF,
                  frame.t_ms & 0xFFFFFFFF)
    )

    for rec in frame.nodes:
        body += _REC.pack(
            rec.node_id & 0xFF,
            rec.flags & 0xFF,
            rec.can_seq & 0xFF,
            rec.loss & 0xFF,
            *rec.raw,
        )

    body += struct.pack("<H", crc16(bytes(body[OFF_VER:])))
    return bytes(body)


def decode_frame(buf: bytes) -> Frame:
    """Parse exactly one frame from the start of ``buf``. Raises DecodeError."""
    if len(buf) < FRAME_SIZE:
        raise DecodeError("short buffer")

    s0, s1, ver, length, seq, t_ms = _HDR.unpack_from(buf, 0)

    if s0 != SYNC0 or s1 != SYNC1:
        raise DecodeError("bad sync")
    if ver != PROTO_VERSION:
        raise DecodeError(f"unsupported version 0x{ver:02X}")
    if length != PAYLOAD_SIZE:
        raise DecodeError(f"bad payload length {length}")

    want = crc16(buf[OFF_VER:FRAME_SIZE - CRC_SIZE])
    got = struct.unpack_from("<H", buf, FRAME_SIZE - CRC_SIZE)[0]
    if want != got:
        raise DecodeError(f"crc mismatch: computed {want:#06x}, frame says {got:#06x}")

    nodes = []
    for i in range(NODE_COUNT):
        off = OFF_PAYLOAD + i * REC_SIZE
        node_id, flags, can_seq, loss, c0, c1, c2 = _REC.unpack_from(buf, off)
        # Guard against a node_id that would index outside CHANNELS.
        if node_id >= NODE_COUNT:
            raise DecodeError(f"node_id {node_id} out of range")
        nodes.append(NodeRecord(node_id, flags, can_seq, loss, (c0, c1, c2)))

    return Frame(seq=seq, t_ms=t_ms, nodes=nodes)


class StreamParser:
    """
    Resynchronising parser for a raw byte stream.

    Radio links drop bytes, and USB serial can hand you a frame split across
    two reads. Feed whatever arrives; complete, CRC-valid frames come back out.

    A false lock on the sync pattern appearing inside a payload is possible but
    is then rejected by the CRC, so the parser simply advances one byte and
    tries again.
    """

    def __init__(self, max_buffer: int = 4096) -> None:
        self._buf = bytearray()
        self._max = max_buffer
        self.frames_ok = 0
        self.crc_errors = 0
        self.bytes_discarded = 0

    def feed(self, data: bytes) -> Iterator[Frame]:
        self._buf += data

        # Bound memory if we are fed garbage that never contains a valid frame.
        if len(self._buf) > self._max:
            excess = len(self._buf) - self._max
            del self._buf[:excess]
            self.bytes_discarded += excess

        while True:
            start = self._buf.find(SYNC)

            if start < 0:
                # Keep the last byte: it might be the first half of a sync pair
                # split across two reads.
                if len(self._buf) > 1:
                    self.bytes_discarded += len(self._buf) - 1
                    del self._buf[:-1]
                return

            if start > 0:
                self.bytes_discarded += start
                del self._buf[:start]

            if len(self._buf) < FRAME_SIZE:
                return  # wait for more bytes

            try:
                frame = decode_frame(bytes(self._buf[:FRAME_SIZE]))
            except DecodeError:
                self.crc_errors += 1
                # Skip past this sync pair and keep hunting.
                self.bytes_discarded += 1
                del self._buf[:1]
                continue

            del self._buf[:FRAME_SIZE]
            self.frames_ok += 1
            yield frame
