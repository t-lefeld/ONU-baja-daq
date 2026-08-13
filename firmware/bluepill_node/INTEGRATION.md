# Bluepill node — integration

CubeMX settings live in **[../CUBEMX_SETUP.md](../CUBEMX_SETUP.md)**. Your
`.ioc` files are untouched; you configure them yourself. This file covers the
source side.

## Files

Already present in each of the three active projects under `firmware/projects/`:

```
Core/Inc/telemetry_proto.h      shared wire format
Core/Inc/can_node.h
Core/Inc/node_id.h              <- the ONLY file that differs per board
Core/Src/telemetry_proto.c
Core/Src/can_node.c
Core/Src/main.c                 rewritten
```

If you would rather all three projects share one copy of the protocol instead of
holding duplicates, add `protocol/` as a linked folder:
Project → Properties → C/C++ General → Paths and Symbols → Source Location →
Link Folder. Three copies that can silently drift apart is the thing worth
avoiding.

## Node identity

`Core/Inc/node_id.h`, already written per project:

| Project | NODE_ID | CAN ID |
|---|---|---|
| 1st Bluepill | 0 | 0x100 |
| 2nd Bluepill | 1 | 0x101 |
| 3rd Bluepill | 2 | 0x102 |

A 4th Bluepill project still sits in `firmware/projects/` with `NODE_ID=3`, but
it is retired: the system was redesigned from 4 CAN nodes to 3, and its
`telemetry_proto.h`/`.c` copies were deliberately left unsynced with the
current protocol. `can_node.h` now `#error`s at compile time for any
`NODE_ID >= TLM_NODE_COUNT` (3), so that project won't build as-is - which is
the point: it stops a stale 4th board from ever being flashed and arbitrating
against a CAN ID nothing else expects.

A header rather than a `-D` build symbol: it is visible when you open the
project, survives CubeMX regeneration, and does not vanish if a build
configuration is ever recreated. `can_node.h` `#error`s if it is missing, so a
misconfigured project fails at compile time rather than at 3 a.m. on the bench.

Two boards flashed with the same ID is the failure mode worth engineering
against — they arbitrate perfectly happily, and all you see is one node whose
sequence counter jumps around.

## What changed from the original firmware

**The main loop no longer blocks.** The old `CAN_Sender_Loop` ended with
`HAL_Delay(200)`, so the CPU spent essentially all of its time parked in a busy
wait and could not service anything else. `can_node_task()` is driven off
`HAL_GetTick()` and returns immediately when there is nothing to do.

**Transmit cadence does not drift.** The schedule advances by a fixed period
rather than "now + period", and resynchronises if it ever falls more than four
periods behind (debugger halt, long blocking call) instead of catching up with
a burst.

**Dropped frames are visible.** If no TX mailbox is free the sample is dropped
rather than waited on — for periodic telemetry the next sample is 100 ms away
and carries fresher data. The sequence number is still consumed, so the hub
sees the gap and reports it as loss. The old code busy-waited up to 50 ms.

**Bus-off recovers.** Belt-and-braces alongside the CubeMX setting.

**Rate is 100 ms, not 200 ms.** Three nodes at 10 Hz is about 3.8 kbit/s on a
500 kbit/s bus — nothing. The hub decouples this from the radio, which cannot
carry anywhere near that.

## Replacing the simulator with real sensors

Two places, nothing else:

1. `sim_read_channels()` in `can_node.c` — read your ADC/I2C/SPI sensor and
   produce three engineering values.
2. `TLM_CHANNELS` in `protocol/telemetry_proto.c` and the mirrored `CHANNELS`
   in `pc_app/telemetry/proto.py` — names, units, scale factors.

Scale factors decide resolution, since values travel as `int16`:

| scale | range | good for |
|---|---|---|
| 0.001 | ±32.767 | volts, amps, g |
| 0.01 | ±327.67 | temperatures, percentages |
| 0.1 | ±3276.7 | pressures |
| 1.0 | ±32767 | rpm, raw counts |

`tools/test_roundtrip.py` checks every channel survives the
engineering → int16 → engineering round trip, so a badly chosen scale shows up
as a test failure.

Deleting the simulator also drops the `sinf`/`cosf`/`fmodf` calls, which is
about 3 KB of libm off a 64 KB part.

## Bus wiring

All five boards need CAN transceivers — SN65HVD230, TJA1050 or similar. The
STM32's CAN peripheral is TTL-level and cannot drive a differential bus.

Exactly **two** 120 Ω terminators, one at each physical end of the bus. Not one
per node. Five terminators presents 24 Ω to the transceivers, which then cannot
pull the bus dominant, and the symptom is a bus that looks wired correctly and
carries nothing.
