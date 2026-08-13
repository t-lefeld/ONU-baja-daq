#!/usr/bin/env python3
"""
Generate protocol/telemetry_v2.dbc from the v2 channel tables.

    python tools/gen_dbc_v2.py

Same motivation as tools/gen_dbc.py (see that file's docstring for the case
for a DBC at all), but v2's multi-frame ("paged") layout makes a generated
DBC more valuable, not less: a node's channels are split across up to
TLM2_MAX_PAGES_PER_NODE separate CAN frames by a greedy packing rule that
lives only in code (protocol/telemetry_proto_v2.c), so hand-decoding a page
on a bus trace means re-deriving that packing in your head, per page, for
four nodes. A DBC does that once and lets SavvyCAN/cantools/etc. do it for
every frame after.

There is no proto_v2.py yet (see V2_DESIGN_NOTES.md, "No proto_v2.py yet"),
so unlike gen_dbc.py this cannot import a Python mirror of the channel
table. Instead it parses protocol/telemetry_proto_v2.h and .c directly with
regexes and re-implements the greedy page-layout walk (layout_node() in the
.c file) in Python. That keeps the "generated, cannot drift" guarantee
gen_dbc.py relies on: if TLM2_CHANNELS changes, this file's output changes
on the next run without anyone touching this script by hand.

Each page is its own CAN ID and its own BO_ message - a DBC message maps to
one CAN frame, and v2 nodes send several frames per transmit cycle. That is
not a workaround, it's what v2's own header comment says: the page index
lives in the CAN ID specifically so a bus analyzer can filter and decode
"give me everything from node N" one frame/message at a time.
"""

from __future__ import annotations

import re
import sys
from datetime import datetime
from decimal import Decimal
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PROTO_H = ROOT / "protocol" / "telemetry_proto_v2.h"
PROTO_C = ROOT / "protocol" / "telemetry_proto_v2.c"
OUT = ROOT / "protocol" / "telemetry_v2.dbc"

RECEIVER = "HUB"

# tlm2_width_t values, from the enum in telemetry_proto_v2.h:
#   TLM2_W_I16 = 2, TLM2_W_I32 = 4   (bytes, not bits - this is the byte
# width used both as the C struct field and as the table constant name).
# Hardcoded rather than parsed: these are the enum's own literal values, and
# a change here would mean a new width tier, which would need a matching
# code change to this generator regardless of how the constant is read.
WIDTH_BYTES = {"TLM2_W_I16": 2, "TLM2_W_I32": 4}

# Short, DBC-safe node identifiers (letters/digits/underscore only - the
# real TLM2_NODE_NAMES strings contain spaces, dashes and slashes, e.g.
# "Hub - GPS/IMU" and "E-CVT / Motor", so they can't be used as BU_ names
# directly; same reasoning as v1's NODE_NAMES in gen_dbc.py).
NODE_IDENT = ["NODE0_HUB", "NODE1_FRONT", "NODE2_REAR", "NODE3_MOTOR"]


# ---------------------------------------------------------------------
# Parsing protocol/telemetry_proto_v2.h / .c
# ---------------------------------------------------------------------

def parse_header_int(text: str, name: str) -> int:
    """Pull a #define NAME <int-literal>u style constant out of the header."""
    m = re.search(rf'#define\s+{name}\s+(0[xX][0-9A-Fa-f]+|\d+)u?\b', text)
    if not m:
        raise ValueError(f"could not find #define {name} in {PROTO_H}")
    return int(m.group(1), 0)


def parse_status_bits(text: str) -> list[tuple[int, str, str]]:
    """
    TLM2_ST_* bitfield -> [(bit_index, snake_case_name, description), ...],
    skipping TLM2_ST_OK (value 0, not a bit). Mirrors STATUS_BITS in
    gen_dbc.py, but derived from the header text instead of hand-copied so a
    new status bit shows up here automatically.
    """
    bits = []
    pattern = re.compile(
        r'#define\s+TLM2_ST_(\w+)\s+(0[xX][0-9A-Fa-f]+)u\s*(?:/\*(.*?)\*/)?'
    )
    for name, value_str, comment in pattern.findall(text):
        value = int(value_str, 16)
        if value == 0:
            continue  # OK / no-fault sentinel, not a real bit
        bit = value.bit_length() - 1
        if (1 << bit) != value:
            raise ValueError(f"TLM2_ST_{name} = {value_str} is not a single bit")
        bits.append((bit, name.lower(), comment.strip() if comment else name))
    bits.sort(key=lambda b: b[0])
    return bits


