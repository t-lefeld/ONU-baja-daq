# Wiring a `firmware/sensors/` driver into a node — v2 protocol

**Status: process document for scaffold code.** Everything this file
describes — `protocol/telemetry_proto_v2.h/.c`, every driver under
`firmware/sensors/`, and the v2 CAN paging scheme — is written and
host-tested, but **nothing in `can_node.c` or `telemetry_hub.c` calls into
any of it today.** The v1 system (3 channels × 3 nodes) is what's actually
running on the three Bluepills and the hub — see the root `README.md` and
`firmware/bluepill_node/INTEGRATION.md` for that, and don't let anything
below suggest otherwise.

This is the companion to `firmware/bluepill_node/INTEGRATION.md`, for the
day you're ready to start migrating a node from the v1 simulator to a real
sensor on the v2 protocol. Same ground rule as that document and as
`firmware/sensors/README.md`: **this file does not assign pins, calibration
constants, or node IDs for you.** It only walks through the mechanical steps
so that when you do sit down and go through it by hand, you know exactly
where each decision needs to land.

## Before you start

Read, in this order:

1. `firmware/sensors/README.md` — the sensor → node → file map and the
   per-driver confidence ratings.
2. The header of the specific driver you're wiring (e.g.
   `firmware/sensors/suspension_pot.h`) — every `TODO` in it is a decision
   you have to make on the bench, not in this document.
3. `protocol/V2_DESIGN_NOTES.md` — the CAN paging scheme, why it exists, and
   the open questions at the bottom (some of them are directly relevant to
   step 5 below).

## The process

Five steps, same shape for every driver, whether it's a single ADC read or
the multi-message ODrive CAN listener.

### 1. Configure the peripheral in CubeMX

Each driver's struct tells you what it needs — it's usually a HAL handle
type in the first field. What each one needs, from reading the headers:

| Driver | Node (v2) | Peripheral CubeMX must configure | Notes |
|---|---|---|---|
| `gps_neo_m8n` | Hub | USART (RX only strictly needed), optionally DMA | Driver doesn't own the peripheral — it never calls a HAL UART function itself. You feed it bytes via `gps_feed_byte()` (one at a time, e.g. from `HAL_UART_RxCpltCallback`) or `gps_feed()` (a whole DMA buffer at once). |
| `imu_bno080` | Hub | I2C, plus 2 plain GPIO (INT, RST) | RST must be pulsed low then released by the caller *before* `bno080_init()` — that GPIO toggle is board-specific and deliberately left outside the driver. |
| `wheel_encoder` | Front (fl, fr), Rear (rl, rr) | GPIO + EXTI, one pin per wheel (4 instances total across the two boards) | Trigger edge (falling vs. both) depends on the 55075's output polarity — check on the bench with a scope before committing to the `.ioc` setting. |
| `suspension_pot` | Front (fl, fr), Rear (rl, rr) | ADC channel, one per corner (4 instances total) | Multiple corners can share one ADC peripheral as different channels/ranks — see the `TODO` in `suspension_pot_read()` about re-selecting the channel with `HAL_ADC_ConfigChannel()` before each conversion if it does. |
| `pressure_transducer` | Front | ADC channel, likely sharing the ADC peripheral used by that board's `suspension_pot` instances | The sensor outputs 0.5–4.5 V; needs a voltage divider sized *before* any calibration constant means anything — see the header. |
| `cvt_temp_mlx90614` | Rear | I2C | Fixed 7-bit address `0x5A`, no strapping to check. |
| `odrive_can` | E-CVT / Motor | **None new** — reuses whatever CAN peripheral is already configured on the board that receives ODrive traffic | Doesn't own a peripheral at all. See the Open Questions section below for which physical board that actually is. |

The `.ioc` file itself is still yours to edit by hand — nothing here touches
it, the same way v1's `CUBEMX_SETUP.md` doesn't touch it either.

### 2. Fill in the driver's `TODO` constants

`grep -n TODO firmware/sensors/<driver>.h firmware/sensors/<driver>.c` gets
you the complete list for that sensor — pin/channel assignment, calibration
constants (usually two voltages or a physical measurement you take by hand),
and for `odrive_can` specifically, the ODrive's actual configured
`axis_node_id`.

### 3. Call the driver's read function

Every driver in this folder returns a plain `float` in physical units and
knows nothing about CAN:

