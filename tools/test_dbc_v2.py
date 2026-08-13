#!/usr/bin/env python3
"""
Validate protocol/telemetry_v2.dbc against the v2 firmware's own packing.

    python tools/test_dbc_v2.py

Same goal as tools/test_dbc.py for v1 - a DBC is only useful if a third-party
tool decodes the bus the same way the real code packs it - adapted for v2's
paged layout and one missing piece v1 has: there is no proto_v2.py yet (see
V2_DESIGN_NOTES.md, "No proto_v2.py yet"), so there's no Python reference
decoder to encode test payloads with, the way test_dbc.py uses proto.py.

Instead, this compiles a tiny throwaway C harness against the REAL,
unmodified telemetry_proto_v2.c and calls tlm2_can_pack_pages() directly -
the same function firmware calls - to get real page bytes for known raw
values, then checks that cantools decodes those bytes to the engineering
values the (independently regex-parsed) channel table says they should be.
That is what actually guards against the byte-order mistake test_dbc.py's
docstring calls out: get @1 vs @0 wrong and every signal still "decodes",
just to plausible-looking garbage.

Skips cleanly if cantools is not installed (pip install cantools).
"""

from __future__ import annotations

import random
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import gen_dbc_v2 as gen  # noqa: E402  (reuse its regex-based table parser)

DBC = ROOT / "protocol" / "telemetry_v2.dbc"
PROTO_C = ROOT / "protocol" / "telemetry_proto_v2.c"
PROTO_DIR = ROOT / "protocol"

failures: list[str] = []
checks = 0


def check(label: str, got, want) -> None:
    global checks
    checks += 1
    if got != want:
        failures.append(f"{label}\n     got  {got!r}\n     want {want!r}")


def close(label: str, got: float, want: float, tol: float) -> None:
    global checks
    checks += 1
    if abs(got - want) > tol:
        failures.append(f"{label}\n     got  {got!r}\n     want {want!r} (tol {tol})")


def ok(label: str, cond: bool) -> None:
    global checks
    checks += 1
    if not cond:
        failures.append(label)


# Reads "<node> <epoch> <status>" then <n> raw ints (n = TLM2_CHAN_COUNT[node])
# from stdin, one request per line-group, calls the real tlm2_can_pack_pages()
# and prints "<page_count> <hex bytes...>" - this is the ONLY C code in this
# file, and it does nothing telemetry_proto_v2.c itself doesn't already do.
HARNESS_SRC = r"""
#include "telemetry_proto_v2.h"
#include <stdio.h>

int main(void) {
    int node; unsigned epoch, status;
    while (scanf("%d %u %u", &node, &epoch, &status) == 3) {
        int n = TLM2_CHAN_COUNT[node];
        int32_t raw[TLM2_MAX_CH_PER_NODE];
        for (int i = 0; i < n; i++) {
            long v;
            if (scanf("%ld", &v) != 1) { return 2; }
            raw[i] = (int32_t)v;
        }
        uint8_t pages[TLM2_MAX_PAGES_PER_NODE][TLM2_CAN_DLC];
        uint8_t pc = tlm2_can_pack_pages((uint8_t)node, raw, (uint8_t)n,
                                          (uint8_t)epoch, (uint8_t)status, pages);
        printf("%u", pc);
        for (unsigned p = 0; p < pc; p++) {
            for (unsigned b = 0; b < TLM2_CAN_DLC; b++) {
                printf(" %02x", pages[p][b]);
            }
        }
        printf("\n");
    }
    return 0;
}
"""


def build_harness(tmpdir: Path):
    cc = next((c for c in ("gcc", "cc", "clang") if shutil.which(c)), None)
    if cc is None:
        return None

    src = tmpdir / "dbc_v2_harness.c"
    src.write_text(HARNESS_SRC, encoding="utf-8")
    exe = tmpdir / ("harness.exe" if sys.platform == "win32" else "harness")

    cmd = [cc, "-std=c99", "-O1", "-Wall", "-Wextra", "-Werror",
           f"-I{PROTO_DIR}", str(PROTO_C), str(src), "-lm", "-o", str(exe)]
    build = subprocess.run(cmd, capture_output=True, text=True)
    if build.returncode != 0:
        print("  harness failed to compile:\n")
        print(build.stdout)
        print(build.stderr)
        return None
    return exe


