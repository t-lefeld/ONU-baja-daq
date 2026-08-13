#!/usr/bin/env python3
"""
quantization_report.py - measure how much error the v2 wire format's
int16/int32 quantization introduces on every channel vehicle_data.py
produces, over one full simulated lap.

    python simulation/quantization_report.py

For each of the 24 channels, this samples vehicle_data.sample_frame() at the
same 2 Hz cadence simulation/server.py broadcasts at (FRAME_PERIOD_MS), for
one full LAP_PERIOD_S lap, and for every sample:

    pre  = the value sample_frame() produced (what server.py used to send
           directly as JSON, before this task wired it through proto_v2)
    raw  = ChannelDef.to_raw(pre)      -- quantize to the wire int16/int32
    post = ChannelDef.to_eng(raw)      -- decode back to engineering units

and tracks max(|post - pre|) per channel across the lap - the worst-case
error a dashboard viewer could actually see once the simulator is round-
tripped through the real encode/decode path instead of sending raw floats.

This is NOT a correctness test (tools/test_roundtrip_v2.py already proves
Python agrees with the C encoder byte for byte) - it is a "does this look
right on a gauge" check, run once and read by a human, not wired into
tools/run_all_tests.py.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "pc_app"))
sys.path.insert(0, str(ROOT / "simulation"))

from telemetry import proto_v2 as v2  # noqa: E402
import vehicle_data as vd  # noqa: E402


def main() -> int:
    lap_s = vd.LAP_PERIOD_S
    period_s = vd.FRAME_PERIOD_MS / 1000.0
    n_samples = int(lap_s / period_s)

    print(f"Sampling {n_samples} frames over one {lap_s:.0f}s lap at "
          f"{1 / period_s:.1f} Hz (matches simulation/server.py's cadence)\n")

    # worst[node][chan_idx] = (max_abs_err, pre_at_worst, post_at_worst, t_at_worst)
    worst: list[list[tuple[float, float, float, float]]] = [
        [(0.0, 0.0, 0.0, 0.0) for _ in range(v2.CHAN_COUNT[n])] for n in range(v2.NODE_COUNT)
    ]
    value_range: list[list[tuple[float, float]]] = [
        [(float("inf"), float("-inf")) for _ in range(v2.CHAN_COUNT[n])] for n in range(v2.NODE_COUNT)
    ]

    for i in range(n_samples):
        t = i * period_s
        frame = vd.sample_frame(i, t)
        for node in frame["nodes"]:
            n = node["node_id"]
            defs = v2.CHANNELS[n]
            for c, ch in enumerate(node["channels"]):
                pre = ch["value"]
                d = defs[c]
                raw = d.to_raw(pre)
                post = d.to_eng(raw)
                err = abs(post - pre)

                lo, hi = value_range[n][c]
                value_range[n][c] = (min(lo, pre), max(hi, pre))

                if err > worst[n][c][0]:
                    worst[n][c] = (err, pre, post, t)

    print(f"{'node':<14}{'channel':<20}{'unit':<8}{'scale':<10}{'value range':<22}"
          f"{'max err':<12}{'at t=':<8}")
    print("-" * 96)

    worst_overall = None
    for n in range(v2.NODE_COUNT):
        defs = v2.CHANNELS[n]
        label = v2.NODE_LABELS[n]
        for c, d in enumerate(defs):
            err, pre, post, t = worst[n][c]
            lo, hi = value_range[n][c]
            rng_str = f"[{lo:.3f}, {hi:.3f}]"
            print(f"{label:<14}{d.name:<20}{d.unit:<8}{d.scale:<10.4g}{rng_str:<22}"
                  f"{err:<12.6g}{t:<8.1f}")
            if worst_overall is None or err > worst_overall[0]:
                worst_overall = (err, n, d, pre, post, t)

    print()
    if worst_overall is not None:
        err, n, d, pre, post, t = worst_overall
        print(f"Worst single-channel error across all 24 channels: "
              f"{d.name} ({v2.NODE_LABELS[n]}), |post - pre| = {err:.6g} {d.unit} "
              f"(pre={pre:.6g}, post={post:.6g}, t={t:.1f}s)")

        # Theoretical bound: rounding to the nearest raw count can be off by
        # at most half a raw count, i.e. scale/2.
        bound = d.scale / 2.0
        print(f"Theoretical bound for that channel's scale ({d.scale:.4g}): "
              f"{bound:.6g} {d.unit} (scale/2)")

    return 0


if __name__ == "__main__":
    sys.exit(main())
