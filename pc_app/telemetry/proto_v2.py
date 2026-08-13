"""
Python mirror of protocol/telemetry_proto_v2.h / .c.

Every constant, offset and scale factor here must match the C source exactly.
tools/test_roundtrip_v2.py compiles the C encoder (via tools/dump_frames_v2.c)
and compares its output with this module byte for byte, the same way
tools/test_roundtrip.py holds proto.py to telemetry_proto.c.

Two things make v2 harder to mirror than v1 (see proto.py):

1. Per-node record width is NOT fixed. v1 is a flat 3 x int16 record for every
   node; v2 has 9/5/5/5 channels of MIXED int16/int32 width. The offset of
   channel c within a node's record - and the offset of node n within the
   payload - are both the running sum of the widths that came before them,
   not a multiple of some constant record size. Get that sum wrong by one
   channel and every byte after it silently decodes as something else.

2. On the wire, which channel-width table decides a node's byte layout is the
   node's POSITION in the frame (0..NODE_COUNT-1), not the node_id byte
   stored in its own header. Look at tlm2_encode_frame/tlm2_decode_frame in
   telemetry_proto_v2.c: both index TLM2_CHANNELS[i] with the loop counter
   i, never with r->node_id. node_id is carried as data, not used to steer
   parsing. This module reproduces that deliberately - decode_frame() below
   walks the payload using range(NODE_COUNT) and CHANNELS[i], and only reads
   node_id out of the bytes for display. A decoder that "helpfully" looked up
   CHANNELS[node_id] instead would happen to work on every honest frame and
   silently desync on a corrupted or hand-crafted one.

CAN-layer paging (tlm2_can_pack_pages / tlm2_reasm_apply_page /
tlm2_reasm_snapshot) is mirrored too, even though the ground station only
ever sees the already-reassembled radio frame - never raw CAN traffic (same
situation v1 documents for encode_can/decode_can in proto.py). It earns its
keep here because it is the only way to exercise NF_PARTIAL and epoch-wrap
loss counting from Python, and because a future bus-sniffing tool or DBC
generator for v2 would otherwise have to reinvent it.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field
from typing import Iterator, NamedTuple, Sequence

# --------------------------------------------------------------------
# Versioning and topology
# --------------------------------------------------------------------

PROTO_VERSION = 0x02

NODE_COUNT = 4
MAX_CH_PER_NODE = 9   # RAM/wire upper bound only - see TLM2_MAX_CH_PER_NODE

# --------------------------------------------------------------------
# CAN layer
# --------------------------------------------------------------------

CAN_BITRATE = 500_000
CAN_DATA_BASE_ID = 0x200
CAN_NODE_SHIFT = 3
CAN_PAGE_MASK = 0x07
CAN_DLC = 8
PAGE_PAYLOAD_SIZE = 6          # 8 - epoch byte - status byte
MAX_PAGES_PER_NODE = 4
NODE_TX_PERIOD_MS = 100
NODE_TIMEOUT_MS = 1500


def can_id_for(node: int, page: int) -> int:
    return CAN_DATA_BASE_ID + (node << CAN_NODE_SHIFT) + (page & CAN_PAGE_MASK)


def can_node_from_id(can_id: int) -> int:
    return (can_id - CAN_DATA_BASE_ID) >> CAN_NODE_SHIFT


def can_page_from_id(can_id: int) -> int:
    return (can_id - CAN_DATA_BASE_ID) & CAN_PAGE_MASK


def can_id_is_node(can_id: int) -> bool:
    return CAN_DATA_BASE_ID <= can_id < CAN_DATA_BASE_ID + (NODE_COUNT << CAN_NODE_SHIFT)


ST_OK = 0x00
ST_SENSOR_FAULT = 0x01
ST_STARTUP = 0x02
ST_CAN_ERROR = 0x04

# --------------------------------------------------------------------
# Radio/USB-serial frame layout
# --------------------------------------------------------------------

SYNC0 = 0xA5
SYNC1 = 0x5A
SYNC = bytes((SYNC0, SYNC1))

FRAME_PERIOD_MS = 500

HDR_SIZE = 10
CRC_SIZE = 2
REC_HDR_SIZE = 4   # node_id, flags, epoch, loss

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
NF_PARTIAL = 0x08   # new in v2: current burst was incomplete at snapshot time

NODE_TIMEOUT_MS_RADIO = NODE_TIMEOUT_MS  # same constant, kept as an alias for readability

W_I16 = 2
W_I32 = 4


def _f32(x: float) -> float:
    """
    Round a Python (double) literal to the nearest float32 and hand back the
    double that exactly represents it.

    The C channel table stores scale/offset as `float`. If this module used
    the double literal 1e-7 directly, `raw * scale` would drift from what the
    C side computes (which promotes its float32 scale to double first) by
    about 1 part in 2^24 - harmless for a dashboard, but it means a "does
    Python match C bit-for-bit" test would have to carry a fudge factor
    instead of an exact bound. Rounding through float32 once, here, removes
    that ambiguity.
    """
    return struct.unpack("<f", struct.pack("<f", x))[0]


# --------------------------------------------------------------------
# Channel table (keep in step with TLM2_CHANNELS in telemetry_proto_v2.c)
# --------------------------------------------------------------------


class ChannelDef(NamedTuple):
    name: str
    unit: str
    scale: float
    offset: float
    width: int   # W_I16 or W_I32, in bytes

    def to_eng(self, raw: int) -> float:
        return raw * self.scale + self.offset

    def to_raw(self, value: float) -> int:
        raw = round((value - self.offset) / self.scale)
        if self.width == W_I32:
            return max(-2_147_483_648, min(2_147_483_647, raw))
        return max(-32768, min(32767, raw))


def _chan(name: str, unit: str, scale: float, offset: float, width: int) -> ChannelDef:
    return ChannelDef(name, unit, _f32(scale), _f32(offset), width)


CHANNELS: tuple[tuple[ChannelDef, ...], ...] = (
    # node 0 - Hub (GPS x5 + IMU x4). gps_lat/gps_lon are the only I32
    # channels - see "GPS precision" in V2_DESIGN_NOTES.md.
    (
        _chan("gps_lat", "deg", 1e-7, 0.0, W_I32),
        _chan("gps_lon", "deg", 1e-7, 0.0, W_I32),
        _chan("gps_speed", "mph", 0.01, 0.0, W_I16),
        _chan("gps_heading", "deg", 0.02, 0.0, W_I16),
        _chan("gps_sats", "count", 1.0, 0.0, W_I16),
        _chan("accel_x", "g", 0.001, 0.0, W_I16),
        _chan("accel_y", "g", 0.001, 0.0, W_I16),
        _chan("accel_z", "g", 0.001, 0.0, W_I16),
        _chan("gyro_z", "deg/s", 0.01, 0.0, W_I16),
    ),
    # node 1 - Front
    (
        _chan("wheel_speed_fl", "mph", 0.01, 0.0, W_I16),
        _chan("wheel_speed_fr", "mph", 0.01, 0.0, W_I16),
        _chan("suspension_fl", "mm", 0.01, 0.0, W_I16),
        _chan("suspension_fr", "mm", 0.01, 0.0, W_I16),
        _chan("brake_pressure_f", "psi", 0.1, 0.0, W_I16),
    ),
    # node 2 - Rear
    (
        _chan("wheel_speed_rl", "mph", 0.01, 0.0, W_I16),
        _chan("wheel_speed_rr", "mph", 0.01, 0.0, W_I16),
        _chan("suspension_rl", "mm", 0.01, 0.0, W_I16),
        _chan("suspension_rr", "mm", 0.01, 0.0, W_I16),
        _chan("cvt_temp", "degC", 0.01, 0.0, W_I16),
    ),
    # node 3 - E-CVT / Motor
    (
        _chan("motor_current", "A", 0.01, 0.0, W_I16),
        _chan("motor_velocity", "rpm", 1.0, 0.0, W_I16),
        _chan("motor_temp", "degC", 0.01, 0.0, W_I16),
        _chan("bus_voltage", "V", 0.01, 0.0, W_I16),
        _chan("brake_resistor_w", "W", 0.01, 0.0, W_I16),
    ),
)

NODE_LABELS = ("Hub - GPS/IMU", "Front", "Rear", "E-CVT / Motor")

CHAN_COUNT: tuple[int, ...] = tuple(len(node_chans) for node_chans in CHANNELS)

# --------------------------------------------------------------------
# Page layout - pure function of CHANNELS, mirrors layout_node() in the .c
# file. Computed once at import time since CHANNELS never changes at
# runtime, same reasoning as v1's compile-time constants.
# --------------------------------------------------------------------


class ChanSlot(NamedTuple):
    page: int
    offset: int   # byte offset within the page's 6-byte payload area
    width: int


def _layout_node(node_id: int) -> tuple[ChanSlot, ...]:
    """
    Greedy, in declared order, never splits a channel across a page boundary -
    byte-for-byte port of layout_node() in telemetry_proto_v2.c. Both the CAN
    packer and the reassembler below call this (indirectly, via _LAYOUTS), so
    they cannot disagree with each other, exactly like the two C-side callers.
    """
    page = 0
    off = 0
    slots = []
    for d in CHANNELS[node_id]:
        w = d.width
        if off + w > PAGE_PAYLOAD_SIZE:
            page += 1
            off = 0
        slots.append(ChanSlot(page, off, w))
        off += w
    return tuple(slots)


_LAYOUTS: tuple[tuple[ChanSlot, ...], ...] = tuple(_layout_node(n) for n in range(NODE_COUNT))

PAGE_COUNT: tuple[int, ...] = tuple(
    (_LAYOUTS[n][-1].page + 1) if _LAYOUTS[n] else 0 for n in range(NODE_COUNT)
)


def node_page_count(node_id: int) -> int:
    return PAGE_COUNT[node_id]


def node_wire_bytes(node_id: int) -> int:
    return sum(d.width for d in CHANNELS[node_id])


WIRE_BYTES: tuple[int, ...] = tuple(node_wire_bytes(n) for n in range(NODE_COUNT))

# --------------------------------------------------------------------
# Frame size - computed exactly like compute_payload_size() in the .c file,
# since v2 has no single fixed PAYLOAD_SIZE constant the way v1 does.
# --------------------------------------------------------------------

PAYLOAD_SIZE = sum(REC_HDR_SIZE + WIRE_BYTES[n] for n in range(NODE_COUNT))
FRAME_SIZE = HDR_SIZE + PAYLOAD_SIZE + CRC_SIZE

# --------------------------------------------------------------------
# Little-endian, signed-safe pack/unpack helpers
# --------------------------------------------------------------------
# struct's 'h'/'i' format codes are inherently signed and little-endian ('<'),
# which sidesteps the classic int.from_bytes(..., signed=True) foot-gun the
# hard way - by never having an unsigned path to forget signed= on.


def _trunc_i16(v: int) -> int:
    """Mirror C's `(int16_t)v` truncation (wrap, not clamp) for an out-of-range raw."""
    v &= 0xFFFF
    return v - 0x1_0000 if v >= 0x8000 else v


