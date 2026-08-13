#!/usr/bin/env python3
"""
Compile and run the node and hub firmware on the host.

    python tools/test_firmware.py

Builds can_node.c, telemetry_hub.c, lora_e22.c and telemetry_proto.c - the real
files, unmodified - against tools/hal_shim/, which fakes the HAL and puts
HAL_GetTick() under test control.

The timing-dependent behaviour is what this buys: transmit cadence, node
timeouts, sequence-loss arithmetic across the 8-bit wrap, ring buffer overflow.
None of that can be verified by reading the code, and all of it is miserable to
debug with four boards on a bench.

SD logging is compiled out here (HUB_ENABLE_SD=0); fat32.c has its own suite in
tools/test_fat32.py, and sd_spi.c is register-level code that only means
anything on silicon.
"""

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

SOURCES = [
    ROOT / "tools" / "test_firmware.c",
    ROOT / "tools" / "hal_shim" / "hal_shim.c",
    ROOT / "protocol" / "telemetry_proto.c",
    ROOT / "firmware" / "bluepill_node" / "can_node.c",
    ROOT / "firmware" / "nucleo_hub" / "telemetry_hub.c",
    ROOT / "firmware" / "nucleo_hub" / "lora_e22.c",
]

INCLUDES = [
    ROOT / "tools" / "hal_shim",
    ROOT / "protocol",
    ROOT / "firmware" / "bluepill_node",
    ROOT / "firmware" / "nucleo_hub",
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
        exe = Path(td) / ("fw.exe" if sys.platform == "win32" else "fw")

        cmd = [
            cc, "-std=c99", "-O1", "-Wall", "-Wextra", "-Werror",
            "-DHUB_ENABLE_SD=0", "-DHUB_DEBUG_UART=1",
            *[f"-I{p}" for p in INCLUDES],
            *[str(s) for s in SOURCES],
            "-lm", "-o", str(exe),
        ]

        build = subprocess.run(cmd, capture_output=True, text=True)
        if build.returncode != 0:
            print("  Firmware failed to compile:\n")
            print(build.stdout)
            print(build.stderr)
            return 1

        # -Werror above is deliberate. The warnings this code could produce
        # (sign conversion, unused results, uninitialised reads) are exactly the
        # class of bug that turns into an intermittent hardware fault.
        run = subprocess.run([str(exe)], capture_output=True, text=True)
        print(run.stdout, end="")
        if run.stderr:
            print(run.stderr, end="", file=sys.stderr)
        return run.returncode


if __name__ == "__main__":
    sys.exit(main())
