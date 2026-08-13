#!/usr/bin/env python3
"""
Check simulation/vehicle_data.py agrees with the v2 channel table.

    python tools/test_channel_sync.py

simulation/vehicle_data.py's NODES list and protocol/telemetry_proto_v2.c's
TLM2_CHANNELS table define the SAME 24 channels across the same 4 nodes,
independently, in two different languages. vehicle_data.py's own docstring
says as much ("intentionally decoupled from protocol/telemetry_proto.c ...
this module exists purely so the themed dashboards can be judged against
data shaped like what the finished car will actually produce") - which is
a real reason for two files to exist, but not a reason for them to be
allowed to silently disagree about names, units, or channel counts. If they
drift, a dashboard built against one and firmware built against the other
would each look correct in isolation and disagree in a demo.

There is no shared source of truth to diff against (no generated-from-C
Python module for v2 yet - see V2_DESIGN_NOTES.md, "No proto_v2.py yet"),
so this reads telemetry_proto_v2.c directly with the same regex parser
tools/gen_dbc_v2.py uses, and simulation/vehicle_data.py by importing it as
a real Python module (it has no side effects on import), then compares.

This is deliberately NOT folded into test_dbc_v2.py: that suite is about
the DBC vs. the C table; this one is about the C table vs. the simulator,
a completely different pair of files with a completely different failure
mode (a renamed/rescaled dashboard channel, not a wire-format bug).
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "simulation"))

import gen_dbc_v2 as gen  # noqa: E402  (reuse its regex-based C table parser)
import vehicle_data  # noqa: E402

failures: list[str] = []
checks = 0


def check(label: str, cond: bool, detail: str = "") -> None:
    """Record one pass/fail. `detail` is appended only on failure, so the
    common case (everything matches) stays quiet and a mismatch says
    exactly what disagreed and in which file."""
    global checks
    checks += 1
    if not cond:
        failures.append(f"{label}: {detail}" if detail else label)


def main() -> int:
    print("Checking simulation/vehicle_data.py against "
          "protocol/telemetry_proto_v2.c's channel table\n")

    c_text = gen.PROTO_C.read_text(encoding="utf-8")
    array_order = gen.parse_node_table_order(c_text)
    c_channels = [gen.parse_channel_array(c_text, a) for a in array_order]
    c_node_count = len(array_order)

    sim_nodes = vehicle_data.NODES
    sim_node_count = len(sim_nodes)

    # ---- node count ----
    check(
        "node count matches",
        c_node_count == sim_node_count,
        f"telemetry_proto_v2.c (TLM2_CHANNELS) has {c_node_count} nodes, "
        f"simulation/vehicle_data.py (NODES) has {sim_node_count}",
    )

    n = min(c_node_count, sim_node_count)

    for node in range(n):
        c_chans = c_channels[node]
        sim_label, sim_chans = sim_nodes[node]

        # ---- per-node channel count ----
        check(
            f"node {node} ({sim_label}) channel count matches",
            len(c_chans) == len(sim_chans),
            f"telemetry_proto_v2.c node {node} has {len(c_chans)} channels "
            f"{[c['name'] for c in c_chans]}, "
            f"vehicle_data.py node {node} ({sim_label!r}) has "
            f"{len(sim_chans)} channels {[c.name for c in sim_chans]}",
        )

        # ---- per-channel name + unit, in order ----
        m = min(len(c_chans), len(sim_chans))
        for i in range(m):
            c_ch = c_chans[i]
            sim_ch = sim_chans[i]

            check(
                f"node {node} channel {i} name matches",
                c_ch["name"] == sim_ch.name,
                f"telemetry_proto_v2.c node {node} channel {i} is "
                f"{c_ch['name']!r}, vehicle_data.py node {node} channel {i} "
                f"is {sim_ch.name!r} - either the order or the name itself "
                f"has drifted between the two files",
            )

            check(
                f"node {node} channel {i} ({c_ch['name']}) unit matches",
                c_ch["unit"] == sim_ch.unit,
                f"telemetry_proto_v2.c channel {c_ch['name']!r} has unit "
                f"{c_ch['unit']!r}, vehicle_data.py channel {sim_ch.name!r} "
                f"has unit {sim_ch.unit!r}",
            )

        # Channels present in one file's node but not checked above because
        # the counts didn't match - call them out individually so the
        # failure says which specific channel is extra/missing, not just
        # "count mismatch".
        if len(c_chans) != len(sim_chans):
            c_names = [c["name"] for c in c_chans]
            sim_names = [c.name for c in sim_chans]
            only_in_c = [nm for nm in c_names if nm not in sim_names]
            only_in_sim = [nm for nm in sim_names if nm not in c_names]
            if only_in_c:
                check(
                    f"node {node} channels only in telemetry_proto_v2.c",
                    False,
                    f"{only_in_c} are in TLM2_CHANNELS[{node}] but not in "
                    f"vehicle_data.NODES[{node}] ({sim_label!r})",
                )
            if only_in_sim:
                check(
                    f"node {node} channels only in vehicle_data.py",
                    False,
                    f"{only_in_sim} are in vehicle_data.NODES[{node}] "
                    f"({sim_label!r}) but not in TLM2_CHANNELS[{node}]",
                )

    print(f"  {checks - len(failures)}/{checks} checks passed")

    if failures:
        print(f"\n  {len(failures)} FAILURE(S):\n")
        for f in failures:
            print(f"   - {f}")
        return 1

    print("\n  simulation/vehicle_data.py and telemetry_proto_v2.c's channel "
          "table agree on node count, channel count, names and units.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
