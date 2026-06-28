"""
can_decode.py — Decode a CAN log file using the Baja DBC definition.

Usage:
    python can_decode.py --dbc ../can/baja.dbc --log <logfile.asc>
    python can_decode.py --dbc ../can/baja.dbc --log <logfile.asc> --out decoded.csv

Requirements:
    pip install cantools pandas
"""

import argparse
import cantools
import pandas as pd


def decode_log(dbc_path: str, log_path: str, out_path: str | None = None):
    db = cantools.database.load_file(dbc_path)

    records = []

    with open(log_path, "r") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("//") or line.startswith("date"):
                continue
            # ASC format: <timestamp> <channel> <ID> <dir> <d> <len> <b0> <b1> ...
            parts = line.split()
            if len(parts) < 7:
                continue
            try:
                timestamp = float(parts[0])
                can_id = int(parts[2], 16)
                dlc = int(parts[5])
                raw = bytes(int(b, 16) for b in parts[6:6 + dlc])

                try:
                    msg = db.get_message_by_frame_id(can_id)
                    decoded = msg.decode(raw)
                    for signal_name, value in decoded.items():
                        records.append({
                            "timestamp": timestamp,
                            "message": msg.name,
                            "signal": signal_name,
                            "value": value,
                        })
                except KeyError:
                    pass  # Unknown CAN ID — skip

            except (ValueError, IndexError):
                continue

    df = pd.DataFrame(records)

    if out_path:
        df.to_csv(out_path, index=False)
        print(f"Decoded {len(df)} signal samples → {out_path}")
    else:
        print(df.to_string())

    return df


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Decode Baja CAN log file")
    parser.add_argument("--dbc", required=True, help="Path to DBC file")
    parser.add_argument("--log", required=True, help="Path to ASC log file")
    parser.add_argument("--out", help="Output CSV path (optional)")
    args = parser.parse_args()

    decode_log(args.dbc, args.log, args.out)
