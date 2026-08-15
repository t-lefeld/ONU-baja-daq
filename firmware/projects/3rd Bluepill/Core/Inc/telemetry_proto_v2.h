/**
 * telemetry_proto_v2.h - Multi-frame CAN protocol extension, >3 channels/node.
 *
 * v1 (telemetry_proto.h/.c) is proven on real hardware and is NOT touched by
 * this file, at the byte level or otherwise. This is a parallel, independent
 * wire format, deliberately namespaced TLM2_ / tlm2_ throughout so both
 * headers can be included in the same translation unit without collisions -
 * useful during a bring-up period where you might run a v1 node next to a
 * v2 node on the same physical bus while migrating one board at a time.
 *
 * Why a new file instead of extending v1: v1's CAN payload and radio record
 * both hard-code exactly 3 x int16 channels into 8/10 fixed bytes with no
 * room to say "how many channels" or "how wide". Bolting variable channel
 * counts and mixed int16/int32 widths onto that layout would mean changing
 * the meaning of bytes on the wire for hardware that already works. Easier,
 * safer, and more honest to give the new shape its own version byte and let
 * the two coexist or cut over cleanly later.
 *
 * Status: SCAFFOLD. Nothing in firmware/ or pc_app/ calls into this yet.
 * See protocol/V2_DESIGN_NOTES.md for the full writeup (frame layout, CAN ID
 * map, GPS precision decision, LoRa airtime arithmetic, open questions).
 *
 * Same ground rules as v1: C99, no dynamic allocation, no floats in the
 * encode/decode path, HAL-free so this compiles for F1, L4, and host tests.
 * Multi-byte fields are little-endian, written byte-wise so it is
 * alignment-safe and endian-explicit regardless of host/target.
 */

#ifndef TELEMETRY_PROTO_V2_H
#define TELEMETRY_PROTO_V2_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Versioning                                                          */
/* ------------------------------------------------------------------ */

#define TLM2_PROTO_VERSION   0x02u   /* distinct from v1's 0x01 */

/* ------------------------------------------------------------------ */
/* Topology - target layout per simulation/vehicle_data.py             */
/* ------------------------------------------------------------------ */

#define TLM2_NODE_COUNT        4u   /* Hub, Front, Rear, Motor/E-CVT     */

/* Widest node today is the Hub (GPS x4 + IMU x5 = 9). Bump this if you add
   a channel anywhere - everything else derives from the per-node tables in
   telemetry_proto_v2.c, this is just the RAM/wire upper bound. */
#define TLM2_MAX_CH_PER_NODE   9u