def main() -> int:
    print("Validating protocol/telemetry_v2.dbc\n")

    # Same drift guard as test_dbc.py: regenerate and confirm nothing moved.
    before = DBC.read_bytes() if DBC.exists() else b""
    subprocess.run([sys.executable, str(ROOT / "tools" / "gen_dbc_v2.py")],
                   capture_output=True, check=True)
    after = DBC.read_bytes()

    def strip_date(b: bytes) -> list[bytes]:
        return [l for l in b.splitlines() if not l.startswith(b'CM_ "Generated')]

    ok("DBC is up to date with the channel table", strip_date(before) == strip_date(after))

    try:
        import cantools
    except ImportError:
        print("  SKIP: cantools not installed (pip install cantools)")
        print(f"  {checks - len(failures)}/{checks} checks passed")
        return 1 if failures else 0

    db = cantools.database.load_file(DBC)

    # Re-derive the table and layout the same way gen_dbc_v2.py did. This
    # can't rubber-stamp a shared bug in the generator: the bytes it's
    # checked against below come from the real C packer, not from this
    # parser or the DBC file.
    h_text = gen.PROTO_H.read_text(encoding="utf-8")
    c_text = gen.PROTO_C.read_text(encoding="utf-8")
    array_order = gen.parse_node_table_order(c_text)
    node_channels = [gen.parse_channel_array(c_text, a) for a in array_order]
    node_count = len(array_order)
    page_payload = gen.parse_header_int(h_text, "TLM2_PAGE_PAYLOAD_SIZE")
    can_base_id = gen.parse_header_int(h_text, "TLM2_CAN_DATA_BASE_ID")
    node_shift = gen.parse_header_int(h_text, "TLM2_CAN_NODE_SHIFT")
    tx_period = gen.parse_header_int(h_text, "TLM2_NODE_TX_PERIOD_MS")

    all_slots = []       # all_slots[node] = [(page, offset), ...] per channel
    all_pagecounts = []
    for node in range(node_count):
        slots, pc = gen.layout_node(node_channels[node], page_payload)
        all_slots.append(slots)
        all_pagecounts.append(pc)

    total_pages = sum(all_pagecounts)

    # ---- structure ----
    check("message count", len(db.messages), total_pages)
    check("node count", len(db.nodes), node_count + 1)  # senders + HUB

    for node in range(node_count):
        for page in range(all_pagecounts[node]):
            can_id = can_base_id + (node << node_shift) + page
            msg = db.get_message_by_frame_id(can_id)

            check(f"node {node} page {page} DLC", msg.length, 8)
            check(f"node {node} page {page} is standard ID", msg.is_extended_frame, False)
            check(f"node {node} page {page} cycle time", msg.cycle_time, tx_period)

            expect_names = {"epoch", "sensor_fault", "startup", "can_error"}
            expect_names |= {ch["name"] for ch, (p, _) in
                             zip(node_channels[node], all_slots[node]) if p == page}
            names = {s.name for s in msg.signals}
            ok(f"node {node} page {page} signal set matches", names == expect_names)

            for ch, (p, _) in zip(node_channels[node], all_slots[node]):
                if p != page:
                    continue
                sig = msg.get_signal_by_name(ch["name"])
                close(f"node {node} {ch['name']} scale", sig.scale, ch["scale"],
                      abs(ch["scale"]) * 1e-6 + 1e-12)
                close(f"node {node} {ch['name']} offset", sig.offset, ch["offset"], 1e-9)
                check(f"node {node} {ch['name']} unit", sig.unit, ch["unit"])
                check(f"node {node} {ch['name']} is signed", sig.is_signed, True)
                check(f"node {node} {ch['name']} length", sig.length, ch["width"] * 8)
                check(f"node {node} {ch['name']} byte order", sig.byte_order, "little_endian")

    # ---- the real test: decode what tlm2_can_pack_pages() would actually send ----
    with tempfile.TemporaryDirectory() as td:
        exe = build_harness(Path(td))
        if exe is None:
            print("  SKIP: no host C compiler found (looked for gcc, cc, clang)")
            print(f"  {checks - len(failures)}/{checks} checks passed")
            return 1 if failures else 0

        random.seed(20260812)
        requests = []  # (node, epoch, status, raw[])
        for trial in range(200):
            node = trial % node_count
            raw = []
            for ch in node_channels[node]:
                if ch["width"] == 4:
                    raw.append(random.randint(-2_000_000_000, 2_000_000_000))
                else:
                    raw.append(random.randint(-32768, 32767))
            epoch = random.randint(0, 255)
            status = random.randint(0, 7)
            requests.append((node, epoch, status, raw))

        stdin_text = "".join(
            f"{node} {epoch} {status}\n" + " ".join(str(v) for v in raw) + "\n"
            for node, epoch, status, raw in requests
        )

        run = subprocess.run([str(exe)], input=stdin_text, capture_output=True, text=True)
        ok("harness ran cleanly", run.returncode == 0)
        out_lines = run.stdout.strip().splitlines()
        check("harness produced one line per request", len(out_lines), len(requests))

        for (node, epoch, status, raw), line in zip(requests, out_lines):
            parts = line.split()
            page_count = int(parts[0])
            byte_vals = [int(x, 16) for x in parts[1:]]
            check(f"trial node {node} page count", page_count, all_pagecounts[node])

            pages = [byte_vals[p * 8:(p + 1) * 8] for p in range(page_count)]
            names = [c["name"] for c in node_channels[node]]

            for page_idx, page_bytes in enumerate(pages):
                can_id = can_base_id + (node << node_shift) + page_idx
                msg = db.get_message_by_frame_id(can_id)
                decoded = msg.decode(bytes(page_bytes))

                check(f"trial node {node} page {page_idx} epoch",
                      int(decoded["epoch"]), epoch)
                for bit, name in ((0, "sensor_fault"), (1, "startup"), (2, "can_error")):
                    want = bool(status & (1 << bit))
                    raw_val = decoded[name]
                    got = bool(int(getattr(raw_val, "value", raw_val)))
                    check(f"trial node {node} page {page_idx} {name}", got, want)

                for ch, (p, _) in zip(node_channels[node], all_slots[node]):
                    if p != page_idx:
                        continue
                    idx = names.index(ch["name"])
                    want = raw[idx] * ch["scale"] + ch["offset"]
                    got = float(decoded[ch["name"]])
                    close(f"trial node {node} {ch['name']}", got, want,
                          abs(ch["scale"]) * 0.51 + 1e-9)

    # ---- encode direction, so the DBC is usable for injection too ----
    for node in range(node_count):
        for page in range(all_pagecounts[node]):
            can_id = can_base_id + (node << node_shift) + page
            msg = db.get_message_by_frame_id(can_id)

            values = {"epoch": 55, "sensor_fault": 1, "startup": 0, "can_error": 1}
            page_chans = [ch for ch, (p, _) in zip(node_channels[node], all_slots[node])
                         if p == page]
            for ch in page_chans:
                values[ch["name"]] = ch["scale"] * 100 + ch["offset"]

            encoded = msg.encode(values)
            decoded = msg.decode(encoded)

            check(f"node {node} page {page} encode round-trips epoch",
                  int(decoded["epoch"]), 55)
            for ch in page_chans:
                close(f"node {node} page {page} encode round-trips {ch['name']}",
                      float(decoded[ch["name"]]), values[ch["name"]],
                      abs(ch["scale"]) * 0.51 + 1e-9)

    print(f"  {checks - len(failures)}/{checks} checks passed")

    if failures:
        print(f"\n  {len(failures)} FAILURE(S):\n")
        for f in failures[:20]:
            print(f"   - {f}")
        if len(failures) > 20:
            print(f"   ... and {len(failures) - 20} more")
        return 1

    print("\n  cantools decodes v2's paged bus exactly as tlm2_can_pack_pages() packs it.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
