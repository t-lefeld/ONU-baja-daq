#!/usr/bin/env python3
"""
Validate protocol/telemetry.dbc against the firmware's own packing.

    python tools/test_dbc.py

A DBC is only useful if a third-party tool decodes the bus the same way the
hub does. So this parses the file with cantools - the same library SavvyCAN,
python-can and most Python CAN tooling build on - and checks that decoding a
payload produced by proto.encode_can() gives back the engineering values the
channel table says it should.

The failure this is really guarding against is byte order. Get @1 vs @0 wrong
and every signal still decodes, just to plausible-looking garbage: temperatures
in the thousands, or worse, values that look almost right. Nothing about the
file's syntax would flag it.

Skips cleanly if cantools is not installed (pip install cantools).
"""

from __future__ import annotations

import random
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "pc_app"))

from telemetry import proto  # noqa: E402

DBC = ROOT / "protocol" / "telemetry.dbc"

failures: list[str] = []
checks = 0


def check(label: str, got, want) -> None:
    global checks
    checks += 1
    if got != want:
        failures.append(f"{label}\n     got  {got!r}\n     want {want!r}")


def close(label: str, got: float, want: float, tol: float) -> None:
    global checks
    checks += 1
    if abs(got - want) > tol:
        failures.append(f"{label}\n     got  {got!r}\n     want {want!r} (tol {tol})")


def ok(label: str, cond: bool) -> None:
    global checks
    checks += 1
    if not cond:
        failures.append(label)


def main() -> int:
    print("Validating protocol/telemetry.dbc\n")

    # The file is generated. If someone edits the channel table and forgets to
    # regenerate, the DBC silently describes the old bus - so regenerate and
    # confirm nothing moved.
    before = DBC.read_bytes() if DBC.exists() else b""
    subprocess.run([sys.executable, str(ROOT / "tools" / "gen_dbc.py")],
                   capture_output=True, check=True)
    after = DBC.read_bytes()

    # Only the date line differs on a re-run; compare everything else.
    def strip_date(b: bytes) -> list[bytes]:
        return [l for l in b.splitlines() if not l.startswith(b'CM_ "Generated')]

    ok("DBC is up to date with the channel table", strip_date(before) == strip_date(after))

    try:
        import cantools
    except ImportError:
        print("  SKIP: cantools not installed (pip install cantools)")
        print(f"  {checks - len(failures)}/{checks} checks passed")
        return 1 if failures else 0

    db = cantools.database.load_file(DBC)

    # ---- structure ----
    check("message count", len(db.messages), proto.NODE_COUNT)
    check("node count", len(db.nodes), proto.NODE_COUNT + 1)  # 3 senders + HUB

    for node in range(proto.NODE_COUNT):
        can_id = proto.CAN_DATA_BASE_ID + node
        msg = db.get_message_by_frame_id(can_id)

        check(f"node {node} DLC", msg.length, proto.CAN_DLC)
        check(f"node {node} is standard ID", msg.is_extended_frame, False)
        check(f"node {node} cycle time", msg.cycle_time, proto.NODE_TX_PERIOD_MS)
        check(f"node {node} signal count", len(msg.signals),
              proto.CH_PER_NODE + 1 + 3)   # channels + seq + 3 status bits

        names = {s.name for s in msg.signals}
        for ch in proto.CHANNELS[node]:
            ok(f"node {node} has signal {ch.name}", ch.name in names)
        for extra in ("seq", "sensor_fault", "startup", "can_error"):
            ok(f"node {node} has signal {extra}", extra in names)

        for ch in proto.CHANNELS[node]:
            sig = msg.get_signal_by_name(ch.name)
            close(f"node {node} {ch.name} scale", sig.scale, ch.scale, 1e-9)
            close(f"node {node} {ch.name} offset", sig.offset, ch.offset, 1e-9)
            check(f"node {node} {ch.name} unit", sig.unit, ch.unit)
            check(f"node {node} {ch.name} is signed", sig.is_signed, True)
            check(f"node {node} {ch.name} length", sig.length, 16)
            check(f"node {node} {ch.name} byte order", sig.byte_order, "little_endian")

    # ---- the real test: decode what the firmware would send ----
    random.seed(20260801)

    for trial in range(200):
        node = trial % proto.NODE_COUNT
        can_id = proto.CAN_DATA_BASE_ID + node
        msg = db.get_message_by_frame_id(can_id)
        defs = proto.CHANNELS[node]

        raw = tuple(random.randint(-32768, 32767) for _ in range(proto.CH_PER_NODE))
        seq = random.randint(0, 255)
        status = random.randint(0, 7)

        payload = proto.encode_can(raw, seq, status)
        decoded = msg.decode(payload)

        for d, r in zip(defs, raw):
            want = d.to_eng(r)
            got = float(decoded[d.name])
            # Scale factors are exact binary-representable only sometimes;
            # half a least-significant bit is the honest tolerance.
            close(f"trial {trial} node {node} {d.name}", got, want, abs(d.scale) * 0.51)

        check(f"trial {trial} seq", int(decoded["seq"]), seq)

        for bit, name in ((0, "sensor_fault"), (1, "startup"), (2, "can_error")):
            want = bool(status & (1 << bit))
            # The VAL_ table makes cantools hand back a NamedSignalValue
            # ("yes"/"no") rather than a bare int; .value is the raw number.
            raw_val = decoded[name]
            got = bool(int(getattr(raw_val, "value", raw_val)))
            check(f"trial {trial} {name}", got, want)

    # ---- encode direction, so the DBC is usable for injection too ----
    for node in range(proto.NODE_COUNT):
        can_id = proto.CAN_DATA_BASE_ID + node
        msg = db.get_message_by_frame_id(can_id)
        defs = proto.CHANNELS[node]

        values = {d.name: d.to_eng(1000) for d in defs}
        values.update({"seq": 7, "sensor_fault": 1, "startup": 0, "can_error": 1})

        encoded = msg.encode(values)
        chans, seq, status = proto.decode_can(encoded)

        check(f"node {node} encode round-trips channels", chans, (1000, 1000, 1000))
        check(f"node {node} encode round-trips seq", seq, 7)
        check(f"node {node} encode round-trips status", status, 0b101)

    print(f"  {checks - len(failures)}/{checks} checks passed")

    if failures:
        print(f"\n  {len(failures)} FAILURE(S):\n")
        for f in failures[:20]:
            print(f"   - {f}")
        if len(failures) > 20:
            print(f"   ... and {len(failures) - 20} more")
        return 1

    print("\n  cantools decodes the bus exactly as the firmware packs it.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