def _trunc_i32(v: int) -> int:
    v &= 0xFFFF_FFFF
    return v - 0x1_0000_0000 if v >= 0x8000_0000 else v


# --------------------------------------------------------------------
# CRC-16/CCITT-FALSE
# --------------------------------------------------------------------
# Duplicated rather than imported from proto.py - same reasoning as
# tlm2_crc16 being reimplemented instead of shared with tlm_crc16 in the C
# source: this file should be deletable-independent of v1, and vice versa.


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
    epoch: int      # last_complete_epoch at snapshot time - v2's analogue of v1's can_seq
    loss: int        # missed/torn bursts since the previous snapshot, sat 255
    raw: tuple[int, ...]   # length == CHAN_COUNT[node_id]

    @property
    def online(self) -> bool:
        return bool(self.flags & NF_ONLINE)

    @property
    def stale(self) -> bool:
        return bool(self.flags & NF_STALE)

    @property
    def fault(self) -> bool:
        return bool(self.flags & NF_FAULT)

    @property
    def partial(self) -> bool:
        """Burst in flight at snapshot time never finished - raw is the last
        COMPLETE set, never torn data, just not this window's numbers."""
        return bool(self.flags & NF_PARTIAL)

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
            "partial": self.partial,
            "epoch": self.epoch,
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
# CAN page pack (mirrors tlm2_can_pack_pages)
# --------------------------------------------------------------------


