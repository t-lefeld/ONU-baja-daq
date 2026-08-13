/**
 * telemetry_proto.c - Implementation of the shared wire format.
 *
 * No HAL dependencies, no dynamic allocation, no floating point in the
 * encode/decode path. Safe to compile for both the F1 and L4 targets and to
 * link into host-side test programs.
 */

#include "telemetry_proto.h"

/* ------------------------------------------------------------------ */
/* Channel table                                                       */
/* ------------------------------------------------------------------ */
/*
 * Rename these when real sensors replace the simulator. The scale factor is
 * what converts the int16 that travels over the wire back into engineering
 * units, so pick it to use as much of the +/-32767 range as the sensor needs.
 *
 *   scale 0.01  -> +/- 327.67   (temperatures, percentages)
 *   scale 0.001 -> +/- 32.767   (volts, amps, g)
 *   scale 0.1   -> +/- 3276.7   (pressures)
 *   scale 1.0   -> +/- 32767    (rpm, counts)
 */
const tlm_chan_def_t TLM_CHANNELS[TLM_NODE_COUNT][TLM_CH_PER_NODE] = {
    /* node 0 - environmental */
    { { "temp_c",     "degC", 0.01f, 0.0f },
      { "humidity",   "%RH",  0.01f, 0.0f },
      { "pressure",   "kPa",  0.10f, 0.0f } },

    /* node 1 - power */
    { { "batt_v",     "V",    0.001f, 0.0f },
      { "current_a",  "A",    0.001f, 0.0f },
      { "power_w",    "W",    0.10f,  0.0f } },

    /* node 2 - motion */
    { { "accel_x",    "g",    0.001f, 0.0f },
      { "accel_y",    "g",    0.001f, 0.0f },
      { "accel_z",    "g",    0.001f, 0.0f } },

    /* node 3 - drivetrain */
    { { "rpm",        "rpm",  1.00f,  0.0f },
      { "throttle",   "%",    0.01f,  0.0f },
      { "coolant_c",  "degC", 0.01f,  0.0f } },
};

/* ------------------------------------------------------------------ */
/* Little-endian helpers                                               */
/* ------------------------------------------------------------------ */
/*
 * Written byte-wise rather than with casts so the code is alignment-safe and
 * endian-explicit. The compiler collapses these to single stores on Cortex-M.
 */

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

static void put_i16(uint8_t *p, int16_t v) { put_u16(p, (uint16_t)v); }
static int16_t get_i16(const uint8_t *p)   { return (int16_t)get_u16(p); }

/* ------------------------------------------------------------------ */
/* CRC-16/CCITT-FALSE                                                  */
/* ------------------------------------------------------------------ */
/*
 * Bitwise rather than table-driven: 52 bytes at 2 Hz is 832 iterations per
 * second, which is nothing, and it saves 512 bytes of flash on the Bluepills.
 * Check value for the ASCII string "123456789" is 0x29B1.
 */

uint16_t tlm_crc16(const uint8_t *data, size_t len)
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
/* CAN payload                                                         */
/* ------------------------------------------------------------------ */

void tlm_can_pack(const tlm_node_sample_t *s, uint8_t *d8)
{
    put_i16(&d8[0], s->ch[0]);
    put_i16(&d8[2], s->ch[1]);
    put_i16(&d8[4], s->ch[2]);
    d8[6] = s->seq;
    d8[7] = s->status;
}

void tlm_can_unpack(const uint8_t *d8, tlm_node_sample_t *s)
{
    s->ch[0]  = get_i16(&d8[0]);
    s->ch[1]  = get_i16(&d8[2]);
    s->ch[2]  = get_i16(&d8[4]);
    s->seq    = d8[6];
    s->status = d8[7];
}

/* ------------------------------------------------------------------ */
/* Frame encode / decode                                               */
/* ------------------------------------------------------------------ */