/* ------------------------------------------------------------------ */
/* CAN layer - multi-frame ("paged") node transmission                 */
/* ------------------------------------------------------------------ */
/*
 * Same physical bus as v1 (500 kbit/s, standard 11-bit IDs) - this is a
 * different set of node boards on the SAME wire, not a second bus.
 *
 * A node with N > 3 channels cannot fit them in one 8-byte CAN frame, so it
 * sends TLM2_MAX_PAGES_PER_NODE (at most) consecutive frames, one "page" per
 * frame, all sharing one CAN ID *block*:
 *
 *   TLM2_CAN_ID_FOR(node, page) = TLM2_CAN_DATA_BASE_ID
 *                                  + (node << TLM2_CAN_NODE_SHIFT)
 *                                  + page
 *
 * page lives in the low 3 bits of the ID (not the payload) so a hardware
 * CAN filter can select "give me everything from node N" with one mask,
 * and so the hub's ISR knows which page it received without touching the
 * payload at all - same principle v1 uses to fold node_id into the ID.
 *
 * How many pages a node actually sends, and which channels land in which
 * page, is NOT transmitted anywhere. It is a pure function of that node's
 * entry in TLM2_CHANNELS (computed by tlm2_node_page_count() / the internal
 * layout walk in telemetry_proto_v2.c), so the packer and the reassembler
 * independently compute the identical layout from the same compile-time
 * table. Add a channel, rebuild both ends, done - no page-count byte to
 * keep in sync by hand.
 *
 * ---- CAN ID collision with the ODrive S1 --------------------------------
 * The ODrive is ALSO a node on this bus and IDs its own CAN Simple messages
 * as (axis_node_id << 5) | cmd_id (see firmware/sensors/odrive_can.h). Its
 * axis_node_id is operator-configured and, per that file's own TODO, is NOT
 * yet pinned down. That means no fixed CAN ID range is provably collision-
 * free until the ODrive's node id is fixed and documented.
 *
 * With TLM2_CAN_DATA_BASE_ID = 0x200 and TLM2_CAN_NODE_SHIFT = 3, this block
 * spans 0x200-0x21F (4 nodes x 8 IDs). Decoded through the ODrive's own
 * addressing scheme, 0x200 = axis_node_id 16, cmd_id 0. So: as long as the
 * ODrive is configured to an axis_node_id below 16 (the ODrive default is 0,
 * and small hand-picked IDs like 0/1/2 are typical), there is no overlap.
 * TODO: confirm the ODrive's actual configured axis_node_id once it's set
 * and record it here - this comment is an assumption, not a guarantee. The
 * same latent risk already exists for v1 at 0x100-0x102 (axis_node_id 8
 * would collide there); this file doesn't fix that, just flags it, since
 * v1 is off-limits.
 *
 * IDs 0x000-0x1FF stay free of v2 traffic entirely - that is both v1's full
 * range (0x100-0x102) and everything below it, plus headroom - so a bus
 * analyzer trace immediately tells you "under 0x200 is old-protocol or
 * ODrive, at/above 0x200 is new-protocol" without decoding payloads.
 */

#define TLM2_CAN_BITRATE          500000u   /* same physical bus as v1 */
#define TLM2_CAN_DATA_BASE_ID     0x200u
#define TLM2_CAN_NODE_SHIFT       3u        /* 3 bits -> up to 8 pages/node */
#define TLM2_CAN_PAGE_MASK        0x07u
#define TLM2_CAN_DLC              8u
#define TLM2_PAGE_PAYLOAD_SIZE    6u        /* 8 - epoch byte - status byte */
#define TLM2_MAX_PAGES_PER_NODE   4u        /* Hub needs 4 today; see .c    */

#define TLM2_NODE_TX_PERIOD_MS    100u   /* each node bursts all its pages at 10 Hz */
#define TLM2_NODE_TIMEOUT_MS      1500u  /* 15 missed bursts -> offline, mirrors v1 */

#define TLM2_CAN_ID_FOR(node, page) \
    (TLM2_CAN_DATA_BASE_ID + ((uint32_t)(node) << TLM2_CAN_NODE_SHIFT) + \
     ((uint32_t)(page) & TLM2_CAN_PAGE_MASK))

#define TLM2_CAN_NODE_FROM_ID(id) \
    ((uint8_t)(((uint32_t)(id) - TLM2_CAN_DATA_BASE_ID) >> TLM2_CAN_NODE_SHIFT))

#define TLM2_CAN_PAGE_FROM_ID(id) \
    ((uint8_t)(((uint32_t)(id) - TLM2_CAN_DATA_BASE_ID) & TLM2_CAN_PAGE_MASK))

#define TLM2_CAN_ID_IS_NODE(id) \
    ((uint32_t)(id) >= TLM2_CAN_DATA_BASE_ID && \
     (uint32_t)(id) <  TLM2_CAN_DATA_BASE_ID + \
                        ((uint32_t)TLM2_NODE_COUNT << TLM2_CAN_NODE_SHIFT))

