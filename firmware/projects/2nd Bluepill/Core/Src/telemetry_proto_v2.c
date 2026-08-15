/**
 * telemetry_proto_v2.c - Implementation of the multi-frame wire format.
 *
 * No HAL dependencies, no dynamic allocation, no floating point in the
 * encode/decode/pack/unpack/reassembly path (float only appears in the
 * channel descriptor table itself, same as v1 - it's metadata a caller uses
 * to convert engineering units to raw integers BEFORE this code ever runs).
 */

#include "telemetry_proto_v2.h"
#include <string.h>

/* ------------------------------------------------------------------ */
/* Channel tables - target layout per simulation/vehicle_data.py       */
/* ------------------------------------------------------------------ */
/*
 * Order within each node matches NODES in vehicle_data.py exactly, so a
 * side-by-side diff of that file and this table is the whole review.
 *
 * Width choice: everything is I16 except gps_lat/gps_lon, which are I32.
 * See "GPS precision" in V2_DESIGN_NOTES.md for the full writeup; short
 * version: 1e-7 deg is ~1.1 cm at the equator, and int32 * 1e-7 covers the
 * full +/-180 deg range with room to spare (raw maxes out around +/-2.1e9,
 * we only need +/-1.8e9), so there's no need for a track-local origin
 * offset the way you'd need one to shoehorn this into 16 bits. The
 * origin-offset approach was considered and rejected: it buys you nothing
 * once you've already spent 4 bytes, and it silently breaks the day someone
 * tests the car somewhere far from Ada, Ohio and forgets to move the origin.
 *
 * Scale choices otherwise follow v1's convention (comment in
 * telemetry_proto.c): pick the finest scale that doesn't clip the channel's
 * expected range at int16 (+/-32767). A couple are worth flagging:
 *
 *   gps_heading: 0..360 deg needs headroom past int16 * 0.01 (max 327.67),
 *   so this uses 0.02 (max 655.34) instead - slightly coarser (0.02 deg
 *   resolution) but the only integer scale that both fits in 2 bytes AND
 *   covers the full compass without wrapping weirdly at 328 deg.
 *
 *   motor_velocity: 6000 rpm at scale 0.1 would need raw = 60000, over the
 *   int16 range. Scale 1.0 (whole rpm) is the finest that fits.
 */

static const tlm2_chan_def_t HUB_CHANNELS[] = {
    { "gps_lat",     "deg",   1e-7f, 0.0f, TLM2_W_I32 },
    { "gps_lon",     "deg",   1e-7f, 0.0f, TLM2_W_I32 },
    { "gps_speed",   "mph",   0.01f, 0.0f, TLM2_W_I16 },
    { "gps_heading", "deg",   0.02f, 0.0f, TLM2_W_I16 },
    { "gps_sats",    "count", 1.0f,  0.0f, TLM2_W_I16 },
    { "accel_x",     "g",     0.001f,0.0f, TLM2_W_I16 },
    { "accel_y",     "g",     0.001f,0.0f, TLM2_W_I16 },
    { "accel_z",     "g",     0.001f,0.0f, TLM2_W_I16 },
    { "gyro_z",      "deg/s", 0.01f, 0.0f, TLM2_W_I16 },
};

static const tlm2_chan_def_t FRONT_CHANNELS[] = {
    { "wheel_speed_fl",   "mph", 0.01f, 0.0f, TLM2_W_I16 },
    { "wheel_speed_fr",   "mph", 0.01f, 0.0f, TLM2_W_I16 },
    { "suspension_fl",    "mm",  0.01f, 0.0f, TLM2_W_I16 },
    { "suspension_fr",    "mm",  0.01f, 0.0f, TLM2_W_I16 },
    { "brake_pressure_f", "psi", 0.1f,  0.0f, TLM2_W_I16 },
};

static const tlm2_chan_def_t REAR_CHANNELS[] = {
    { "wheel_speed_rl", "mph",  0.01f, 0.0f, TLM2_W_I16 },
    { "wheel_speed_rr", "mph",  0.01f, 0.0f, TLM2_W_I16 },
    { "suspension_rl",  "mm",   0.01f, 0.0f, TLM2_W_I16 },
    { "suspension_rr",  "mm",   0.01f, 0.0f, TLM2_W_I16 },
    { "cvt_temp",       "degC", 0.01f, 0.0f, TLM2_W_I16 },
};

