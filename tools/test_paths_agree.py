#!/usr/bin/env python3
"""
Build and run the CAN/LoRa/SD agreement test.

Unlike tools/test_firmware.py, this one builds the hub with **HUB_ENABLE_SD=1**
so the SD write path is actually compiled and exercised. That is the whole
point: the main firmware suite compiles SD out, so without this the SD leg
never runs at all and "the log matches the radio" is an unchecked claim.

Real telemetry_hub.c and lora_e22.c, unmodified. Only sd_log is stubbed - and
only to capture the bytes, since sd_log.c is SPI/FAT32 register code covered
separately by tools/test_fat32.py.
"""

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

SOURCES = [
    ROOT / "tools" / "test_paths_agree.c",
    ROOT / "tools" / "hal_shim" / "hal_shim.c",
    ROOT / "protocol" / "telemetry_proto.c",
    ROOT / "firmware" / "nucleo_hub" / "telemetry_hub.c",
    ROOT / "firmware" / "nucleo_hub" / "lora_e22.c",
]

INCLUDES = [
    ROOT / "tools" / "hal_shim",
    ROOT / "protocol",
    ROOT / "firmware" / "nucleo_hub",
    ROOT / "firmware" / "bluepill_node",
]


def main() -> int:
    cc = next((c for c in ("gcc", "cc", "clang") if shutil.which(c)), None)
    if cc is None:
        print("  SKIP: no host C compiler found (looked for gcc, cc, clang)")
        print("  On Windows the compiler bundled with STM32CubeIDE targets ARM,")
        print("  not the host - install MSYS2/MinGW or run this under WSL.")
        return 0

    missing = [s for s in SOURCES if not s.exists()]
    if missing:
        print("  Missing sources:")
        for m in missing:
            print(f"    {m}")
        return 1

    with tempfile.TemporaryDirectory() as td:
        exe = Path(td) / ("paths.exe" if sys.platform == "win32" else "paths")

        cmd = [
            cc, "-std=c99", "-O1", "-Wall", "-Wextra", "-Werror",
            "-DHUB_ENABLE_SD=1", "-DHUB_DEBUG_UART=0",
            *[f"-I{p}" for p in INCLUDES],
            *[str(s) for s in SOURCES],
            "-lm", "-o", str(exe),
        ]

        build = subprocess.run(cmd, capture_output=True, text=True)
        if build.returncode != 0:
            print("  Failed to compile:\n")
            print(build.stderr[:3000])
            return 1

        run = subprocess.run([str(exe)], capture_output=True, text=True)
        print(run.stdout.rstrip())
        if run.returncode != 0 and run.stderr:
            print(run.stderr[:2000])
        return run.returncode


if __name__ == "__main__":
    sys.exit(main())