/*
 * Page payload, 8 bytes:
 *
 *   [0]     epoch    uint8   increments once per NODE BURST (i.e. once per
 *                             TLM2_NODE_TX_PERIOD_MS cycle), NOT once per CAN
 *                             frame. Every page belonging to the same burst
 *                             carries the same epoch value.
 *   [1]     status   uint8   TLM2_ST_* bitfield, repeated on every page of
 *                             the burst (cheap - one byte - and means a
 *                             receiver that only sees page 0 of a torn set
 *                             still knows the node's status).
 *   [2..7]  payload  6 bytes of packed channel values for THIS page, per
 *                     the layout computed from TLM2_CHANNELS.
 *
 * epoch is the torn-set detector: if the hub is still missing a page from
 * epoch N when a page carrying epoch N+1 shows up, epoch N's set never
 * completed - discard it rather than mixing an old page 0 with a new page 1.
 * This is the multi-frame analogue of v1's per-frame seq byte; see
 * V2_DESIGN_NOTES.md for why epoch is coarser (per-burst, not per-frame) and
 * why that trade was made deliberately.
 */

#define TLM2_ST_OK            0x00u
#define TLM2_ST_SENSOR_FAULT  0x01u  /* node could not read a sensor         */
#define TLM2_ST_STARTUP       0x02u  /* first few seconds after reset        */
#define TLM2_ST_CAN_ERROR     0x04u  /* node saw bus errors / recovered      */

/* ------------------------------------------------------------------ */
/* Channel descriptors                                                 */
/* ------------------------------------------------------------------ */
/*
 * engineering_value = raw * scale + offset, same convention as v1.
 *
 * width is TLM2_W_I16 (2 bytes) or TLM2_W_I32 (4 bytes). Only GPS lat/lon
 * use I32 today - see the comment above TLM2_CHANNELS in the .c file for why.
 */

typedef enum {
    TLM2_W_I16 = 2,
    TLM2_W_I32 = 4,
} tlm2_width_t;

typedef struct {
    const char  *name;
    const char  *unit;
    float        scale;
    float        offset;
    uint8_t      width;   /* tlm2_width_t, in bytes */
} tlm2_chan_def_t;

/* TLM2_CHANNELS[node] is an array of TLM2_CHAN_COUNT[node] descriptors -
   ragged per node, so it is exposed as an array of pointers rather than a
   rectangular 2D array (nodes have 9, 5, 5, 5 channels, not a common width). */
extern const tlm2_chan_def_t *const TLM2_CHANNELS[TLM2_NODE_COUNT];
extern const uint8_t                TLM2_CHAN_COUNT[TLM2_NODE_COUNT];
extern const char *const            TLM2_NODE_NAMES[TLM2_NODE_COUNT];

/* ------------------------------------------------------------------ */
/* Hub-side reassembly                                                 */
/* ------------------------------------------------------------------ */
/*
 * One of these per node, owned by whatever eventually replaces
 * telemetry_hub.c's per-node node_state_t. ch[] only ever holds a complete,
 * internally-consistent set of values from a single epoch - apply_page()
 * builds into a separate scratch array and only publishes into ch[] once
 * every page of the current epoch has arrived, so a caller reading ch[] at
 * any time never sees a torn mix of old and new values.
 */

typedef struct {
    int32_t  ch[TLM2_MAX_CH_PER_NODE];          /* last COMPLETE snapshot */
    int32_t  ch_building[TLM2_MAX_CH_PER_NODE];  /* in-progress epoch     */

    uint16_t page_mask;        /* pages seen so far for ch_building's epoch */
    uint8_t  cur_epoch;        /* epoch ch_building belongs to              */
    uint8_t  status;           /* TLM2_ST_* from the most recent page       */

    uint8_t  last_complete_epoch;
    bool     have_epoch;       /* cur_epoch is meaningful                   */
    bool     have_complete;    /* ch[] holds at least one valid snapshot    */

    uint32_t last_rx_ms;       /* time of the most recent page, any epoch   */
    uint16_t loss_accum;       /* missed/torn bursts since last snapshot()  */
    bool     seen_this_window; /* any page arrived since last snapshot()    */
} tlm2_node_reassembler_t;

