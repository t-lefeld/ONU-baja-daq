# telemetry_proto_v2 - design notes

Scaffold for a >3-channel-per-node CAN protocol, targeting the layout in
`simulation/vehicle_data.py` (Hub 9 ch, Front/Rear/Motor 5 ch each). Nothing
in `firmware/` or `pc_app/` calls into this yet - see `firmware/sensors/README.md`
for why that wiring is deliberately left for later, once pins are assigned.

**v1 (`telemetry_proto.h/.c`) is untouched.** This is a separate file pair,
`TLM2_` / `tlm2_` namespaced throughout, so it can be `#include`d alongside
v1 without any symbol collision. That's not just tidiness - it means a
mixed-fleet bring-up (some boards still on v1, one board being migrated to
v2) is representable without a flag day.

## Why not just extend v1

v1's CAN payload is `int16 ch[3] + seq + status` in exactly 8 bytes, and its
radio record is `node_id/flags/seq/loss + int16 ch[3]` in exactly 10 bytes.
Both are fixed-shape with zero room to say "how many channels" or "how wide."
Changing what the bytes mean on a proven, hardware-verified wire format is
exactly the kind of edit the brief for this work says not to make. A new
version byte and a new file pair costs nothing and keeps v1's guarantees
intact.

## CAN layer: paging

A CAN frame is 8 bytes; a node can need up to 9 channels (36 bytes if all
were int32). So a node's full channel set is split into up to
`TLM2_MAX_PAGES_PER_NODE` (4) consecutive 8-byte "page" frames.

### Where the page index lives

In the CAN ID, not the payload:

```
TLM2_CAN_ID_FOR(node, page) = TLM2_CAN_DATA_BASE_ID + (node << 3) + page
```

Rationale: it lets a hardware CAN filter select "everything from node N" with
one ID/mask pair, mirrors how v1 already folds `node_id` into the ID, and
means the hub's RX ISR knows node+page before it even looks at the payload -
useful because the ISR is the one place in this whole chain where every
cycle spent matters.

### How many pages, and which channels go where

**Not transmitted.** It's a pure, deterministic function of the channel
width table (`TLM2_CHANNELS` in the .c file): walk channels in declared
order, pack into a page's 6 free bytes (8 minus the 2-byte header below)
greedily, never split a channel across a page boundary, start a new page
when the next channel wouldn't fit. Both `tlm2_can_pack_pages()` (the
transmit side) and `tlm2_reasm_apply_page()` (the receive side) call the
same internal `layout_node()` walk, so they can't disagree - there's no
"page count" byte that could get out of sync between firmware builds.