def can_pack_pages(node_id: int, raw: Sequence[int], epoch: int, status: int) -> list[bytes]:
    """Pack one node's full channel set into its CAN pages. len(raw) must equal
    CHAN_COUNT[node_id]. Returns PAGE_COUNT[node_id] 8-byte page payloads."""
    defs = CHANNELS[node_id]
    if len(raw) != len(defs):
        raise ValueError(f"node {node_id} expects {len(defs)} channels, got {len(raw)}")

    slots = _LAYOUTS[node_id]
    pages = [bytearray(CAN_DLC) for _ in range(PAGE_COUNT[node_id])]
    for p in pages:
        p[0] = epoch & 0xFF
        p[1] = status & 0xFF
        # bytes [2:8] start zeroed by bytearray(CAN_DLC) already - matches the
        # C side's explicit zero-fill of unused tail bytes.

    for d, r, slot in zip(defs, raw, slots):
        dst = 2 + slot.offset
        if slot.width == W_I32:
            struct.pack_into("<i", pages[slot.page], dst, _trunc_i32(r))
        else:
            struct.pack_into("<h", pages[slot.page], dst, _trunc_i16(r))

    return [bytes(p) for p in pages]


# --------------------------------------------------------------------
# Hub-side reassembly (mirrors tlm2_node_reassembler_t / tlm2_reasm_*)
# --------------------------------------------------------------------


