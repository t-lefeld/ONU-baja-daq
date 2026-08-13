/**
 * roundtrip_harness.c - Host-side exerciser for the C implementation.
 *
 * Compiled by tools/test_roundtrip.py with the real telemetry_proto.c, then run
 * so its output can be compared against the Python implementation.
 *
 * Prints, one item per line:
 *   CRC <hex>              CRC-16 of "123456789", the standard check value
 *   SIZE <n>               sizeof the on-wire frame
 *   FRAME <hex-bytes>      N encoded frames built from a deterministic LCG
 *   DECODE <ok|FAIL:code>  each frame fed back through tlm_decode_frame
 *   CANPACK <hex-bytes>    CAN payload packing
 *
 * Everything is derived from a fixed seed so the Python side can generate the
 * identical inputs without any data being passed between the two.
 */

#include "telemetry_proto.h"
#include <stdio.h>
#include <string.h>

#define N_FRAMES 64

/* Same LCG constants as the Python side. */
static uint32_t rng_state = 0xC0FFEEu;

static uint32_t next_rand(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state;
}

static void print_hex(const uint8_t *d, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        printf("%02X", d[i]);
    }
}

int main(void)
{
    /* 1. CRC check value. */
    printf("CRC %04X\n", tlm_crc16((const uint8_t *)"123456789", 9));

    /* 2. Struct/layout size. */
    printf("SIZE %u\n", (unsigned)TLM_FRAME_SIZE);

    /* 3. CAN payload packing. */
    for (int i = 0; i < 8; i++)
    {
        tlm_node_sample_t s;
        s.ch[0]  = (int16_t)(next_rand() & 0xFFFFu);
        s.ch[1]  = (int16_t)(next_rand() & 0xFFFFu);
        s.ch[2]  = (int16_t)(next_rand() & 0xFFFFu);
        s.seq    = (uint8_t)(next_rand() & 0xFFu);
        s.status = (uint8_t)(next_rand() & 0x07u);

        uint8_t d8[TLM_CAN_DLC];
        tlm_can_pack(&s, d8);

        printf("CANPACK ");
        print_hex(d8, sizeof(d8));
        printf("\n");

        /* Unpack must return exactly what went in. */
        tlm_node_sample_t back;
        tlm_can_unpack(d8, &back);
        if (memcmp(&s, &back, sizeof(s)) != 0)
        {
            printf("CANPACK_ROUNDTRIP FAIL\n");
            return 1;
        }
    }

    /* 4. Frame encode + decode. */
    for (int f = 0; f < N_FRAMES; f++)
    {
        tlm_frame_t frame;
        frame.seq  = (uint16_t)(next_rand() & 0xFFFFu);
        frame.t_ms = next_rand();

        for (unsigned n = 0; n < TLM_NODE_COUNT; n++)
        {
            frame.nodes[n].node_id = (uint8_t)n;
            frame.nodes[n].flags   = (uint8_t)(next_rand() & 0x07u);
            frame.nodes[n].can_seq = (uint8_t)(next_rand() & 0xFFu);
            frame.nodes[n].loss    = (uint8_t)(next_rand() & 0xFFu);
            frame.nodes[n].ch[0]   = (int16_t)(next_rand() & 0xFFFFu);
            frame.nodes[n].ch[1]   = (int16_t)(next_rand() & 0xFFFFu);
            frame.nodes[n].ch[2]   = (int16_t)(next_rand() & 0xFFFFu);
        }

        uint8_t buf[TLM_FRAME_SIZE];
        size_t  len = tlm_encode_frame(&frame, buf, sizeof(buf));

        if (len != TLM_FRAME_SIZE)
        {
            printf("FRAME ENCODE_FAIL\n");
            return 1;
        }

        printf("FRAME ");
        print_hex(buf, len);
        printf("\n");

        tlm_frame_t decoded;
        int rc = tlm_decode_frame(buf, len, &decoded);

        if (rc != 0)
        {
            printf("DECODE FAIL:%d\n", rc);
            return 1;
        }

        if (memcmp(&frame, &decoded, sizeof(frame)) != 0)
        {
            printf("DECODE FAIL:mismatch\n");
            return 1;
        }

        printf("DECODE ok\n");
    }

    /* 5. Corruption must be caught. Flip one bit in the middle of a payload. */
    {
        tlm_frame_t frame;
        memset(&frame, 0, sizeof(frame));
        frame.seq = 1; frame.t_ms = 2;
        for (unsigned n = 0; n < TLM_NODE_COUNT; n++) frame.nodes[n].node_id = (uint8_t)n;

        uint8_t buf[TLM_FRAME_SIZE];
        tlm_encode_frame(&frame, buf, sizeof(buf));

        buf[20] ^= 0x01u;

        tlm_frame_t decoded;
        int rc = tlm_decode_frame(buf, sizeof(buf), &decoded);
        printf("CORRUPT %d\n", rc);   /* must be -5, CRC mismatch */
    }

    /* 6. Stream resynchronisation: a valid frame preceded by junk. */
    {
        tlm_frame_t frame;
        memset(&frame, 0, sizeof(frame));
        frame.seq = 0xBEEF; frame.t_ms = 0x12345678u;

        uint8_t stream[16 + TLM_FRAME_SIZE];
        for (int i = 0; i < 16; i++) stream[i] = (uint8_t)(i * 7);
        stream[10] = TLM_SYNC0;   /* a false sync in the junk */
        stream[11] = TLM_SYNC1;

        tlm_encode_frame(&frame, &stream[16], TLM_FRAME_SIZE);

        printf("FIND %d\n", tlm_find_frame(stream, sizeof(stream)));  /* must be 16 */
    }

    return 0;
}
