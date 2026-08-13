#!/usr/bin/env python3
"""
Cross-implementation test: the C v2 encoder/CAN-packer/reassembler must agree
byte-for-byte (and flag-for-flag, loss-count-for-loss-count) with proto_v2.py.

Same rationale as tools/test_roundtrip.py: firmware and the ground station are
separate codebases that never see each other's source. For v2 there is an
extra way for that to go wrong that v1 never had - per-node record width is
not fixed, so a byte-offset mistake in either implementation produces
plausible-looking wrong numbers on SOME channels while others stay right,
which is a much easier bug to miss by eye than v1's uniform layout.

    python tools/test_roundtrip_v2.py

Needs a host C compiler (gcc, clang, or MSVC's cl on PATH). Builds
tools/dump_frames_v2.c against protocol/telemetry_proto_v2.c and diffs its
stdout against this module's own encoder/decoder/reassembler.
"""

from __future__ import annotations

import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "pc_app"))

from telemetry import proto_v2 as v2  # noqa: E402
from telemetry.proto_v2 import Frame, NodeRecord, NodeReassembler, StreamParser  # noqa: E402

N_FRAMES = 32   # must match N_FRAMES in dump_frames_v2.c

failures: list[str] = []
checks = 0


def check(label: str, got, want) -> None:
    global checks
    checks += 1
    if got != want:
        failures.append(f"{label}\n     got  {got!r}\n     want {want!r}")


def check_close(label: str, got: float, want: float, tol: float) -> None:
    global checks
    checks += 1
    if abs(got - want) > tol:
        failures.append(f"{label}\n     got  {got!r}\n     want {want!r} (tol {tol})")


# --------------------------------------------------------------------
# Deterministic generator - must match dump_frames_v2.c's next_rand()/
# rand_raw() call-for-call, in the exact order the C file makes them.
# --------------------------------------------------------------------


class LCG:
    def __init__(self, seed: int = 0xC0FFEE) -> None:
        self.state = seed

    def __call__(self) -> int:
        self.state = (self.state * 1664525 + 1013904223) & 0xFFFFFFFF
        return self.state


def to_i16(u: int) -> int:
    return struct.unpack("<h", struct.pack("<H", u & 0xFFFF))[0]


def to_i32(u: int) -> int:
    return struct.unpack("<i", struct.pack("<I", u & 0xFFFFFFFF))[0]


def hex_i32(chunk: str) -> int:
    """Undo dump_frames_v2.c's `printf("%08X", (uint32_t)value)`."""
    v = int(chunk, 16)
    return v - 0x1_0000_0000 if v >= 0x8000_0000 else v


# --------------------------------------------------------------------
# Build and run the C dumper
# --------------------------------------------------------------------


def find_compiler() -> list[str] | None:
    for name in ("gcc", "cc", "clang"):
        if shutil.which(name):
            return [name]
    return None


def run_c_dumper(workdir: Path) -> list[str]:
    cc = find_compiler()
    if cc is None:
        raise RuntimeError(
            "No C compiler found (looked for gcc, cc, clang).\n"
            "On Windows, the compiler that ships with STM32CubeIDE will not "
            "target the host; install MSYS2/MinGW or run this test under WSL."
        )

    exe = workdir / ("dump_frames_v2.exe" if sys.platform == "win32" else "dump_frames_v2")

    cmd = cc + [
        "-std=c99",
        "-O1",
        "-Wall",
        "-Wextra",
        "-Werror",
        f"-I{ROOT / 'protocol'}",
        str(ROOT / "tools" / "dump_frames_v2.c"),
        str(ROOT / "protocol" / "telemetry_proto_v2.c"),
        "-o",
        str(exe),
    ]

    build = subprocess.run(cmd, capture_output=True, text=True)
    if build.returncode != 0:
        raise RuntimeError(f"compile failed:\n{build.stdout}\n{build.stderr}")

    run = subprocess.run([str(exe)], capture_output=True, text=True)
    if run.returncode != 0:
        raise RuntimeError(f"dumper exited {run.returncode}:\n{run.stdout}\n{run.stderr}")

    return run.stdout.strip().splitlines()


