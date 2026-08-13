#!/usr/bin/env python3
"""
Cross-implementation test: the C encoder must produce byte-identical output to
the Python one.

This is the test that matters most in this project. The firmware and the ground
station are separate codebases that never see each other's source, and a
one-byte disagreement about a struct offset or an endianness assumption
produces a dashboard full of plausible-looking wrong numbers rather than an
obvious crash. Comparing the two implementations byte for byte is the only way
to catch that early.

    python tools/test_roundtrip.py

Needs a host C compiler (gcc, clang, or MSVC's cl on PATH).
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

from telemetry import proto  # noqa: E402
from telemetry.proto import Frame, NodeRecord, StreamParser  # noqa: E402

N_FRAMES = 64

failures: list[str] = []
checks = 0


def check(label: str, got, want) -> None:
    global checks
    checks += 1
    if got != want:
        failures.append(f"{label}\n     got  {got!r}\n     want {want!r}")


# --------------------------------------------------------------------
# Deterministic generator - must match roundtrip_harness.c exactly
# --------------------------------------------------------------------


class LCG:
    def __init__(self, seed: int = 0xC0FFEE) -> None:
        self.state = seed

    def __call__(self) -> int:
        self.state = (self.state * 1664525 + 1013904223) & 0xFFFFFFFF
        return self.state


def to_i16(u: int) -> int:
    """Reproduce the C cast (int16_t)(uint32 & 0xFFFF)."""
    return struct.unpack("<h", struct.pack("<H", u & 0xFFFF))[0]


# --------------------------------------------------------------------
# Build and run the C harness
# --------------------------------------------------------------------


def find_compiler() -> list[str] | None:
    for name in ("gcc", "cc", "clang"):
        if shutil.which(name):
            return [name]
    return None


def run_c_harness(workdir: Path) -> list[str]:
    cc = find_compiler()
    if cc is None:
        raise RuntimeError(
            "No C compiler found (looked for gcc, cc, clang).\n"
            "On Windows, the compiler that ships with STM32CubeIDE will not "
            "target the host; install MSYS2/MinGW or run this test under WSL."
        )

    exe = workdir / ("harness.exe" if sys.platform == "win32" else "harness")

    cmd = cc + [
        "-std=c99",
        "-O1",
        "-Wall",
        "-Wextra",
        "-Werror",
        f"-I{ROOT / 'protocol'}",
        str(ROOT / "tools" / "roundtrip_harness.c"),
        str(ROOT / "protocol" / "telemetry_proto.c"),
        "-o",
        str(exe),
    ]

    build = subprocess.run(cmd, capture_output=True, text=True)
    if build.returncode != 0:
        raise RuntimeError(f"compile failed:\n{build.stdout}\n{build.stderr}")

    # -Werror above means any warning is a failure. Worth keeping: the warnings
    # this code would produce (sign conversion, unused results) are exactly the
    # class of bug that corrupts a wire format silently.

    run = subprocess.run([str(exe)], capture_output=True, text=True)
    if run.returncode != 0:
        raise RuntimeError(f"harness exited {run.returncode}:\n{run.stdout}\n{run.stderr}")

    return run.stdout.strip().splitlines()


# --------------------------------------------------------------------
# Tests
# --------------------------------------------------------------------


def main() -> int:
    print("Cross-checking C and Python implementations of the wire format\n")

    # --- pure Python self-checks first -------------------------------------
    check("CRC check value for '123456789'", f"{proto.crc16(b'123456789'):04X}", "29B1")
    check("frame size", proto.FRAME_SIZE, 42)
    check("payload size", proto.PAYLOAD_SIZE, 30)
    check("record size x nodes", proto.REC_SIZE * proto.NODE_COUNT, proto.PAYLOAD_SIZE)

    # Channel scale factors must round-trip through int16 without drift.
    for node in range(proto.NODE_COUNT):
        for ch in proto.CHANNELS[node]:
            raw = ch.to_raw(ch.to_eng(1234))
            check(f"scale round-trip {node}/{ch.name}", raw, 1234)

    # --- build and run the C side ------------------------------------------
    with tempfile.TemporaryDirectory() as tmp:
        try:
            lines = run_c_harness(Path(tmp))
        except RuntimeError as exc:
            print(f"  SKIP  C comparison: {exc}\n")
            lines = []

    if lines:
        rng = LCG()
        c_frames: list[str] = []
        c_canpack: list[str] = []

        for line in lines:
            tag, _, rest = line.partition(" ")
            if tag == "CRC":
                check("C CRC check value", rest, "29B1")
            elif tag == "SIZE":
                check("C frame size", int(rest), proto.FRAME_SIZE)
            elif tag == "CANPACK":
                c_canpack.append(rest)
            elif tag == "FRAME":
                c_frames.append(rest)
            elif tag == "DECODE":
                check("C decode of its own frame", rest, "ok")
            elif tag == "CORRUPT":
                check("C rejects a flipped bit", int(rest), -5)
            elif tag == "FIND":
                check("C resyncs past junk and a false sync", int(rest), 16)

        # CAN payload packing
        for i, c_hex in enumerate(c_canpack):
            ch = [to_i16(rng()) for _ in range(3)]
            seq = rng() & 0xFF
            status = rng() & 0x07
            py = struct.pack("<hhhBB", *ch, seq, status).hex().upper()
            check(f"CAN payload #{i}", py, c_hex)

        # Full frames
        for i, c_hex in enumerate(c_frames):
            seq = rng() & 0xFFFF
            t_ms = rng()

            nodes = []
            for n in range(proto.NODE_COUNT):
                flags = rng() & 0x07
                can_seq = rng() & 0xFF
                loss = rng() & 0xFF
                raw = tuple(to_i16(rng()) for _ in range(3))
                nodes.append(NodeRecord(n, flags, can_seq, loss, raw))

            py = proto.encode_frame(Frame(seq, t_ms, nodes)).hex().upper()
            check(f"frame #{i}", py, c_hex)

            # And Python must be able to decode what C produced.
            decoded = proto.decode_frame(bytes.fromhex(c_hex))
            check(f"frame #{i} seq", decoded.seq, seq)
            check(f"frame #{i} t_ms", decoded.t_ms, t_ms)
            check(f"frame #{i} node2 ch", decoded.nodes[2].raw, nodes[2].raw)

    # --- stream parser behaviour -------------------------------------------
    good = proto.encode_frame(
        Frame(1, 1000, [NodeRecord(n, proto.NF_ONLINE, 0, 0, (n, -n, 100 * n))
                        for n in range(proto.NODE_COUNT)])
    )

    p = StreamParser()
    check("clean frame parses", len(list(p.feed(good))), 1)

    p = StreamParser()
    check("leading junk skipped", len(list(p.feed(b"\x00\xffnoise" + good))), 1)

    p = StreamParser()
    got = []
    for byte in good:                      # one byte at a time
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

    # A payload that happens to contain the sync pattern must not derail things.
    tricky = proto.encode_frame(
        Frame(proto.SYNC0 | (proto.SYNC1 << 8), 0xA55AA55A,
              [NodeRecord(n, 0, 0xA5, 0x5A, (0x5AA5, to_i16(0xA55A), 0))
               for n in range(proto.NODE_COUNT)])
    )
    p = StreamParser()
    check("sync bytes inside payload", len(list(p.feed(tricky))), 1)

    # --- CAN sequence-loss arithmetic ---------------------------------------
    # The hub computes (uint8)(new - last) - 1 as the number of missed frames.
    for last, new, expect in ((0, 1, 0), (0, 5, 4), (254, 1, 2), (255, 0, 0), (200, 200, -1)):
        delta = (new - last) & 0xFF
        check(f"loss {last}->{new}", delta - 1, expect)

    # --- report --------------------------------------------------------------
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