/* ------------------------------------------------------------------ */
/* LoRa / USB-serial frame (hub -> PC)                                 */
/* ------------------------------------------------------------------ */
/*
 * Same header shape as v1 (sync/ver/len/seq/t_ms/crc) - see
 * V2_DESIGN_NOTES.md for why the sync bytes are reused (0xA5 0x5A) and the
 * version byte alone is what tells a v1 and a v2 frame apart.
 *
 * Unlike v1, per-node record length is NOT fixed (9 channels on the Hub vs 5
 * elsewhere, mixed widths), so there is no single TLM2_PAYLOAD_SIZE constant
 * the way v1 has TLM_PAYLOAD_SIZE. tlm2_encode_frame()/tlm2_decode_frame()
 * compute the real length from TLM2_CHANNELS at both ends, identically, for
 * the same reason the CAN page layout isn't transmitted either. The _MAX
 * macros below are compile-time upper bounds for sizing static buffers -
 * NOT the actual on-wire size, which is smaller and computed at runtime by
 * tlm2_node_wire_bytes() / returned by tlm2_encode_frame().
 */

#define TLM2_SYNC0             0xA5u   /* same bytes as v1 - version differs */
#define TLM2_SYNC1             0x5Au

#define TLM2_FRAME_PERIOD_MS   500u    /* 2 Hz, matches v1 for now - see notes */

#define TLM2_HDR_SIZE           10u
#define TLM2_CRC_SIZE            2u
#define TLM2_REC_HDR_SIZE        4u    /* node_id, flags, epoch, loss */

/* Worst case: every node has TLM2_MAX_CH_PER_NODE channels, all I32. Actual
   wire size is far smaller - see V2_DESIGN_NOTES.md for the real number. */
#define TLM2_MAX_REC_SIZE       (TLM2_REC_HDR_SIZE + TLM2_MAX_CH_PER_NODE * 4u)
#define TLM2_PAYLOAD_SIZE_MAX   (TLM2_NODE_COUNT * TLM2_MAX_REC_SIZE)
#define TLM2_FRAME_SIZE_MAX     (TLM2_HDR_SIZE + TLM2_PAYLOAD_SIZE_MAX + TLM2_CRC_SIZE)

#define TLM2_OFF_SYNC0   0u
#define TLM2_OFF_SYNC1   1u
#define TLM2_OFF_VER     2u
#define TLM2_OFF_LEN     3u
#define TLM2_OFF_SEQ     4u
#define TLM2_OFF_TMS     6u
#define TLM2_OFF_PAYLOAD 10u

#define TLM2_NF_ONLINE   0x01u  /* heard from this node since the last snapshot */
#define TLM2_NF_STALE    0x02u  /* values older than TLM2_NODE_TIMEOUT_MS       */
#define TLM2_NF_FAULT    0x04u  /* node reported TLM2_ST_SENSOR_FAULT           */
#define TLM2_NF_PARTIAL  0x08u  /* current burst incomplete at snapshot time -  */
                                 /* ch[] is the last GOOD set, not garbage, but  */
                                 /* it is not this window's data                */

typedef struct {
    uint8_t  node_id;
    uint8_t  flags;      /* TLM2_NF_* */
    uint8_t  epoch;       /* last_complete_epoch at snapshot time */
    uint8_t  loss;        /* missed/torn bursts since previous snapshot, sat 255 */
    int32_t  ch[TLM2_MAX_CH_PER_NODE];  /* only [0 .. TLM2_CHAN_COUNT[node]) valid */
} tlm2_node_record_t;

typedef struct {
    uint16_t            seq;
    uint32_t            t_ms;
    tlm2_node_record_t  nodes[TLM2_NODE_COUNT];
} tlm2_frame_t;

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

/** CRC-16/CCITT-FALSE, identical algorithm to v1's tlm_crc16(). Reimplemented
 *  rather than shared so this file has zero dependency on telemetry_proto.h -
 *  it should be possible to delete v1 entirely someday without touching v2. */