Cost: some trailing bytes per page go unused (e.g. the Hub's page 0 holds
only `gps_lat`, 4 of 6 bytes, because `gps_lon` right after it is also 4
bytes and won't fit in the remaining 2). Benefit: no channel's value ever
depends on reconstructing bytes split across two CAN frames, which would be
a second, nastier kind of tear to handle on top of the page-level one.

### Computed page layout for the current channel tables

| Node | Channels | Pages | Page contents |
|---|---|---|---|
| 0 Hub | 9 (22 B) | 4 | `[lat]` `[lon,speed]` `[heading,sats,accel_x]` `[accel_y,accel_z,gyro_z]` |
| 1 Front | 5 (10 B) | 2 | `[wheel_fl,wheel_fr,susp_fl]` `[susp_fr,brake_f]` |
| 2 Rear | 5 (10 B) | 2 | `[wheel_rl,wheel_rr,susp_rl]` `[susp_rr,cvt_temp]` |
| 3 Motor | 5 (10 B) | 2 | `[current,velocity,temp]` `[bus_v,brake_r_w]` |

10 CAN frames per full cycle across all 4 nodes (vs. 3 today). At the same
10 Hz per-node cadence as v1 that's 800 B/s of CAN payload against a
500 kbit/s (62.5 kB/s) bus - about 1.3% utilization, not a real constraint.

### Torn-set detection: epoch, not per-frame seq

Each page carries `epoch` (byte 0) and `status` (byte 1) ahead of its 6
payload bytes. `epoch` increments once per **burst** (all pages for one
node's transmission cycle), not once per CAN frame - every page in a burst
shares the same value.

The reassembler (`tlm2_node_reassembler_t`) keeps two channel arrays: `ch[]`
(last complete snapshot - safe to read any time) and `ch_building[]`
(whatever's arrived for the *current* epoch so far). A page updates
`ch_building` and sets a bit in a page mask; only when every expected page's
bit is set does `ch_building` get copied into `ch[]` atomically. If a new
epoch shows up before the mask ever fills, whatever was in `ch_building` is
simply dropped - `ch[]` is untouched, so a reader never sees page 0 from one
cycle spliced with page 1 from the next.

This is coarser than v1's per-CAN-frame `seq` byte: v1 can tell you "frame 3
of 10 was lost," v2 can only tell you "burst 3 wasn't usable." That's a
deliberate trade for simplicity here - see Open Questions below for whether
it's worth revisiting.

### CAN ID allocation map

| Range | Owner |
|---|---|
| `0x000-0x0FF` | reserved (v1's own comment: future high-priority frames) |
| `0x100-0x102` | v1 NODE_DATA, nodes 0-2 (unchanged, untouched) |
| `0x103-0x1FF` | free |
| `0x200-0x21F` | **v2 NODE_DATA, nodes 0-3, pages 0-7 each** (`TLM2_CAN_ID_FOR`) |
| `0x220-0x7FF` | free |
| ODrive S1 | `(axis_node_id << 5) \| cmd_id`, wherever the operator configures `axis_node_id` |

**The ODrive line is the one with a real, currently-unresolved risk.**
`firmware/sensors/odrive_can.h` already flags that the ODrive's
`axis_node_id` is operator-set and not yet pinned down. Decoded through the
ODrive's own addressing, `0x200` is `axis_node_id=16, cmd_id=0`. So this
block is collision-free **only if the ODrive stays below node id 16** - true
of the default (0) and any small hand-picked id, but not guaranteed by
anything in code today. v1's own `0x100-0x102` has the identical exposure
at `axis_node_id=8`. Neither this file nor v1 enforces it in software; it's
a configuration convention that needs to be written down wherever the ODrive
actually gets configured (odrivetool / the GUI) and cross-checked against
this table at that time.

## GPS precision: int32, no origin offset

v1's channels are all `int16 * scale`. Latitude/longitude need about 7
decimal degrees for ~1 cm resolution (1e-7 deg ≈ 1.11 cm at the equator).//
An `int16` literally cannot represent that - even at 1 raw count = 1e-7 deg,
the range would be +/-3.2767e-3 degrees, a box a few hundred meters wide.

Two ways to fix it:

1. **Plain `int32`, global range.** `raw = round(deg / 1e-7)`. Range is
   +/-2.147e9 raw, i.e. +/-214.7 degrees - covers the entire +/-180/+/-90
   range with headroom, at full 1.1 cm resolution, everywhere on Earth.
2. **Fixed-point offset from a track-local origin.** Store `raw = round((deg
   - origin_deg) / scale)` in fewer bits, since you only need to represent a
   few hundred meters around one fixed point (`TRACK_CENTER_LAT/LON` in
   `vehicle_data.py`). Could plausibly fit in `int16` at a coarse-enough
   scale, saving 2 bytes/channel.

**Picked option 1.** The byte budget already allows int32 (the frame-size
math below has room), and option 2 trades 2 bytes/channel for a
correctness landmine: it silently produces garbage the moment the car goes
somewhere the origin constant wasn't updated for - a preseason test track,
a different competition site, a trailer GPS check in a parking lot. A wrong
`TRACK_CENTER_LAT` is a compile-time constant that's easy to forget to
change and hard to notice went stale (values still "look like" small
numbers, just wrong ones). Global int32 has no such foot-gun. If flash/RAM
or radio bytes ever get genuinely tight, this is the first thing worth
revisiting - it's a real 4 bytes/channel cost.

## Radio frame size and LoRa airtime

### Size

Unlike v1, per-node record length isn't fixed (9 channels on the Hub, mixed
int16/int32, vs. 5 elsewhere) - so there's no single `TLM2_PAYLOAD_SIZE`
constant. `tlm2_encode_frame()`/`tlm2_decode_frame()` compute the real size
from `TLM2_CHANNELS` at both ends (same "shared table, nothing transmitted"
principle as the CAN paging). For the *current* tables:

```
node 0 (Hub):   4 (hdr) + 22 (9 ch, 2x i32 + 7x i16) = 26 B
node 1 (Front): 4 (hdr) + 10 (5x i16)                = 14 B
node 2 (Rear):  4 (hdr) + 10 (5x i16)                = 14 B
node 3 (Motor): 4 (hdr) + 10 (5x i16)                = 14 B
                                          payload sum = 68 B

frame = 10 (hdr) + 68 (payload) + 2 (crc) = 80 bytes
```

vs. v1's 42 bytes - about 1.9x, driven almost entirely by 24 channels vs. 9
and the two 4-byte GPS fields.

### Airtime at 2.4 kbps

v1's own header comment establishes the number this repo has been using for
the E22 at its default air rate: **~200 usable bytes/second** (2.4 kbit/s
raw, minus module/LoRa framing overhead - see `lora_e22.h`). This repo has
not independently re-measured that figure for a real 80-byte packet; treat
what follows as first-pass arithmetic to catch a design that's obviously
wrong, not as a verified budget. See Open Questions.

At `TLM2_FRAME_PERIOD_MS = 500` (2 Hz, same as v1):

```
average rate:     80 B x 2 Hz = 160 B/s          vs. ~200 B/s cap  -> 80% utilization
single-frame time: 80 B / 200 B/s = 400 ms        vs. 500 ms budget -> 100 ms (20%) slack
```

**It fits, on paper, with thin margin.** For comparison, v1 at 42 B/2 Hz is
84 B/s average (42% utilization) and 210 ms per frame (290 ms / 58% slack) -
much more comfortable. v2's 20%/80% numbers are the kind of margin that
looks fine in a spreadsheet and then doesn't survive contact with real
per-packet LoRa preamble/sync overhead, which the linear "B/s" model above
doesn't capture at all (that overhead is closer to a fixed cost per
transmission than a per-byte one, so it hurts small-ish packets like this
proportionally more than it hurts a hypothetical much-larger one).

**If it doesn't hold up on the bench, in order of how much they cost you:**

1. **Raise the E22 air rate** from `E22_AIR_2K4` to `E22_AIR_4K8`
   (`lora_e22.h` already defines the enum value). Roughly doubles effective
   throughput to ~400 B/s -> ~200 ms/frame, 60% slack. One-line config
   change on both ends, no protocol change - the obvious first thing to try.
   Caveat already on record in `lora_e22.h`: verify the air-rate bit
   encoding against your actual module's manual revision before trusting it
   silently.
2. **Drop `TLM2_FRAME_PERIOD_MS` to 1000 (1 Hz).** Average rate falls to
   80 B/s (40%), and the 400 ms single-frame cost now has a 1000 ms budget
   (60% slack). Costs you dashboard update latency - acceptable for slow
   channels (CVT temp, bus voltage), less obviously fine for anything you
   want to watch in near-real-time during a run.
3. **Send only changed channels** (delta/dirty-bit encoding). Real
   bandwidth savings, since chassis data is mostly slowly varying between
   500 ms ticks. Meaningfully more complex (needs a per-channel dirty
   threshold and a way to express "channel N unchanged" compactly) - not
   attempted here, flagged as a real option for later.
4. **Split nodes across radio cycles** (e.g. Hub+Motor on even ticks,
   Front+Rear on odd ticks). Halves per-frame payload (~40 B) at the cost of
   halving each individual node's *radio* update rate to 1 Hz while keeping
   the CAN bus itself at full rate - a middle ground between options 2 and 3
   in both effort and benefit.

None of these are implemented here. `TLM2_FRAME_PERIOD_MS` is left at 500 to
match v1 for a clean side-by-side comparison; changing it is a one-line edit
once real hardware gives you a real number instead of this estimate.

## Loss / staleness tracking

Same shape as v1, adapted to bursts instead of individual frames:

- `TLM2_NF_ONLINE` - any page arrived since the last radio snapshot.
- `TLM2_NF_STALE` - no *complete* burst in `TLM2_NODE_TIMEOUT_MS` (1500 ms,
  same constant as v1 - 15 missed 100 ms bursts).
- `TLM2_NF_FAULT` - node reported `TLM2_ST_SENSOR_FAULT`.
- `TLM2_NF_PARTIAL` - **new in v2.** The burst that was in flight at
  snapshot time never finished. `ch[]` still holds the last complete set
  (never a torn mix), this flag just tells a human "what you're looking at
  isn't this window's data."
- `loss` - counted in missed/torn **bursts**, via the same unsigned-wrap
  epoch-delta trick v1 uses on `seq` (`(uint8_t)(epoch - last_epoch)`), so a
  torn burst that's later superseded by a completing one still shows up as
  loss instead of vanishing silently.

## Open questions / not yet decided

- **The ~200 B/s LoRa throughput figure is unverified for this payload
  size.** It's carried over from v1's header comment, itself presumably a
  rough estimate rather than a bench measurement. The 20%/80%-utilization
  margin above should be treated as "plausible, not proven" until someone
  puts a real 80-byte frame over a real E22 link and times it.
- **ODrive `axis_node_id` is not pinned down anywhere in code.** The CAN ID
  map above assumes it stays below 16; nothing enforces that. Worth a single
  source of truth (a header, a comment in the .ioc, something) once the
  ODrive is actually configured, covering both v1's and v2's exposure.
- **Per-page loss detail was traded away for burst-level loss.** If it turns
  out you need to know "was it specifically the GPS page or the IMU page
  that keeps dropping," the epoch scheme as designed can't tell you that -
  it's an all-or-nothing burst. Extending it would mean either a per-page
  seq (more bytes) or accepting that granularity is gone.
- **Truncation vs. saturation in `tlm2_can_pack_pages()`.** Right now an
  out-of-range raw value is silently truncated (wraps), matching what
  happens if you skip the clamp v1's `sim_to_raw()` does upstream. v1's
  firmware clamps before packing; nothing here enforces that a future v2
  `can_node.c` does the same. Worth a debug-build assert once real sensors
  are wired in, so a bad calibration constant is loud instead of a quietly
  wrong plot.
- **`TLM2_OFF_LEN` is one byte**, same limitation v1 has. Today's 68-byte
  payload is nowhere near 255, but it's a ceiling nobody's tracking - if
  channels keep growing, this needs to become 2 bytes (a real wire-format
  change, unlike everything else here) before it silently wraps.
- **This file pair is not wired into `run_all_tests.py`.** Its self-test
  (`tools/test_roundtrip_v2.c`) is standalone by design (gcc + run
  directly) so adding it here didn't require touching that existing
  manifest file. Wiring it in is a natural follow-up once there's a Python
  reference implementation to compare against, the way `test_roundtrip.py`
  compares v1's C against `proto.py`.
- **No `proto_v2.py` yet.** v1 has a hand-mirrored Python decoder in
  `pc_app/telemetry/proto.py` that the PC app actually uses; v2 has none.
  Until the PC app needs to speak v2, that's appropriately deferred, but
  it's real work, not a formality - variable-width mixed int16/int32 records
  are more to get byte-exact than v1's fixed 3-int16 layout.