class NodeReassembler:
    """One of these per node. ch holds the last COMPLETE snapshot only -
    apply_page() builds into ch_building and only publishes into ch once every
    page of the current epoch has arrived, so a reader of ch never sees a
    torn mix of old and new values. Same contract as the C struct."""

    def __init__(self, node_id: int) -> None:
        self.node_id = node_id
        n = CHAN_COUNT[node_id]
        self.ch: list[int] = [0] * n
        self.ch_building: list[int] = [0] * n

        self.page_mask = 0
        self.cur_epoch = 0
        self.status = 0

        self.last_complete_epoch = 0
        self.have_epoch = False
        self.have_complete = False

        self.last_rx_ms = 0
        self.loss_accum = 0
        self.seen_this_window = False

    def apply_page(self, page_idx: int, d8: bytes, now_ms: int) -> bool:
        """Feed one received CAN page. Returns True iff this page completed
        the set for its epoch (ch was just published with a fresh snapshot)."""
        page_count = PAGE_COUNT[self.node_id]
        if page_count == 0 or page_idx >= page_count:
            return False   # bogus node/page - caller's ID decode is wrong, or noise

        epoch = d8[0]
        status = d8[1]

        if not self.have_epoch or epoch != self.cur_epoch:
            # Fresh burst. Anything already in ch_building for the OLD epoch
            # that never reached completeness is discarded right here - the
            # torn-set case. self.ch (last COMPLETE snapshot) is untouched.
            self.cur_epoch = epoch
            self.page_mask = 0
            self.have_epoch = True

        self.status = status
        self.page_mask |= (1 << page_idx)
        self.last_rx_ms = now_ms
        self.seen_this_window = True

        for i, slot in enumerate(_LAYOUTS[self.node_id]):
            if slot.page != page_idx:
                continue
            src = 2 + slot.offset
            if slot.width == W_I32:
                (v,) = struct.unpack_from("<i", d8, src)
            else:
                (v,) = struct.unpack_from("<h", d8, src)
            self.ch_building[i] = v

        full_mask = (1 << page_count) - 1
        if (self.page_mask & full_mask) != full_mask:
            return False   # still waiting on at least one more page

        # Complete. Loss is measured in bursts (epoch gaps), same unsigned-wrap
        # trick v1 uses on seq: (epoch - last_epoch) & 0xFF.
        if self.have_complete:
            delta = (epoch - self.last_complete_epoch) & 0xFF
            if delta > 1:
                self.loss_accum += delta - 1

        self.ch = list(self.ch_building)
        self.last_complete_epoch = epoch
        self.have_complete = True
        return True

    def snapshot(self, now_ms: int) -> NodeRecord:
        """Build this node's NodeRecord from current state and reset the
        per-window counters. Call once per FRAME_PERIOD_MS, like v1's
        build_frame() draining node_state_t."""
        flags = 0

        if self.seen_this_window:
            flags |= NF_ONLINE

        if not self.have_complete or ((now_ms - self.last_rx_ms) & 0xFFFF_FFFF) > NODE_TIMEOUT_MS:
            flags |= NF_STALE
            flags &= (~NF_ONLINE) & 0xFF

        if self.status & ST_SENSOR_FAULT:
            flags |= NF_FAULT

        page_count = PAGE_COUNT[self.node_id]
        full_mask = (1 << page_count) - 1
        if self.have_epoch and (self.page_mask & full_mask) != full_mask:
            flags |= NF_PARTIAL

        raw = tuple(self.ch) if self.have_complete else tuple(0 for _ in self.ch)

        rec = NodeRecord(
            node_id=self.node_id,
            flags=flags,
            epoch=self.last_complete_epoch,
            loss=min(self.loss_accum, 255),
            raw=raw,
        )

        self.loss_accum = 0
        self.seen_this_window = False
        return rec


