#!/usr/bin/env python3
"""
Run every test suite in the repo.

    python tools/run_all_tests.py
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

SUITES = [
    ("wire format (C vs Python)", "test_roundtrip.py"),
    ("FAT32 writer vs fsck.fat", "test_fat32.py"),
    ("node + hub firmware logic", "test_firmware.py"),
    ("CAN = LoRa = SD agreement", "test_paths_agree.py"),
    ("ground station backend", "test_app.py"),
    ("FramePump (shared UI logic)", "test_qt.py"),
    ("DBC vs firmware packing", "test_dbc.py"),
    ("repository structure", "test_repo.py"),
    ("app-window launcher", "test_launcher.py"),
    ("v2 protocol (C host test)", "test_roundtrip_v2_c.py"),
    ("v2 wire format (C vs Python)", "test_roundtrip_v2.py"),
    ("v2 Bluepill node firmware", "test_can_node_v2.py"),
    ("E-CVT control loop", "test_ecvt_control.py"),
    ("v2 DBC vs firmware packing", "test_dbc_v2.py"),
    ("v2 channel table vs simulator", "test_channel_sync.py"),
]


def main() -> int:
    results = []

    for label, script in SUITES:
        print("=" * 70)
        print(f"  {label}")
        print("=" * 70)

        r = subprocess.run([sys.executable, str(ROOT / "tools" / script)])
        results.append((label, r.returncode))
        print()

    print("=" * 70)
    failed = [l for l, rc in results if rc != 0]

    for label, rc in results:
        print(f"  {'PASS' if rc == 0 else 'FAIL'}  {label}")

    print("=" * 70)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