static const tlm2_chan_def_t MOTOR_CHANNELS[] = {
    { "motor_current",    "A",    0.01f, 0.0f, TLM2_W_I16 },
    { "motor_velocity",   "rpm",  1.0f,  0.0f, TLM2_W_I16 },
    { "motor_temp",       "degC", 0.01f, 0.0f, TLM2_W_I16 },
    { "bus_voltage",      "V",    0.01f, 0.0f, TLM2_W_I16 },
    { "brake_resistor_w", "W",    0.01f, 0.0f, TLM2_W_I16 },
};

const tlm2_chan_def_t *const TLM2_CHANNELS[TLM2_NODE_COUNT] = {
    HUB_CHANNELS, FRONT_CHANNELS, REAR_CHANNELS, MOTOR_CHANNELS,
};

const uint8_t TLM2_CHAN_COUNT[TLM2_NODE_COUNT] = {
    (uint8_t)(sizeof(HUB_CHANNELS)   / sizeof(HUB_CHANNELS[0])),
    (uint8_t)(sizeof(FRONT_CHANNELS) / sizeof(FRONT_CHANNELS[0])),
    (uint8_t)(sizeof(REAR_CHANNELS)  / sizeof(REAR_CHANNELS[0])),
    (uint8_t)(sizeof(MOTOR_CHANNELS) / sizeof(MOTOR_CHANNELS[0])),
};

const char *const TLM2_NODE_NAMES[TLM2_NODE_COUNT] = {
    "Hub - GPS/IMU", "Front", "Rear", "E-CVT / Motor",
};