# --------------------------------------------------------------------
# Radio frame encode / decode
# --------------------------------------------------------------------


class DecodeError(ValueError):
    pass


_HDR = struct.Struct("<BBBBHI")


def encode_frame(frame: Frame) -> bytes:
    """frame.nodes must have exactly NODE_COUNT entries, in node-index order
    (frame.nodes[i] is node i) - same positional assumption tlm2_encode_frame
    makes by indexing TLM2_CHANNELS[i], not TLM2_CHANNELS[node_id]."""
    body = bytearray(
        _HDR.pack(SYNC0, SYNC1, PROTO_VERSION, PAYLOAD_SIZE & 0xFF,
                  frame.seq & 0xFFFF, frame.t_ms & 0xFFFFFFFF)
    )

    for i in range(NODE_COUNT):
        rec = frame.nodes[i]
        body += struct.pack("<BBBB", rec.node_id & 0xFF, rec.flags & 0xFF,
                             rec.epoch & 0xFF, rec.loss & 0xFF)

        for d, raw in zip(CHANNELS[i], rec.raw):
            if d.width == W_I32:
                body += struct.pack("<i", _trunc_i32(raw))
            else:
                body += struct.pack("<h", _trunc_i16(raw))

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
    if length != (PAYLOAD_SIZE & 0xFF):
        raise DecodeError(f"bad payload length {length}")

    want = crc16(buf[OFF_VER:FRAME_SIZE - CRC_SIZE])
    got = struct.unpack_from("<H", buf, FRAME_SIZE - CRC_SIZE)[0]
    if want != got:
        raise DecodeError(f"crc mismatch: computed {want:#06x}, frame says {got:#06x}")

    nodes = []
    off = OFF_PAYLOAD
    for i in range(NODE_COUNT):
        node_id, flags, epoch, loss = struct.unpack_from("<BBBB", buf, off)
        off += REC_HDR_SIZE

        # Position i (not the node_id byte) selects the width table - see the
        # module docstring. node_id is carried through untouched for display.
        raw = []
        for d in CHANNELS[i]:
            if d.width == W_I32:
                (v,) = struct.unpack_from("<i", buf, off)
                off += 4
            else:
                (v,) = struct.unpack_from("<h", buf, off)
                off += 2
            raw.append(v)

        nodes.append(NodeRecord(node_id, flags, epoch, loss, tuple(raw)))

    return Frame(seq=seq, t_ms=t_ms, nodes=nodes)


class StreamParser:
    """
    Resynchronising parser for a raw byte stream. Identical design to
    proto.StreamParser - see that class's docstring - just pointed at v2's
    SYNC/FRAME_SIZE/decode_frame. v1 and v2 share the same sync bytes
    (0xA5 0x5A); the version byte at offset 2 is what actually tells them
    apart, so a mixed-fleet stream needs the caller to try both parsers (or
    branch on buf[OFF_VER] after peeking) - this class alone will just reject
    a v1 frame as "unsupported version" and resync past it, which is safe but
    not automatic demuxing.
    """

    def __init__(self, max_buffer: int = 4096) -> None:
        self._buf = bytearray()
        self._max = max_buffer
        self.frames_ok = 0
        self.crc_errors = 0
        self.bytes_discarded = 0

    def feed(self, data: bytes) -> Iterator[Frame]:
        self._buf += data

        if len(self._buf) > self._max:
            excess = len(self._buf) - self._max
            del self._buf[:excess]
            self.bytes_discarded += excess

        while True:
            start = self._buf.find(SYNC)

            if start < 0:
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
                self.bytes_discarded += 1
                del self._buf[:1]
                continue

            del self._buf[:FRAME_SIZE]
            self.frames_ok += 1
            yield frame