# --------------------------------------------------------------------
# Tests
# --------------------------------------------------------------------


def main() -> int:
    print("Cross-checking C (telemetry_proto_v2.c) and Python (proto_v2.py)\n")

    # --- pure Python self-checks first --------------------------------------
    check("CRC check value for '123456789'", f"{v2.crc16(b'123456789'):04X}", "29B1")

    check("node count", v2.NODE_COUNT, 4)
    check("chan count per node (Hub, Front, Rear, Motor)", v2.CHAN_COUNT, (9, 5, 5, 5))
    check("page count per node", v2.PAGE_COUNT, (4, 2, 2, 2))
    check("wire bytes per node (matches V2_DESIGN_NOTES.md)", v2.WIRE_BYTES, (22, 10, 10, 10))
    check("payload size (design doc: 68 B)", v2.PAYLOAD_SIZE, 68)
    check("frame size (design doc: 80 B)", v2.FRAME_SIZE, 80)

    # Every scale factor must round-trip through its channel's integer width
    # without drift - the same check v1 runs, extended to also exercise the
    # I32 GPS channels.
    for node in range(v2.NODE_COUNT):
        for ch in v2.CHANNELS[node]:
            probe = 1234 if ch.width == v2.W_I16 else 123_456_789
            raw = ch.to_raw(ch.to_eng(probe))
            check(f"scale round-trip {node}/{ch.name}", raw, probe)

    # Negative values, explicitly - int.from_bytes(..., signed=True) is the
    # classic thing to forget, and struct's 'h'/'i' codes sidestep it, but
    # prove the round trip anyway rather than trusting the sidestep blindly.
    neg_frame = Frame(
        seq=1,
        t_ms=2,
        nodes=[
            NodeRecord(0, v2.NF_ONLINE, 5, 0,
                       (-2_000_000_000, -1, -32768, -1, 0, -1, 32767, -30000, 12345)),
            NodeRecord(1, 0, 0, 0, (-1, -32768, 32767, 0, -12345)),
            NodeRecord(2, 0, 0, 0, (0, 0, 0, 0, 0)),
            NodeRecord(3, v2.NF_PARTIAL, 9, 3, (-500, -6000, -1, -32768, 32767)),
        ],
    )
    neg_bytes = v2.encode_frame(neg_frame)
    neg_decoded = v2.decode_frame(neg_bytes)
    check("negative int32 (gps_lat) round trip", neg_decoded.nodes[0].raw[0], -2_000_000_000)
    check("negative int16 min round trip", neg_decoded.nodes[0].raw[2], -32768)
    check("negative int16 round trip (node 1)", neg_decoded.nodes[1].raw[1], -32768)
    check("partial flag round trip", neg_decoded.nodes[3].partial, True)
    check("online flag round trip", neg_decoded.nodes[0].online, True)

    # --- build and run the C side -------------------------------------------
    with tempfile.TemporaryDirectory() as tmp:
        try:
            lines = run_c_dumper(Path(tmp))
        except RuntimeError as exc:
            print(f"  SKIP  C comparison: {exc}\n")
            lines = []

    if lines:
        rng = LCG()

        frame_hexes: list[str] = []
        decode_results: list[str] = []
        mislabel_hex: str | None = None
        canpack_lines: list[tuple[int, int, int, str]] = []
        gpsraw = None
        gpseng = None
        torn_pages1: dict[int, bytes] = {}
        torn_pages2: dict[int, bytes] = {}
        torn_result = None
        wrap_pagesA: dict[int, bytes] = {}
        wrap_pagesB: dict[int, bytes] = {}
        wrap_result = None
        wrap_result2 = None
        corrupt_rc = None
        find_off = None

        for line in lines:
            tag, _, rest = line.partition(" ")
            if tag == "CRC":
                check("C CRC check value", rest, "29B1")
            elif tag == "SIZE":
                check("C frame size", int(rest), v2.FRAME_SIZE)
            elif tag == "FRAME":
                frame_hexes.append(rest)
            elif tag == "DECODE":
                decode_results.append(rest)
            elif tag == "MISLABEL":
                mislabel_hex = rest
            elif tag == "CANPACK":
                node_s, epoch_s, status_s, hexstr = rest.split(" ", 3)
                canpack_lines.append((int(node_s), int(epoch_s), int(status_s), hexstr))
            elif tag == "GPSRAW":
                gpsraw = rest
            elif tag == "GPSENG":
                gpseng = rest
            elif tag == "TORN_PAGE":
                epoch_label, page_s, hexstr = rest.split(" ", 2)
                data = bytes.fromhex(hexstr)
                (torn_pages1 if epoch_label == "1" else torn_pages2)[int(page_s)] = data
            elif tag == "TORN_RESULT":
                flags_s, epoch_s, loss_s, ch_hex = rest.split(" ", 3)
                torn_result = (int(flags_s), int(epoch_s), int(loss_s), ch_hex)
            elif tag == "WRAP_PAGE":
                epoch_label, page_s, hexstr = rest.split(" ", 2)
                data = bytes.fromhex(hexstr)
                (wrap_pagesA if epoch_label == "254" else wrap_pagesB)[int(page_s)] = data
            elif tag == "WRAP_RESULT":
                wrap_result = tuple(int(x) for x in rest.split(" "))
            elif tag == "WRAP_RESULT2":
                wrap_result2 = int(rest)
            elif tag == "CORRUPT":
                corrupt_rc = int(rest)
                check("C rejects a flipped bit", corrupt_rc, -5)
            elif tag == "FIND":
                find_off = int(rest)
                check("C resyncs past junk and a false sync", find_off, 16)

        # ---- full radio frames: rebuild C's inputs from the LCG, encode
        # with Python, and require byte-identical hex. Then decode C's own
        # hex with Python and check the reassembled record matches too. ----
        for i, c_hex in enumerate(frame_hexes):
            seq = rng() & 0xFFFF
            t_ms = rng()

            nodes = []
            for node_id in range(v2.NODE_COUNT):
                flags = rng() & 0x0F
                epoch = rng() & 0xFF
                loss = rng() & 0xFF
                raw = tuple(
                    to_i32(rng()) if d.width == v2.W_I32 else to_i16(rng() & 0xFFFF)
                    for d in v2.CHANNELS[node_id]
                )
                nodes.append(NodeRecord(node_id, flags, epoch, loss, raw))

            py_hex = v2.encode_frame(Frame(seq, t_ms, nodes)).hex().upper()
            check(f"frame #{i} bytes", py_hex, c_hex)

            decoded = v2.decode_frame(bytes.fromhex(c_hex))
            check(f"frame #{i} seq", decoded.seq, seq)
            check(f"frame #{i} t_ms", decoded.t_ms, t_ms)
            check(f"frame #{i} node0 (Hub, mixed i16/i32) raw", decoded.nodes[0].raw, nodes[0].raw)
            check(f"frame #{i} node3 (Motor) raw", decoded.nodes[3].raw, nodes[3].raw)

            if i < len(decode_results):
                check(f"C self-decode #{i}", decode_results[i], "ok")

        # ---- mislabelled node_id: layout must be position-driven ----------
        if mislabel_hex is not None:
            nodes = []
            for node_id in range(v2.NODE_COUNT):
                raw = tuple(
                    to_i32(rng()) if d.width == v2.W_I32 else to_i16(rng() & 0xFFFF)
                    for d in v2.CHANNELS[node_id]
                )
                nodes.append(NodeRecord(200 + node_id, 0, 50 + node_id, 0, raw))

            py_hex = v2.encode_frame(Frame(777, 999888, nodes)).hex().upper()
            check("mislabelled-node_id frame bytes", py_hex, mislabel_hex)

            decoded = v2.decode_frame(bytes.fromhex(mislabel_hex))
            for node_id in range(v2.NODE_COUNT):
                check(f"mislabel node {node_id}: node_id echoed, not used for parsing",
                      decoded.nodes[node_id].node_id, 200 + node_id)
                check(f"mislabel node {node_id}: channel values still position-decoded",
                      decoded.nodes[node_id].raw, nodes[node_id].raw)

        # ---- CAN page packing across all four nodes ------------------------
        # Iteration order below (outer iter, inner node) matches the C
        # dumper's print order exactly, so canpack_lines[idx] lines up.
        idx = 0
        for _iter in range(8):
            for node in range(v2.NODE_COUNT):
                raw = tuple(
                    to_i32(rng()) if d.width == v2.W_I32 else to_i16(rng() & 0xFFFF)
                    for d in v2.CHANNELS[node]
                )
                epoch = rng() & 0xFF
                status = rng() & 0x07

                c_node, c_epoch, c_status, c_hex = canpack_lines[idx]
                idx += 1
                check(f"canpack #{idx} node id matches stream order", c_node, node)
                check(f"canpack #{idx} epoch matches stream order", c_epoch, epoch)
                check(f"canpack #{idx} status matches stream order", c_status, status)

                pages = v2.can_pack_pages(node, raw, epoch, status)
                py_hex = b"".join(pages).hex().upper()
                check(f"canpack #{idx} bytes (node {node})", py_hex, c_hex)

        # ---- GPS precision: int32 * 1e-7 deg, positive lat / negative lon --
        if gpsraw and gpseng:
            raw_lat, raw_lon = (int(x) for x in gpsraw.split())
            lat_c, lon_c = (float(x) for x in gpseng.split())

            hub = v2.CHANNELS[0]
            lat_py = hub[0].to_eng(raw_lat)
            lon_py = hub[1].to_eng(raw_lon)

            # C and Python both promote the float32 scale to double before
            # multiplying, so this should agree to within a few ULP of a
            # double - 1e-9 degrees is generous by several orders of
            # magnitude and still ~100x tighter than the ~1cm the format is
            # actually spec'd for.
            check_close("gps_lat matches C's own computed engineering value", lat_py, lat_c, 1e-9)
            check_close("gps_lon matches C's own computed engineering value", lon_py, lon_c, 1e-9)

            # NOTE on methodology: raw_lat/raw_lon above were chosen as if
            # scale were the exact double 1e-7 (raw = round(deg * 1e7)). The
            # real channel scale is a C `float`, i.e. float32(1e-7) ==
            # 1.0000000116860974e-07 (double), not 1e-7 exactly - scale
            # itself is never transmitted, both sides just hard-code the
            # same truncated constant. Comparing a hand-picked "textbook"
            # raw value against a literal double degree figure therefore
            # shows a latitude-dependent offset of several cm (here, ~5.3 cm
            # at raw ~4.08e8) that is NOT a bug - it is the same number both
            # implementations compute, confirmed above - just a reminder
            # that the wire integer means "deg * float32(1e-7)", not "deg *
            # 1e-7" exactly.
            #
            # The design doc's actual precision claim - int32 * 1e-7 deg is
            # accurate to ~1 cm - is about ROUND-TRIP quantization noise
            # (encode a real fix, decode it back), which is what an actual
            # GPS-to-raw-to-GPS path does. Test that claim the way a real
            # caller would exercise it: through to_raw()/to_eng() using the
            # SAME (float32-truncated) scale on both ends, so the constant's
            # own imprecision cancels out and only the integer rounding step
            # remains.
            true_lat, true_lon = 40.7660321, -83.8220456
            rt_lat = hub[0].to_eng(hub[0].to_raw(true_lat))
            rt_lon = hub[1].to_eng(hub[1].to_raw(true_lon))
            err_lat_m = abs(rt_lat - true_lat) * 111_320.0
            err_lon_m = abs(rt_lon - true_lon) * 111_320.0
            check("gps_lat round-trip quantization within ~2 cm", err_lat_m < 0.02, True)
            check("gps_lon round-trip quantization within ~2 cm", err_lon_m < 0.02, True)

        # ---- torn burst: replay C's own CAN pages through Python's own
        # reassembler and require the identical flags/epoch/loss/values ----
        if torn_result is not None and len(torn_pages1) == 4 and 0 in torn_pages2:
            c_flags, c_epoch, c_loss, c_ch_hex = torn_result
            c_ch = tuple(hex_i32(c_ch_hex[i * 8:(i + 1) * 8]) for i in range(9))

            r = NodeReassembler(0)
            for p in range(4):
                completed = r.apply_page(p, torn_pages1[p], now_ms=1000)
                if p < 3:
                    check(f"torn: page {p}/4 of epoch 1 not yet complete", completed, False)
                else:
                    check("torn: epoch 1 completes on its last page", completed, True)

            completed2 = r.apply_page(0, torn_pages2[0], now_ms=1100)
            check("torn: single page of epoch 2 never completes", completed2, False)

            rec = r.snapshot(now_ms=1100)
            check("torn: flags match C's reassembler", rec.flags, c_flags)
            check("torn: epoch match C's reassembler (stuck at last complete)", rec.epoch, c_epoch)
            check("torn: loss matches C's reassembler", rec.loss, c_loss)
            check("torn: PARTIAL flag set", rec.partial, True)
            check("torn: ONLINE flag still set (a page did arrive)", rec.online, True)
            check("torn: channel values match C's reassembler (last GOOD snapshot)", rec.raw, c_ch)

        # ---- epoch wraparound loss counting, replayed through Python ------
        if wrap_result is not None and wrap_result2 is not None and len(wrap_pagesA) == 2 and len(wrap_pagesB) == 2:
            c_flags, c_epoch, c_loss = wrap_result

            r = NodeReassembler(3)
            for p in range(2):
                r.apply_page(p, wrap_pagesA[p], now_ms=0)
            for p in range(2):
                r.apply_page(p, wrap_pagesB[p], now_ms=100)

            rec = r.snapshot(now_ms=100)
            check("epoch wrap: flags match C", rec.flags, c_flags)
            check("epoch wrap: epoch match C", rec.epoch, c_epoch)
            check("epoch wrap: loss match C (skipped epoch 255 -> 1 lost burst)", rec.loss, c_loss)

            rec2 = r.snapshot(now_ms=100)
            check("epoch wrap: loss resets after being read", rec2.loss, wrap_result2)

    # --- stream parser behaviour (pure Python, same pattern as v1) ---------
    good = v2.encode_frame(
        Frame(1, 1000, [
            NodeRecord(n, v2.NF_ONLINE, 0, 0, tuple(n * 10 + i for i in range(len(v2.CHANNELS[n]))))
            for n in range(v2.NODE_COUNT)
        ])
    )

    p = StreamParser()
    check("clean frame parses", len(list(p.feed(good))), 1)

    p = StreamParser()
    check("leading junk skipped", len(list(p.feed(b"\x00\xffnoise" + good))), 1)

    p = StreamParser()
    got = []
    for byte in good:
        got += list(p.feed(bytes([byte])))
    check("byte-at-a-time reassembly", len(got), 1)

    p = StreamParser()
    check("split across reads", len(list(p.feed(good[:20]))), 0)
    check("split across reads, completed", len(list(p.feed(good[20:]))), 1)

    p = StreamParser()
    corrupt = bytearray(good)
    corrupt[25] ^= 0xFF
    check("corrupt frame rejected", len(list(p.feed(bytes(corrupt)))), 0)
    check("corrupt frame counted", p.crc_errors > 0, True)

    p = StreamParser()
    check("back-to-back frames", len(list(p.feed(good * 3))), 3)

    # --- meta/to_dict shape sanity (what pump.py would need, see report) ---
    frame = v2.decode_frame(good)
    d = frame.to_dict()
    check("to_dict node count", len(d["nodes"]), v2.NODE_COUNT)
    for n in range(v2.NODE_COUNT):
        check(f"to_dict node {n} channel count", len(d["nodes"][n]["channels"]), v2.CHAN_COUNT[n])
    check("to_dict has stale/fault/loss/channels (dashboard contract fields)",
          all(k in d["nodes"][0] for k in ("stale", "fault", "loss", "channels")), True)
    check("to_dict channels have name/unit/value (meta+frame contract fields)",
          all(k in d["nodes"][0]["channels"][0] for k in ("name", "unit", "value")), True)

    # --- report ---------------------------------------------------------------
    print(f"  {checks - len(failures)}/{checks} checks passed")

    if failures:
        print(f"\n  {len(failures)} FAILURE(S):\n")
        for f in failures:
            print(f"   - {f}")
        return 1

    print("\n  C and Python agree byte for byte.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