```c
float travel_mm    = suspension_pot_read(&sp_fl);        /* suspension_pot.h  */
float speed_mph     = wheel_encoder_update(&enc_fl);       /* wheel_encoder.h   */
float psi            = pressure_transducer_read(&pt_front); /* pressure_transducer.h */
bool  ok = cvt_temp_read(&cvt, &degc);                      /* cvt_temp_mlx90614.h, false on I2C error */
```

`gps_neo_m8n` and `imu_bno080` are push-style instead (you feed them bytes /
poll them and read fields off the struct directly — `g.lat_deg`,
`imu.accel_x_g`, etc. — rather than calling a single blocking read
function), but the output is the same shape: plain floats in a struct, no
CAN awareness.

### 4. Convert the float to the v2 channel's raw integer

This is the step v1's `sim_to_raw()` in `can_node.c` already does for the
old protocol — same idea, different table. Every channel's conversion is
`engineering_value = raw * scale + offset`, so going the other direction:

```c
raw = round((value - offset) / scale)
```

Look up `scale`, `offset`, and `width` for your channel in
`TLM2_CHANNELS[node_id][channel_index]` (`protocol/telemetry_proto_v2.c`) —
**not** by copying a number from this file, since that table is the single
source of truth both the packer and the reassembler read from.

Two things worth doing explicitly here that v1's `sim_to_raw()` also does,
because `tlm2_can_pack_pages()` does **not** do them for you:

- **Clamp before packing.** `tlm2_can_pack_pages()` truncates (wraps) an
  out-of-range raw value rather than saturating it — this is called out as
  an open item in `protocol/V2_DESIGN_NOTES.md`. A bad calibration constant
  should peg a channel at its max, not wrap it into a plausible-looking
  wrong number. Clamp to `[-32768, 32767]` for a `TLM2_W_I16` channel, or
  `[-2147483648, 2147483647]` for `TLM2_W_I32` (GPS lat/lon only, today).
- **Round, don't truncate**, the same `>= 0 ? +0.5f : -0.5f` trick
  `sim_to_raw()` uses — plain `(int32_t)` cast truncates toward zero, which
  quietly biases every negative reading.

### 5. Hand it to the v2 packer and transmit the pages

`tlm2_can_pack_pages()` packs **one whole node's** channel set per call, not
one channel at a time — so by the time you call it, you need every channel
for that node in one `int32_t` array, in the exact order they're declared in
`TLM2_CHANNELS[node_id]`. If you're wiring sensors in one at a time, the
channels you haven't gotten to yet still need *some* value in that array
(0, or the last known value) — see the worked example below.

```c
uint8_t pages[TLM2_MAX_PAGES_PER_NODE][TLM2_CAN_DLC];
uint8_t page_count = tlm2_can_pack_pages(node_id, raw, n_raw,
                                          epoch, status, pages);

for (uint8_t p = 0; p < page_count; p++) {
    CAN_TxHeaderTypeDef hdr = {
        .StdId = TLM2_CAN_ID_FOR(node_id, p),
        .IDE   = CAN_ID_STD,
        .RTR   = CAN_RTR_DATA,
        .DLC   = TLM2_CAN_DLC,
    };
    uint32_t mailbox;
    HAL_CAN_AddTxMessage(hcan, &hdr, pages[p], &mailbox);
    /* same non-blocking / drop-rather-than-wait pattern can_node.c already
       uses for v1 — see can_node_task() for the mailbox-full handling. */
}
```

**`epoch` increments once per burst, not once per page.** Every page you
just got back from one `tlm2_can_pack_pages()` call shares the same `epoch`
value — that's what lets the hub's reassembler tell a complete set of pages
from a torn one. Increment `epoch` only after this whole loop finishes, the
same cadence `can_node_task()` uses for v1's `seq`.

### Hub-side: the receiving half

None of this exists in `telemetry_hub.c` yet either — it's the mirror image
of steps 3–5, run once per received CAN frame plus once per radio period:

```c
/* once per received v2-range CAN frame (0x200-0x21F): */
uint8_t node_id = TLM2_CAN_NODE_FROM_ID(rx_header.StdId);
uint8_t page_idx = TLM2_CAN_PAGE_FROM_ID(rx_header.StdId);
tlm2_reasm_apply_page(&reassemblers[node_id], node_id, page_idx,
                       rx_data, HAL_GetTick());

/* once per TLM2_FRAME_PERIOD_MS, for each node: */
tlm2_node_record_t rec;
tlm2_reasm_snapshot(&reassemblers[node_id], node_id, HAL_GetTick(), &rec);
/* ... assemble a tlm2_frame_t from all 4 records, then: */
size_t n = tlm2_encode_frame(&frame, txbuf, sizeof(txbuf));
/* send txbuf over the E22, same as v1's build_frame()/lora_e22 path */
```

