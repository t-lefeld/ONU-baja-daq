#!/usr/bin/env python3
"""
Compile and run the v2 protocol host test.

    python tools/test_roundtrip_v2_c.py

Builds tools/test_roundtrip_v2.c against protocol/telemetry_proto_v2.c - the
real file, unmodified - with no HAL involved (v2 has none, same as
firmware/control/ecvt_control.c). Follows the same build pattern as
tools/test_firmware.py and tools/test_ecvt_control.py.

Named test_roundtrip_v2_c.py rather than test_roundtrip_v2.py: that name is
reserved for a separate, Python-side cross-language test (v2's C
implementation vs. a future proto_v2.py) that does not exist yet - see
V2_DESIGN_NOTES.md, "No proto_v2.py yet". This script only builds and runs
the standalone C self-test described in test_roundtrip_v2.c's own header
comment (torn bursts, epoch wraparound, CRC corruption, GPS int32
precision, full pack/reassemble/encode/decode pipeline).
"""

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

SOURCES = [
    ROOT / "tools" / "test_roundtrip_v2.c",
    ROOT / "protocol" / "telemetry_proto_v2.c",
]

INCLUDES = [
    ROOT / "protocol",
]


def main() -> int:
    cc = next((c for c in ("gcc", "cc", "clang") if shutil.which(c)), None)
    if cc is None:
        print("  SKIP: no host C compiler found (looked for gcc, cc, clang)")
        return 0

    missing = [s for s in SOURCES if not s.exists()]
    if missing:
        print("  Missing sources:")
        for m in missing:
            print(f"    {m}")
        return 1

    with tempfile.TemporaryDirectory() as td:
        exe = Path(td) / ("roundtrip_v2.exe" if sys.platform == "win32" else "roundtrip_v2")

        cmd = [
            cc, "-std=c99", "-O1", "-Wall", "-Wextra", "-Werror",
            *[f"-I{p}" for p in INCLUDES],
            *[str(s) for s in SOURCES],
            "-lm", "-o", str(exe),
        ]

        build = subprocess.run(cmd, capture_output=True, text=True)
        if build.returncode != 0:
            print("  test_roundtrip_v2 failed to compile:\n")
            print(build.stdout)
            print(build.stderr)
            return 1

        run = subprocess.run([str(exe)], capture_output=True, text=True)
        print(run.stdout, end="")
        if run.stderr:
            print(run.stderr, end="", file=sys.stderr)
        return run.returncode


if __name__ == "__main__":
    sys.exit(main())