uint16_t tlm2_crc16(const uint8_t *data, size_t len);

/** Number of CAN pages this node's channel table packs into, given the
 *  greedy no-split layout (see telemetry_proto_v2.c). Never 0 for a valid
 *  node_id, never more than TLM2_MAX_PAGES_PER_NODE. */
uint8_t tlm2_node_page_count(uint8_t node_id);

/** Total bytes this node's channels occupy on the wire (sum of widths). */
size_t tlm2_node_wire_bytes(uint8_t node_id);

/**
 * Pack one node's full channel set into its CAN pages.
 *
 * @p raw       TLM2_CHAN_COUNT[node_id] raw values, already scaled (same
 *              convention as v1: caller applies scale/offset and clamps to
 *              the channel's width BEFORE calling this - see sim_to_raw() in
 *              can_node.c for the pattern. Values that don't fit their
 *              channel's width are silently truncated here, not saturated;
 *              TODO: decide whether that truncation should become a hard
 *              assert in a debug build once real sensors are wired in, so a
 *              bad calibration constant is loud instead of a quietly wrong
 *              plot.
 * @p n_raw     length of @p raw, must equal TLM2_CHAN_COUNT[node_id].
 * @p epoch     shared across every page of this burst.
 * @p pages_out must hold TLM2_MAX_PAGES_PER_NODE entries; only the first
 *              N (the return value) are meaningful.
 *
 * Returns the number of pages written (== tlm2_node_page_count(node_id)),
 * or 0 if node_id or n_raw is invalid.
 */
uint8_t tlm2_can_pack_pages(uint8_t node_id,
                            const int32_t *raw, uint8_t n_raw,
                            uint8_t epoch, uint8_t status,
                            uint8_t pages_out[][TLM2_CAN_DLC]);

/** Reset a reassembler to "never seen a page" state. */
void tlm2_reasm_init(tlm2_node_reassembler_t *r);

/**
 * Feed one received CAN page (node_id and page_idx already pulled from the
 * CAN ID via TLM2_CAN_NODE_FROM_ID / TLM2_CAN_PAGE_FROM_ID) into the
 * reassembler for that node.
 *
 * Returns true if this page completed the set for its epoch - i.e. r->ch[]
 * was just published with a fresh, fully-consistent snapshot. Returns false
 * otherwise, including when this page belongs to a new epoch that starts a
 * fresh (still-incomplete) burst.
 */
bool tlm2_reasm_apply_page(tlm2_node_reassembler_t *r,
                           uint8_t node_id, uint8_t page_idx,
                           const uint8_t d8[TLM2_CAN_DLC], uint32_t now_ms);

/**
 * Build this node's tlm2_node_record_t from the reassembler's current state
 * (flags, staleness, loss, the last complete channel set) and reset the
 * per-window counters (loss_accum, seen_this_window), same lifecycle as v1's
 * build_frame() draining node_state_t. Call once per TLM2_FRAME_PERIOD_MS.
 */
void tlm2_reasm_snapshot(tlm2_node_reassembler_t *r, uint8_t node_id,
                         uint32_t now_ms, tlm2_node_record_t *out);

/**
 * Serialise a frame. Returns the actual number of bytes written on success
 * (always <= TLM2_FRAME_SIZE_MAX), 0 if out_sz is too small.
 */
size_t tlm2_encode_frame(const tlm2_frame_t *f, uint8_t *out, size_t out_sz);

/**
 * Parse exactly one frame from the start of @p buf.
 * Returns 0 on success, negative on failure:
 *   -1 too short   -2 bad sync   -3 bad version
 *   -4 bad length  -5 bad CRC
 */
int tlm2_decode_frame(const uint8_t *buf, size_t len, tlm2_frame_t *out);

/**
 * Scan @p buf for the first byte offset at which a complete, CRC-valid frame
 * begins. Returns the offset, or -1 if none found.
 */
int tlm2_find_frame(const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* TELEMETRY_PROTO_V2_H */
