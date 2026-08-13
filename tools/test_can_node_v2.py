#!/usr/bin/env python3
"""
Build and run the v2 Bluepill node host test, once per node identity.

NODE_ID is a compile-time constant (see firmware/bluepill_node/can_node_v2.h),
so the same test source has to be compiled twice - once as the Front board and
once as the Rear board. Each build picks up a different node_id.h by putting
tools/v2_node_id_front/ or tools/v2_node_id_rear/ ahead of tools/hal_shim/ on
the include path.

Builds the REAL can_node_v2.c and telemetry_proto_v2.c unmodified, against
tools/hal_shim/, which fakes the HAL - same approach as tools/test_firmware.py
uses for the v1 firmware.
"""

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

SOURCES = [
    ROOT / "tools" / "test_can_node_v2.c",
    ROOT / "tools" / "hal_shim" / "hal_shim.c",
    ROOT / "tools" / "v2_node_test_stubs.c",
    ROOT / "firmware" / "bluepill_node" / "can_node_v2.c",
    ROOT / "protocol" / "telemetry_proto_v2.c",
]

# node_id dir goes FIRST so its node_id.h wins over any other on the path.
BASE_INCLUDES = [
    ROOT / "tools" / "hal_shim",
    ROOT / "protocol",
    ROOT / "firmware" / "bluepill_node",
]

VARIANTS = [
    ("Front", "v2_node_id_front"),
    ("Rear", "v2_node_id_rear"),
]


def run_variant(cc: str, label: str, node_id_dir: str, workdir: Path) -> bool:
    # The .exe suffix is not cosmetic on Windows: MinGW gcc appends it when
    # the -o name has no extension, so an extensionless path here compiles
    # fine and then fails to launch. Same guard tools/test_firmware.py uses.
    stem = f"test_can_node_v2_{node_id_dir}"
    exe = workdir / (f"{stem}.exe" if sys.platform == "win32" else stem)
    includes = [ROOT / "tools" / node_id_dir] + BASE_INCLUDES

    cmd = [cc, "-std=c99", "-Wall", "-Wextra", "-O1", "-o", str(exe)]
    for inc in includes:
        cmd += ["-I", str(inc)]
    cmd += [str(s) for s in SOURCES]
    cmd += ["-lm"]

    build = subprocess.run(cmd, capture_output=True, text=True)
    if build.returncode != 0:
        print(f"  {label}: BUILD FAILED")
        print(build.stderr.strip()[:2000])
        return False

    result = subprocess.run([str(exe)], capture_output=True, text=True)
    print(result.stdout.rstrip())
    if result.returncode != 0:
        print(result.stderr.strip()[:2000])
    return result.returncode == 0


def main() -> int:
    cc = next((c for c in ("gcc", "cc", "clang") if shutil.which(c)), None)
    if cc is None:
        # Skip, don't fail - matching every other C-backed suite here
        # (test_firmware.py, test_ecvt_control.py, test_roundtrip_v2_c.py).
        # A missing toolchain is a property of the machine, not a defect in
        # the firmware, and a suite that goes red on a Windows box with no
        # MinGW trains everyone to ignore red.
        print("  SKIP: no host C compiler found (looked for gcc, cc, clang)")
        print("  On Windows the compiler bundled with STM32CubeIDE targets ARM,")
        print("  not the host - install MSYS2/MinGW or run this under WSL.")
        return 0

    ok = True
    with tempfile.TemporaryDirectory() as td:
        workdir = Path(td)
        for label, node_id_dir in VARIANTS:
            if not run_variant(cc, label, node_id_dir, workdir):
                ok = False

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