`tools/test_roundtrip_v2.c` exercises this exact pipeline end to end
(pack → CAN pages → reassemble → snapshot → radio-encode → decode) on the
host — read it alongside this section if you want to see it working before
committing to writing the real ISR/main-loop code.

## Worked example: suspension pot, front-left corner

**Reference only — this is not a file in the repo and is not meant to
compile as-is.** It assumes CubeMX is already configured (an ADC channel
picked for this pin — that's your decision, step 1 above) and that
`sp_fl.volts_at_min_travel` / `volts_at_max_travel` / `max_travel_mm` have
already been calibrated by hand per `suspension_pot.h`'s header comment.

```c
#include "suspension_pot.h"
#include "telemetry_proto_v2.h"

/* Front node = index 1 in TLM2_CHANNELS / TLM2_NODE_NAMES (Hub=0, Front=1,
 * Rear=2, Motor=3 - see telemetry_proto_v2.c). FRONT_CHANNELS is declared
 * as: [0] wheel_speed_fl [1] wheel_speed_fr [2] suspension_fl
 *      [3] suspension_fr [4] brake_pressure_f
 * suspension_fl is { "suspension_fl", "mm", scale=0.01f, offset=0.0f,
 * width=TLM2_W_I16 } - i.e. raw units of 0.01 mm, range +/-327.67 mm. */

#define FRONT_NODE_ID   1u
#define CH_SUSPENSION_FL 2u

static suspension_pot_t sp_fl;   /* configured elsewhere via
                                     suspension_pot_init(), step 1/2 above */
static uint8_t s_epoch;
static uint8_t s_status = TLM2_ST_STARTUP;

/* Clamp-and-round, mirroring can_node.c's sim_to_raw() for v1 - see step 4
 * above for why tlm2_can_pack_pages() does not do this for you. */
static int32_t eng_to_raw_i16(float value, float scale, float offset)
{
    float raw = (value - offset) / scale;
    if (raw >  32767.0f) raw =  32767.0f;
    if (raw < -32768.0f) raw = -32768.0f;
    return (int32_t)(raw >= 0.0f ? raw + 0.5f : raw - 0.5f);
}

void front_node_tx_burst(CAN_HandleTypeDef *hcan)
{
    /* Step 3: read the sensor. */
    float travel_mm = suspension_pot_read(&sp_fl);

    /* Step 4: engineering value -> raw int16, using this channel's own
     * scale/offset from TLM2_CHANNELS - hardcoded 0.01f/0.0f here only to
     * keep this example self-contained; real code should read them from
     * TLM2_CHANNELS[FRONT_NODE_ID][CH_SUSPENSION_FL] directly so it can
     * never drift out of sync with the table. */
    int32_t raw[5];
    raw[0] = 0;   /* wheel_speed_fl  - not wired yet */
    raw[1] = 0;   /* wheel_speed_fr  - not wired yet */
    raw[CH_SUSPENSION_FL] = eng_to_raw_i16(travel_mm, 0.01f, 0.0f);
    raw[3] = 0;   /* suspension_fr   - not wired yet */
    raw[4] = 0;   /* brake_pressure_f - not wired yet */

    /* Step 5: pack this node's whole channel set into its CAN pages. */
    uint8_t pages[TLM2_MAX_PAGES_PER_NODE][TLM2_CAN_DLC];
    uint8_t page_count = tlm2_can_pack_pages(FRONT_NODE_ID, raw, 5,
                                              s_epoch, s_status, pages);

    for (uint8_t p = 0; p < page_count; p++) {
        CAN_TxHeaderTypeDef hdr = {
            .StdId = TLM2_CAN_ID_FOR(FRONT_NODE_ID, p),
            .IDE   = CAN_ID_STD,
            .RTR   = CAN_RTR_DATA,
            .DLC   = TLM2_CAN_DLC,
        };
        uint32_t mailbox;
        HAL_CAN_AddTxMessage(hcan, &hdr, pages[p], &mailbox);
    }

    s_epoch++;   /* once per burst, after every page of THIS burst is sent */
}
```

Every other ADC-based driver (`pressure_transducer`) follows the identical
shape. The I2C drivers (`cvt_temp_mlx90614`, `imu_bno080`) and the UART one
(`gps_neo_m8n`) differ only in step 3 — how you get the float out — steps
4–5 are unchanged.

## Open questions: the E-CVT / Motor node has no physical Bluepill

`TLM2_NODE_COUNT` is 4 (Hub, Front, Rear, Motor/E-CVT) and
`V2_DESIGN_NOTES.md`'s CAN ID map reserves a full 8-ID paging block
(`0x218`–`0x21F`, per `TLM2_CAN_ID_FOR(3, page)`) for node 3 like it does for
the other three, even though the Motor node's channel table only actually
needs 2 pages (`0x218`–`0x219`) per the page-count table in that same
document. But there are only **three physical Bluepills**, and they map to
Front, Rear, and — before
the redesign that dropped the system from 4 nodes to 3 — what used to be a
4th board. Nothing was ever repurposed as a Motor node board. So the
question is real: what actually transmits node 3's CAN pages?

Reading `firmware/sensors/odrive_can.h` and the sensor README together, the
answer implied by the existing scaffold is: **nothing does, because nothing
needs to.** The ODrive S1 is already its own node on the same physical CAN
bus (`odrive_can.h`: "it is NOT behind a Bluepill... this driver just needs
to listen for the ODrive's own CAN Simple messages on whichever bus the hub
is already receiving from"). `odrive_can_handle_frame()` is meant to be
called straight from whichever board's existing CAN RX path already sees
the ODrive's native CAN Simple traffic — realistically the hub, since that's
the board with the aggregation logic and the radio. That board would decode
`odrive_can_t`'s fields locally and build node 3's `tlm2_node_record_t`
**directly**, the same way `firmware/sensors/README.md` already says the
Hub's own GPS/IMU channels (node 0) get read locally rather than received
over CAN — "In `can_node.c` (or `telemetry_hub.c` for the Hub's
directly-attached GPS/IMU), replace the relevant channel(s)..."

If that reading is right, **two of the four v2 "nodes" (Hub and Motor) never
actually send CAN pages at all** — only Front and Rear, which really are
separate physical boards, go through `tlm2_can_pack_pages()` /
`tlm2_reasm_apply_page()`. Node 0 and node 3's CAN ID ranges in the paging
scheme (`0x200`–`0x207` and `0x218`–`0x21F`) would sit unused, and their
`tlm2_node_record_t`s would be built by hand in hub code from local sensor
reads (GPS/IMU) and from `odrive_can_t` fields, then dropped straight into
the same `tlm2_frame_t` alongside the two records that did arrive over CAN.

**This is genuinely unresolved, not just undocumented:**

- No code anywhere — `telemetry_hub.c`, `telemetry_proto_v2.c`, or
  `odrive_can.c` — actually builds a `tlm2_node_record_t` by hand this way.
  The only record-building path that exists today is
  `tlm2_reasm_snapshot()`, which assumes pages arrived over CAN.
- If Hub and Motor really do bypass the CAN-paging path, `tlm2_reasm_*` is
  only ever needed for 2 of the 4 nodes, which is worth knowing before
  building out the hub-side reassembler array in step 5 above — don't
  allocate and drive 4 reassemblers if 2 of them will sit permanently empty.
- The alternative — the hub loops its own GPS/IMU/ODrive readings back onto
  the CAN bus as synthetic TLM2 pages just so every node's data arrives
  through one uniform path — was not found written down anywhere, and would
  be a strange thing to do (transmitting CAN pages to yourself, on a bus
  you already share with two other real transmitters, for data you already
  had in hand) but isn't explicitly ruled out either.
- The ODrive's `axis_node_id` isn't pinned down in code (see
  `odrive_can.h`), which is a separate but related prerequisite — you can't
  wire up node 3 at all, by either path, until that's fixed and confirmed
  against the CAN ID collision risk `V2_DESIGN_NOTES.md` already flags at
  `0x200` (`axis_node_id` 16).

Whoever picks this back up should treat "how does node 3's record actually
get built" as a design decision to make explicitly — in a comment in
`telemetry_hub.c` at minimum — rather than something to discover by reading
four different files and inferring it, the way this section just did.