/* ------------------------------------------------------------------ */
/* Little-endian helpers                                               */
/* ------------------------------------------------------------------ */
/* Byte-wise, not cast-based - alignment-safe and endian-explicit, same
   reasoning as v1. */

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8)  & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p)
{
    return  (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static void    put_i16(uint8_t *p, int16_t v) { put_u16(p, (uint16_t)v); }
static int16_t get_i16(const uint8_t *p)      { return (int16_t)get_u16(p); }
static void    put_i32(uint8_t *p, int32_t v) { put_u32(p, (uint32_t)v); }
static int32_t get_i32(const uint8_t *p)      { return (int32_t)get_u32(p); }

/* ------------------------------------------------------------------ */
/* CRC-16/CCITT-FALSE                                                  */
/* ------------------------------------------------------------------ */
/* Identical algorithm to v1's tlm_crc16(). Duplicated rather than shared -
   see the header comment on why this file avoids depending on v1 at all.
   Check value for "123456789" is 0x29B1, same as v1 (it's the same CRC). */

uint16_t tlm2_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFFu;

    for (size_t i = 0; i < len; i++)
    {
        crc ^= (uint16_t)((uint16_t)data[i] << 8);

        for (int bit = 0; bit < 8; bit++)
        {
            if (crc & 0x8000u)
            {
                crc = (uint16_t)((uint16_t)(crc << 1) ^ 0x1021u);
            }
            else
            {
                crc = (uint16_t)(crc << 1);
            }
        }
    }

    return crc;
}

/* ------------------------------------------------------------------ */
/* Page layout - the heart of the multi-frame scheme                   */
/* ------------------------------------------------------------------ */
/*
 * Greedy, in declared order, never splits a channel across a page boundary.
 * A channel that would overflow the current page's 6-byte payload starts a
 * new page instead - some bytes at the tail of a page can go unused (e.g.
 * the Hub's page 0 holds only gps_lat, 4 of 6 bytes, because gps_lon right
 * after it is also 4 bytes and 4+4 > 6). That waste is the price of "no
 * channel ever depends on two CAN frames both arriving to be readable at
 * all" - a much simpler failure mode than reassembling a value split across
 * a torn page pair.
 *
 * This is a pure function of TLM2_CHANNELS, so the packer (tlm2_can_pack_pages)
 * and the reassembler (tlm2_reasm_apply_page) call it independently and are
 * guaranteed to agree - nothing about the layout goes over the wire.
 */

typedef struct {
    uint8_t page;
    uint8_t offset;   /* byte offset within the page's 6-byte payload area */
    uint8_t width;
} chan_slot_t;

static uint8_t layout_node(uint8_t node_id, chan_slot_t slots[TLM2_MAX_CH_PER_NODE])
{
    if (node_id >= TLM2_NODE_COUNT)
    {
        return 0u;
    }

    const tlm2_chan_def_t *defs = TLM2_CHANNELS[node_id];
    uint8_t n   = TLM2_CHAN_COUNT[node_id];
    uint8_t page = 0u;
    uint8_t off  = 0u;

    for (uint8_t i = 0; i < n; i++)
    {
        uint8_t w = defs[i].width;

        if ((uint16_t)off + w > TLM2_PAGE_PAYLOAD_SIZE)
        {
            page++;
            off = 0u;
        }

        slots[i].page   = page;
        slots[i].offset = off;
        slots[i].width  = w;
        off = (uint8_t)(off + w);
    }

    return (uint8_t)(page + 1u);   /* n > 0 for every real node -> never 0 here */
}

uint8_t tlm2_node_page_count(uint8_t node_id)
{
    chan_slot_t slots[TLM2_MAX_CH_PER_NODE];
    return layout_node(node_id, slots);
}

size_t tlm2_node_wire_bytes(uint8_t node_id)
{
    if (node_id >= TLM2_NODE_COUNT)
    {
        return 0u;
    }

    const tlm2_chan_def_t *defs = TLM2_CHANNELS[node_id];
    size_t total = 0u;

    for (uint8_t i = 0; i < TLM2_CHAN_COUNT[node_id]; i++)
    {
        total += defs[i].width;
    }

    return total;
}

/* ------------------------------------------------------------------ */
/* CAN page pack                                                       */
/* ------------------------------------------------------------------ */

uint8_t tlm2_can_pack_pages(uint8_t node_id,
                            const int32_t *raw, uint8_t n_raw,
                            uint8_t epoch, uint8_t status,
                            uint8_t pages_out[][TLM2_CAN_DLC])
{
    if (node_id >= TLM2_NODE_COUNT || n_raw != TLM2_CHAN_COUNT[node_id])
    {
        return 0u;
    }

    chan_slot_t slots[TLM2_MAX_CH_PER_NODE];
    uint8_t page_count = layout_node(node_id, slots);

    for (uint8_t p = 0; p < page_count; p++)
    {
        pages_out[p][0] = epoch;
        pages_out[p][1] = status;

        for (uint8_t b = 2; b < TLM2_CAN_DLC; b++)
        {
            pages_out[p][b] = 0u;   /* unused tail bytes are zeroed, not garbage */
        }
    }

    for (uint8_t i = 0; i < n_raw; i++)
    {
        uint8_t *dst = &pages_out[slots[i].page][2u + slots[i].offset];

        if (slots[i].width == TLM2_W_I32)
        {
            put_i32(dst, raw[i]);
        }
        else
        {
            /* Truncating, not saturating - see the TODO on this function in
               the header. Caller owns clamping to the channel's real range. */
            put_i16(dst, (int16_t)raw[i]);
        }
    }

    return page_count;
}

/* ------------------------------------------------------------------ */
/* Hub-side reassembly                                                 */
/* ------------------------------------------------------------------ */

void tlm2_reasm_init(tlm2_node_reassembler_t *r)
{
    memset(r, 0, sizeof(*r));
}

bool tlm2_reasm_apply_page(tlm2_node_reassembler_t *r,
                           uint8_t node_id, uint8_t page_idx,
                           const uint8_t d8[TLM2_CAN_DLC], uint32_t now_ms)
{
    uint8_t page_count = tlm2_node_page_count(node_id);

    if (page_count == 0u || page_idx >= page_count)
    {
        return false;   /* bogus node/page - caller's ID decode is wrong, or noise */
    }

    uint8_t epoch  = d8[0];
    uint8_t status = d8[1];

    if (!r->have_epoch || epoch != r->cur_epoch)
    {
        /*
         * Starting a fresh burst. Anything already collected into
         * ch_building for the OLD epoch that never reached completeness is
         * discarded right here - that is the torn-set case. r->ch (the last
         * COMPLETE snapshot) is untouched, so a caller reading it still sees
         * the last good values, just one burst further behind than usual.
         * The gap shows up as loss the next time a burst DOES complete (see
         * the epoch-delta check below), so it isn't silently invisible.
         */
        r->cur_epoch  = epoch;
        r->page_mask  = 0u;
        r->have_epoch = true;
    }

    r->status           = status;
    r->page_mask         = (uint16_t)(r->page_mask | (1u << page_idx));
    r->last_rx_ms        = now_ms;
    r->seen_this_window  = true;

    chan_slot_t slots[TLM2_MAX_CH_PER_NODE];
    layout_node(node_id, slots);
    uint8_t n = TLM2_CHAN_COUNT[node_id];

    for (uint8_t i = 0; i < n; i++)
    {
        if (slots[i].page != page_idx)
        {
            continue;
        }

        const uint8_t *src = &d8[2u + slots[i].offset];
        r->ch_building[i] = (slots[i].width == TLM2_W_I32)
                             ? get_i32(src)
                             : (int32_t)get_i16(src);
    }

    uint16_t full_mask = (uint16_t)((1u << page_count) - 1u);
    if ((r->page_mask & full_mask) != full_mask)
    {
        return false;   /* still waiting on at least one more page */
    }

    /*
     * Complete. Loss is measured in bursts, not individual CAN frames -
     * coarser than v1's per-frame seq, but the granularity that actually
     * matches what a torn set means: you either got a usable snapshot or you
     * didn't. Unsigned subtraction handles the 8-bit epoch wrap exactly like
     * v1's seq gap check.
     */
    if (r->have_complete)
    {
        uint8_t delta = (uint8_t)(epoch - r->last_complete_epoch);

        if (delta > 1u)
        {
            r->loss_accum = (uint16_t)(r->loss_accum + (delta - 1u));
        }
    }

    memcpy(r->ch, r->ch_building, sizeof(r->ch));
    r->last_complete_epoch = epoch;
    r->have_complete        = true;

    return true;
}

void tlm2_reasm_snapshot(tlm2_node_reassembler_t *r, uint8_t node_id,
                         uint32_t now_ms, tlm2_node_record_t *out)
{
    uint8_t flags = 0u;

    if (r->seen_this_window)
    {
        flags |= TLM2_NF_ONLINE;
    }

    if (!r->have_complete || (uint32_t)(now_ms - r->last_rx_ms) > TLM2_NODE_TIMEOUT_MS)
    {
        flags |= TLM2_NF_STALE;
        flags &= (uint8_t)~TLM2_NF_ONLINE;
    }

    if (r->status & TLM2_ST_SENSOR_FAULT)
    {
        flags |= TLM2_NF_FAULT;
    }

    /*
     * PARTIAL: the burst that was in flight at snapshot time never finished.
     * out->ch still carries the last COMPLETE set (never torn data), this
     * flag just tells a human "what you're looking at is not this window's
     * numbers."
     */
    uint8_t  page_count = tlm2_node_page_count(node_id);
    uint16_t full_mask  = (uint16_t)((1u << page_count) - 1u);

    if (r->have_epoch && (r->page_mask & full_mask) != full_mask)
    {
        flags |= TLM2_NF_PARTIAL;
    }

    out->node_id = node_id;
    out->flags   = flags;
    out->epoch   = r->last_complete_epoch;
    out->loss    = (r->loss_accum > 255u) ? 255u : (uint8_t)r->loss_accum;

    memset(out->ch, 0, sizeof(out->ch));
    if (r->have_complete)
    {
        memcpy(out->ch, r->ch, (size_t)TLM2_CHAN_COUNT[node_id] * sizeof(out->ch[0]));
    }

    r->loss_accum       = 0u;
    r->seen_this_window = false;
}

/* ------------------------------------------------------------------ */
/* Radio frame encode / decode                                         */
/* ------------------------------------------------------------------ */

static size_t compute_payload_size(void)
{
    size_t total = 0u;

    for (uint8_t i = 0; i < TLM2_NODE_COUNT; i++)
    {
        total += TLM2_REC_HDR_SIZE + tlm2_node_wire_bytes(i);
    }

    return total;
}

size_t tlm2_encode_frame(const tlm2_frame_t *f, uint8_t *out, size_t out_sz)
{
    size_t payload_size = compute_payload_size();
    size_t frame_size   = TLM2_HDR_SIZE + payload_size + TLM2_CRC_SIZE;

    if (out_sz < frame_size)
    {
        return 0u;
    }

    out[TLM2_OFF_SYNC0] = TLM2_SYNC0;
    out[TLM2_OFF_SYNC1] = TLM2_SYNC1;
    out[TLM2_OFF_VER]   = TLM2_PROTO_VERSION;
    /* Single byte, same as v1 - a future channel table that pushes payload
       past 255 bytes would silently truncate this field. Flagged in
       V2_DESIGN_NOTES.md; today's payload (68 B) is nowhere close. */
    out[TLM2_OFF_LEN]   = (uint8_t)payload_size;

    put_u16(&out[TLM2_OFF_SEQ], f->seq);
    put_u32(&out[TLM2_OFF_TMS], f->t_ms);

    uint8_t *p = &out[TLM2_OFF_PAYLOAD];

    for (uint8_t i = 0; i < TLM2_NODE_COUNT; i++)
    {
        const tlm2_node_record_t *r = &f->nodes[i];

        p[0] = r->node_id;
        p[1] = r->flags;
        p[2] = r->epoch;
        p[3] = r->loss;
        p += TLM2_REC_HDR_SIZE;

        const tlm2_chan_def_t *defs = TLM2_CHANNELS[i];

        for (uint8_t c = 0; c < TLM2_CHAN_COUNT[i]; c++)
        {
            if (defs[c].width == TLM2_W_I32)
            {
                put_i32(p, r->ch[c]);
                p += 4;
            }
            else
            {
                put_i16(p, (int16_t)r->ch[c]);
                p += 2;
            }
        }
    }

    uint16_t crc = tlm2_crc16(&out[TLM2_OFF_VER],
                              (size_t)(frame_size - TLM2_CRC_SIZE - TLM2_OFF_VER));

    put_u16(&out[frame_size - TLM2_CRC_SIZE], crc);

    return frame_size;
}

int tlm2_decode_frame(const uint8_t *buf, size_t len, tlm2_frame_t *out)
{
    size_t payload_size = compute_payload_size();
    size_t frame_size   = TLM2_HDR_SIZE + payload_size + TLM2_CRC_SIZE;

    if (len < frame_size)
    {
        return -1;
    }

    if (buf[TLM2_OFF_SYNC0] != TLM2_SYNC0 || buf[TLM2_OFF_SYNC1] != TLM2_SYNC1)
    {
        return -2;
    }

    if (buf[TLM2_OFF_VER] != TLM2_PROTO_VERSION)
    {
        return -3;
    }

    if (buf[TLM2_OFF_LEN] != (uint8_t)payload_size)
    {
        return -4;
    }

    uint16_t want = tlm2_crc16(&buf[TLM2_OFF_VER],
                               (size_t)(frame_size - TLM2_CRC_SIZE - TLM2_OFF_VER));
    uint16_t got  = get_u16(&buf[frame_size - TLM2_CRC_SIZE]);

    if (want != got)
    {
        return -5;
    }

    out->seq  = get_u16(&buf[TLM2_OFF_SEQ]);
    out->t_ms = get_u32(&buf[TLM2_OFF_TMS]);

    const uint8_t *p = &buf[TLM2_OFF_PAYLOAD];

    for (uint8_t i = 0; i < TLM2_NODE_COUNT; i++)
    {
        tlm2_node_record_t *r = &out->nodes[i];

        r->node_id = p[0];
        r->flags   = p[1];
        r->epoch   = p[2];
        r->loss    = p[3];
        p += TLM2_REC_HDR_SIZE;

        memset(r->ch, 0, sizeof(r->ch));

        const tlm2_chan_def_t *defs = TLM2_CHANNELS[i];

        for (uint8_t c = 0; c < TLM2_CHAN_COUNT[i]; c++)
        {
            if (defs[c].width == TLM2_W_I32)
            {
                r->ch[c] = get_i32(p);
                p += 4;
            }
            else
            {
                r->ch[c] = (int32_t)get_i16(p);
                p += 2;
            }
        }
    }

    return 0;
}

int tlm2_find_frame(const uint8_t *buf, size_t len)
{
    size_t payload_size = compute_payload_size();
    size_t frame_size   = TLM2_HDR_SIZE + payload_size + TLM2_CRC_SIZE;

    if (len < frame_size)
    {
        return -1;
    }

    tlm2_frame_t scratch;
    size_t last = len - frame_size;

    for (size_t i = 0; i <= last; i++)
    {
        if (buf[i] == TLM2_SYNC0 && buf[i + 1] == TLM2_SYNC1)
        {
            if (tlm2_decode_frame(&buf[i], len - i, &scratch) == 0)
            {
                return (int)i;
            }
        }
    }

    return -1;
}