# Numbers here can be plain (0.01, 0.0) or scientific with a SIGNED exponent
# (1e-7 for gps_lat/lon) - the float regex must allow a +/- after the e/E, not
# just before the whole number, or entries like "1e-7f" silently fail to
# match and the channel is dropped without any error (caught in review by
# diffing page counts against V2_DESIGN_NOTES.md's table - do not regress).
FLOAT_RE = r'[+-]?[0-9]*\.?[0-9]+(?:[eE][+-]?[0-9]+)?'
CHAN_ENTRY_RE = re.compile(
    r'\{\s*"([^"]+)"\s*,\s*"([^"]*)"\s*,\s*'
    rf'({FLOAT_RE})f\s*,\s*({FLOAT_RE})f\s*,\s*'
    r'(TLM2_W_I16|TLM2_W_I32)\s*\}'
)


def parse_channel_array(text: str, array_name: str) -> list[dict]:
    """Extract one `static const tlm2_chan_def_t NAME[] = { ... };` block."""
    m = re.search(
        rf'static\s+const\s+tlm2_chan_def_t\s+{array_name}\[\]\s*=\s*\{{(.*?)\}};',
        text, re.DOTALL,
    )
    if not m:
        raise ValueError(f"could not find channel array {array_name} in {PROTO_C}")

    chans = []
    for name, unit, scale, offset, width_name in CHAN_ENTRY_RE.findall(m.group(1)):
        chans.append({
            "name": name,
            "unit": unit,
            "scale": float(scale),
            "offset": float(offset),
            "width": WIDTH_BYTES[width_name],
        })
    if not chans:
        raise ValueError(f"channel array {array_name} parsed empty - regex out of sync?")
    return chans


def parse_node_table_order(text: str) -> list[str]:
    """
    The TLM2_CHANNELS[] initializer list gives the authoritative node order
    (array-name per node index), independent of the order the arrays happen
    to be declared in above it.
    """
    m = re.search(
        r'const\s+tlm2_chan_def_t\s+\*const\s+TLM2_CHANNELS\[TLM2_NODE_COUNT\]\s*=\s*\{(.*?)\};',
        text, re.DOTALL,
    )
    if not m:
        raise ValueError("could not find TLM2_CHANNELS[] initializer")
    names = [n.strip() for n in m.group(1).split(",")]
    return [n for n in names if n]


def parse_node_names(text: str) -> list[str]:
    m = re.search(
        r'const\s+char\s+\*const\s+TLM2_NODE_NAMES\[TLM2_NODE_COUNT\]\s*=\s*\{(.*?)\};',
        text, re.DOTALL,
    )
    if not m:
        raise ValueError("could not find TLM2_NODE_NAMES[] initializer")
    return re.findall(r'"([^"]*)"', m.group(1))


# ---------------------------------------------------------------------
# Page layout - Python re-implementation of layout_node() in the .c file
# ---------------------------------------------------------------------

def layout_node(chans: list[dict], page_payload_size: int) -> tuple[list[tuple[int, int]], int]:
    """
    Greedy, in declared order, never splits a channel across a page - the
    exact algorithm in telemetry_proto_v2.c's layout_node(). Returns
    ([(page, offset) per channel], page_count).
    """
    slots = []
    page = 0
    off = 0
    for ch in chans:
        w = ch["width"]
        if off + w > page_payload_size:
            page += 1
            off = 0
        slots.append((page, off))
        off += w
    return slots, page + 1


# ---------------------------------------------------------------------
# DBC text generation
# ---------------------------------------------------------------------