size_t tlm_encode_frame(const tlm_frame_t *f, uint8_t *out, size_t out_sz)
{
    if (out_sz < TLM_FRAME_SIZE)
    {
        return 0u;
    }

    out[TLM_OFF_SYNC0] = TLM_SYNC0;
    out[TLM_OFF_SYNC1] = TLM_SYNC1;
    out[TLM_OFF_VER]   = TLM_PROTO_VERSION;
    out[TLM_OFF_LEN]   = (uint8_t)TLM_PAYLOAD_SIZE;

    put_u16(&out[TLM_OFF_SEQ], f->seq);
    put_u32(&out[TLM_OFF_TMS], f->t_ms);

    uint8_t *p = &out[TLM_OFF_PAYLOAD];

    for (unsigned n = 0; n < TLM_NODE_COUNT; n++)
    {
        const tlm_node_record_t *r = &f->nodes[n];

        p[0] = r->node_id;
        p[1] = r->flags;
        p[2] = r->can_seq;
        p[3] = r->loss;
        put_i16(&p[4], r->ch[0]);
        put_i16(&p[6], r->ch[1]);
        put_i16(&p[8], r->ch[2]);

        p += TLM_REC_SIZE;
    }

    /* CRC spans ver..end-of-payload, i.e. everything except sync and the CRC. */
    uint16_t crc = tlm_crc16(&out[TLM_OFF_VER],
                             (size_t)(TLM_FRAME_SIZE - TLM_CRC_SIZE - TLM_OFF_VER));

    put_u16(&out[TLM_FRAME_SIZE - TLM_CRC_SIZE], crc);

    return (size_t)TLM_FRAME_SIZE;
}

int tlm_decode_frame(const uint8_t *buf, size_t len, tlm_frame_t *out)
{
    if (len < TLM_FRAME_SIZE)
    {
        return -1;
    }

    if (buf[TLM_OFF_SYNC0] != TLM_SYNC0 || buf[TLM_OFF_SYNC1] != TLM_SYNC1)
    {
        return -2;
    }

    if (buf[TLM_OFF_VER] != TLM_PROTO_VERSION)
    {
        return -3;
    }

    if (buf[TLM_OFF_LEN] != (uint8_t)TLM_PAYLOAD_SIZE)
    {
        return -4;
    }

    uint16_t want = tlm_crc16(&buf[TLM_OFF_VER],
                              (size_t)(TLM_FRAME_SIZE - TLM_CRC_SIZE - TLM_OFF_VER));
    uint16_t got  = get_u16(&buf[TLM_FRAME_SIZE - TLM_CRC_SIZE]);

    if (want != got)
    {
        return -5;
    }

    out->seq  = get_u16(&buf[TLM_OFF_SEQ]);
    out->t_ms = get_u32(&buf[TLM_OFF_TMS]);

    const uint8_t *p = &buf[TLM_OFF_PAYLOAD];

    for (unsigned n = 0; n < TLM_NODE_COUNT; n++)
    {
        tlm_node_record_t *r = &out->nodes[n];

        r->node_id = p[0];
        r->flags   = p[1];
        r->can_seq = p[2];
        r->loss    = p[3];
        r->ch[0]   = get_i16(&p[4]);
        r->ch[1]   = get_i16(&p[6]);
        r->ch[2]   = get_i16(&p[8]);

        p += TLM_REC_SIZE;
    }

    return 0;
}

int tlm_find_frame(const uint8_t *buf, size_t len)
{
    if (len < TLM_FRAME_SIZE)
    {
        return -1;
    }

    tlm_frame_t scratch;
    size_t last = len - TLM_FRAME_SIZE;

    for (size_t i = 0; i <= last; i++)
    {
        if (buf[i] == TLM_SYNC0 && buf[i + 1] == TLM_SYNC1)
        {
            if (tlm_decode_frame(&buf[i], len - i, &scratch) == 0)
            {
                return (int)i;
            }
        }
    }

    return -1;
}
