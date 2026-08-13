#!/usr/bin/env python3
"""
Convert an SD card log (LOGnnnn.TLM) into a CSV.

    python tools/tlm_to_csv.py logs/LOG0001.TLM
    python tools/tlm_to_csv.py logs/*.TLM --out combined.csv

Runs the file through the same StreamParser the live link uses, so anything the
dashboard could decode ends up in the CSV and anything it could not is reported
as a skipped frame rather than silently dropped.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "pc_app"))

from telemetry.proto import FRAME_SIZE, StreamParser  # noqa: E402
from telemetry.recorder import CsvRecorder  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="+", type=Path)
    ap.add_argument("-o", "--out", type=Path, help="output CSV (default: alongside input)")
    args = ap.parse_args()

    out = args.out or args.files[0].with_suffix(".csv")
    rec = CsvRecorder(out)
    parser = StreamParser()

    total_bytes = 0

    for path in args.files:
        if not path.exists():
            print(f"  skip {path}: not found", file=sys.stderr)
            continue

        data = path.read_bytes()
        total_bytes += len(data)
        before = parser.frames_ok

        for frame in parser.feed(data):
            rec.write(frame)

        print(f"  {path.name}: {len(data)} bytes -> {parser.frames_ok - before} frames")

    rec.close()

    expected = total_bytes // FRAME_SIZE
    print(f"\nWrote {rec.rows} rows to {out}")

    if parser.crc_errors or parser.bytes_discarded:
        print(
            f"  {parser.crc_errors} CRC failures, "
            f"{parser.bytes_discarded} bytes discarded"
        )
        # A log written straight to SD should be perfectly clean; junk here
        # points at the card or the write path, not the radio.
        if parser.frames_ok < expected * 0.99:
            print("  Log is lossier than expected for an SD capture - check the card.")

    return 0 if rec.rows else 1


if __name__ == "__main__":
    sys.exit(main())
