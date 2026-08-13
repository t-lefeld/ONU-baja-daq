#!/usr/bin/env python3
"""
Compile and run the E-CVT control loop host test.

    python tools/test_ecvt_control.py

Builds tools/test_ecvt_control.c against firmware/control/ecvt_control.c -
the real controller, unmodified - with no HAL involved at all (this module
has none). Follows the same build pattern as tools/test_firmware.py.

Not wired into tools/run_all_tests.py yet - this is new, additive scaffold
code (see firmware/control/README.md), and hooking it into the main suite
list is a one-line addition left for whoever picks this module up next,
rather than made here alongside code that hasn't run on real hardware yet.
"""

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

SOURCES = [
    ROOT / "tools" / "test_ecvt_control.c",
    ROOT / "firmware" / "control" / "ecvt_control.c",
]

INCLUDES = [
    ROOT / "firmware" / "control",
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
        exe = Path(td) / ("ecvt.exe" if sys.platform == "win32" else "ecvt")

        cmd = [
            cc, "-std=c99", "-O1", "-Wall", "-Wextra", "-Werror",
            *[f"-I{p}" for p in INCLUDES],
            *[str(s) for s in SOURCES],
            "-lm", "-o", str(exe),
        ]

        build = subprocess.run(cmd, capture_output=True, text=True)
        if build.returncode != 0:
            print("  ecvt_control failed to compile:\n")
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