def signal_line(name: str, start: int, length: int, signed: bool,
                 scale: float, offset: float, unit: str) -> str:
    """One SG_ entry - identical shape to gen_dbc.py's signal_line()."""
    span = (1 << length)
    if signed:
        lo = (-(span // 2)) * scale + offset
        hi = (span // 2 - 1) * scale + offset
    else:
        lo = offset
        hi = (span - 1) * scale + offset

    sign = "-" if signed else "+"

    def num(v: float) -> str:
        # gen_dbc.py's version of this helper truncates to 6 decimal places,
        # which is fine for v1's coarsest-need scale (0.001) but SILENTLY
        # ZEROES OUT v2's gps_lat/gps_lon scale (1e-7 -> "0.000000" -> "0"),
        # which would make those two signals decode to a constant 0 in any
        # DBC-driven tool - a wrong-but-plausible-looking failure, exactly
        # what this file's own docstring (via gen_dbc.py's) warns about.
        # repr() gives the shortest string that round-trips to the same
        # float; Decimal() expands any scientific notation ("1e-07") into a
        # plain decimal DBC parsers expect, at full precision.
        if v == 0:
            return "0"
        s = repr(v)
        if "e" in s or "E" in s:
            s = format(Decimal(str(v)), "f")
        if "." in s:
            s = s.rstrip("0").rstrip(".")
        return s if s else "0"

    return (f' SG_ {name} : {start}|{length}@1{sign} '
            f'({num(scale)},{num(offset)}) [{num(lo)}|{num(hi)}] "{unit}" {RECEIVER}')


def build() -> str:
    h_text = PROTO_H.read_text(encoding="utf-8")
    c_text = PROTO_C.read_text(encoding="utf-8")

    proto_version   = parse_header_int(h_text, "TLM2_PROTO_VERSION")
    can_base_id     = parse_header_int(h_text, "TLM2_CAN_DATA_BASE_ID")
    node_shift      = parse_header_int(h_text, "TLM2_CAN_NODE_SHIFT")
    dlc             = parse_header_int(h_text, "TLM2_CAN_DLC")
    page_payload    = parse_header_int(h_text, "TLM2_PAGE_PAYLOAD_SIZE")
    tx_period_ms    = parse_header_int(h_text, "TLM2_NODE_TX_PERIOD_MS")
    bitrate         = parse_header_int(h_text, "TLM2_CAN_BITRATE")
    status_bits     = parse_status_bits(h_text)

    array_order = parse_node_table_order(c_text)
    node_count = len(array_order)
    if node_count != len(NODE_IDENT):
        raise ValueError(
            f"TLM2_CHANNELS[] has {node_count} nodes but NODE_IDENT in this "
            f"script has {len(NODE_IDENT)} - update NODE_IDENT to match."
        )

    node_channels = [parse_channel_array(c_text, arr) for arr in array_order]
    node_labels = parse_node_names(c_text)

    # epoch is byte 0 (full byte, unsigned) - the multi-frame analogue of
    # v1's "seq", useful for spotting torn/duplicated bursts on a trace.
    # status lives in byte 1, broken into named bits rather than exposed as
    # one opaque byte, same reasoning as v1's STATUS_BITS in gen_dbc.py.
    L: list[str] = []

    L.append(f'VERSION "CAN-LoRa telemetry, protocol version {proto_version}"')
    L.append("")
    L.append("")

    L.append("NS_ :")
    for tag in ["NS_DESC_", "CM_", "BA_DEF_", "BA_", "VAL_", "CAT_DEF_", "CAT_",
                "FILTER", "BA_DEF_DEF_", "EV_DATA_", "ENVVAR_DATA_",
                "SGTYPE_", "SGTYPE_VAL_", "BA_DEF_SGTYPE_", "BA_SGTYPE_",
                "SIG_TYPE_REF_", "VAL_TABLE_", "SIG_GROUP_", "SIG_VALTYPE_",
                "SIGTYPE_VALTYPE_", "BO_TX_BU_", "BA_DEF_REL_", "BA_REL_",
                "BA_DEF_DEF_REL_", "BU_SG_REL_", "BU_EV_REL_", "BU_BO_REL_",
                "SG_MUL_VAL_"]:
        L.append(f"    {tag}")
    L.append("")
    L.append("BS_:")
    L.append("")

    L.append(f"BU_: {' '.join(NODE_IDENT)} {RECEIVER}")
    L.append("")

    # Precompute layouts so the messages, comments and attributes sections
    # below all walk the same (node, page) list in the same order.
    all_slots = []      # all_slots[node] = [(page, offset), ...] per channel
    all_pagecounts = []  # all_pagecounts[node] = page_count
    all_can_ids = []     # all_can_ids[node][page] = can_id

    for node in range(node_count):
        slots, page_count = layout_node(node_channels[node], page_payload)
        all_slots.append(slots)
        all_pagecounts.append(page_count)
        all_can_ids.append([can_base_id + (node << node_shift) + p
                             for p in range(page_count)])

    # ---- messages ----
    for node in range(node_count):
        chans = node_channels[node]
        slots = all_slots[node]
        page_count = all_pagecounts[node]

        for page in range(page_count):
            can_id = all_can_ids[node][page]
            msg = f"{NODE_IDENT[node]}_PAGE{page}"

            L.append(f"BO_ {can_id} {msg}: {dlc} {NODE_IDENT[node]}")

            L.append(signal_line("epoch", 0, 8, False, 1, 0, ""))
            for bit, name, _ in status_bits:
                L.append(signal_line(name, 8 + bit, 1, False, 1, 0, ""))

            for ch, (ch_page, ch_off) in zip(chans, slots):
                if ch_page != page:
                    continue
                start = (2 + ch_off) * 8
                length = ch["width"] * 8
                L.append(signal_line(ch["name"], start, length, True,
                                     ch["scale"], ch["offset"], ch["unit"]))

            L.append("")

    # ---- comments ----
    L.append(f'CM_ "Generated by tools/gen_dbc_v2.py on '
             f'{datetime.now():%Y-%m-%d}. Do not edit by hand - '
             f'edit TLM2_CHANNELS in protocol/telemetry_proto_v2.c and regenerate.";')

    for node in range(node_count):
        page_count = all_pagecounts[node]
        label = node_labels[node] if node < len(node_labels) else NODE_IDENT[node]
        L.append(f'CM_ BU_ {NODE_IDENT[node]} "{label}, sends {page_count} '
                 f'page(s) per burst";')

        for page in range(page_count):
            can_id = all_can_ids[node][page]
            L.append(f'CM_ BO_ {can_id} "{label} page {page} of {page_count}, '
                     f'part of a burst sent every {tx_period_ms} ms. All pages '
                     f'of a burst share the same epoch value.";')
            L.append(f'CM_ SG_ {can_id} epoch "Increments once per BURST (all '
                     f'pages of one transmit cycle share a value), not once '
                     f'per CAN frame. A gap here means a whole burst was '
                     f'lost or torn, not just this page.";')
            for bit, name, desc in status_bits:
                L.append(f'CM_ SG_ {can_id} {name} "{desc}";')

    L.append(f'CM_ BU_ {RECEIVER} "STM32L476RG, aggregates the bus and '
             f'uplinks snapshots over LoRa";')
    L.append("")

    # ---- attributes ----
    L.append('BA_DEF_ BO_  "GenMsgCycleTime" INT 0 65535;')
    L.append('BA_DEF_ "BusType" STRING ;')
    L.append('BA_DEF_DEF_  "GenMsgCycleTime" 0;')
    L.append('BA_DEF_DEF_  "BusType" "CAN";')
    L.append('BA_ "BusType" "CAN";')

    for node in range(node_count):
        for page in range(all_pagecounts[node]):
            can_id = all_can_ids[node][page]
            L.append(f'BA_ "GenMsgCycleTime" BO_ {can_id} {tx_period_ms};')

    L.append("")

    # ---- value descriptions ----
    for node in range(node_count):
        for page in range(all_pagecounts[node]):
            can_id = all_can_ids[node][page]
            for _, name, _ in status_bits:
                L.append(f'VAL_ {can_id} {name} 1 "yes" 0 "no";')

    L.append("")

    return "\n".join(L), {
        "node_count": node_count,
        "total_messages": sum(all_pagecounts),
        "bitrate": bitrate,
        "can_base_id": can_base_id,
        "max_id": max(max(ids) for ids in all_can_ids),
    }


def main() -> int:
    text, info = build()
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(text, encoding="utf-8")

    print(f"Wrote {OUT.relative_to(ROOT)}")
    print(f"  {info['node_count']} nodes, {info['total_messages']} messages (paged)")
    print(f"  IDs 0x{info['can_base_id']:03X}-0x{info['max_id']:03X}, "
          f"{info['bitrate'] // 1000} kbit/s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
