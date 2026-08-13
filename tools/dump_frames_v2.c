/**
 * dump_frames_v2.c - Host-side exerciser for telemetry_proto_v2, output only.
 *
 * Same role as roundtrip_harness.c plays for v1: compiled and run by
 * tools/test_roundtrip_v2.py against the real telemetry_proto_v2.c, its
 * stdout is what the Python side (proto_v2.py) is checked against. This file
 * contains no assertions of its own beyond "did the C API itself say ok" -
 * the actual cross-language comparison happens in Python, which is the
 * point: two independent implementations agreeing is the test.
 *
 * Everything is derived from a fixed-seed LCG (same algorithm as
 * roundtrip_harness.c and test_roundtrip_v2.c's next_rand(), unrelated seed)
 * so Python can regenerate the identical inputs with zero data passed
 * between the two processes except this stdout stream.
 *
 * Line tags, one item per line:
 *   CRC <hex4>                          CRC-16 of "123456789"
 *   SIZE <n>                            tlm2 frame size for the current tables
 *   FRAME <hex>                         N_FRAMES encoded radio frames
 *   DECODE ok|FAIL:<code>               each FRAME fed back through tlm2_decode_frame
 *   MISLABEL <hex>                      a frame whose node_id bytes are deliberately
 *                                        wrong, proving layout is position- not
 *                                        node_id-driven (see decode_frame in proto_v2.py)
 *   CANPACK <node> <epoch> <status> <hex>   tlm2_can_pack_pages output, pages concatenated
 *   GPSENG <lat> <lon>                  engineering values for a fixed raw lat/lon pair,
 *                                        computed by the C side, %.9f
 *   TORN_PAGE <epoch> <page> <hex8>     CAN pages for the torn-burst scenario (node 0)
 *   TORN_RESULT <flags> <epoch> <loss> <hex-of-9-int32>   C's own reassembler result
 *   WRAP_PAGE <epoch> <page> <hex8>     CAN pages for the epoch-wrap scenario (node 3)
 *   WRAP_RESULT <flags> <epoch> <loss>  C's own reassembler result after the wrap
 *   WRAP_RESULT2 <loss>                 loss after a second, idle snapshot (must be 0)
 *   CORRUPT <rc>                        decode result after flipping one payload bit
 *   FIND <offset>                       tlm2_find_frame() result on junk + false-sync + frame
 *
 * Build:
 *   gcc -std=c99 -Wall -Wextra -Werror \
 *       -I ../protocol ../protocol/telemetry_proto_v2.c dump_frames_v2.c \
 *       -o dump_frames_v2
 *   ./dump_frames_v2
 */

#include "telemetry_proto_v2.h"
#include <stdio.h>
#include <string.h>

#define N_FRAMES 32

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

/* One raw value, honouring the channel's declared width, same convention
   test_roundtrip_v2.c's randomised test uses. */
static int32_t rand_raw(uint8_t width)
{
    uint32_t r = next_rand();
    return (width == TLM2_W_I32) ? (int32_t)r : (int32_t)(int16_t)(r & 0xFFFFu);
}

int main(void)
{
    /* 1. CRC check value. */
    printf("CRC %04X\n", tlm2_crc16((const uint8_t *)"123456789", 9));

    /* 2. Frame size for the current channel tables. */
    {
        tlm2_frame_t f;
        memset(&f, 0, sizeof(f));
        uint8_t buf[TLM2_FRAME_SIZE_MAX];
        size_t len = tlm2_encode_frame(&f, buf, sizeof(buf));
        printf("SIZE %zu\n", len);
    }

    /* 3. Full radio frames: encode, then decode-and-verify on the C side too. */
    for (int f = 0; f < N_FRAMES; f++)
    {
        tlm2_frame_t frame;
        memset(&frame, 0, sizeof(frame));
        frame.seq  = (uint16_t)(next_rand() & 0xFFFFu);
        frame.t_ms = next_rand();

        for (uint8_t i = 0; i < TLM2_NODE_COUNT; i++)
        {
            tlm2_node_record_t *r = &frame.nodes[i];
            r->node_id = i;
            r->flags   = (uint8_t)(next_rand() & 0x0Fu);   /* includes PARTIAL bit */
            r->epoch   = (uint8_t)(next_rand() & 0xFFu);
            r->loss    = (uint8_t)(next_rand() & 0xFFu);

            const tlm2_chan_def_t *defs = TLM2_CHANNELS[i];
            for (uint8_t c = 0; c < TLM2_CHAN_COUNT[i]; c++)
            {
                r->ch[c] = rand_raw(defs[c].width);
            }
        }

        uint8_t buf[TLM2_FRAME_SIZE_MAX];
        size_t len = tlm2_encode_frame(&frame, buf, sizeof(buf));

        printf("FRAME ");
        print_hex(buf, len);
        printf("\n");

        tlm2_frame_t decoded;
        int rc = tlm2_decode_frame(buf, len, &decoded);
        if (rc != 0)
        {
            printf("DECODE FAIL:%d\n", rc);
            continue;
        }

        int ok = (decoded.seq == frame.seq) && (decoded.t_ms == frame.t_ms);
        for (uint8_t i = 0; ok && i < TLM2_NODE_COUNT; i++)
        {
            const tlm2_node_record_t *a = &frame.nodes[i];
            const tlm2_node_record_t *b = &decoded.nodes[i];
            if (a->node_id != b->node_id || a->flags != b->flags ||
                a->epoch != b->epoch || a->loss != b->loss)
            {
                ok = 0;
                break;
            }
            for (uint8_t c = 0; c < TLM2_CHAN_COUNT[i]; c++)
            {
                if (a->ch[c] != b->ch[c]) { ok = 0; break; }
            }
        }
        printf("DECODE %s\n", ok ? "ok" : "FAIL:mismatch");
    }

    /* 4. Mislabelled node_id: proves the wire layout is chosen by position
       (loop index), not by the node_id byte stored in the record - see the
       module docstring in proto_v2.py. tlm2_encode_frame indexes
       TLM2_CHANNELS[i], never TLM2_CHANNELS[r->node_id]. */
    {
        tlm2_frame_t frame;
        memset(&frame, 0, sizeof(frame));
        frame.seq = 777; frame.t_ms = 999888u;

        for (uint8_t i = 0; i < TLM2_NODE_COUNT; i++)
        {
            tlm2_node_record_t *r = &frame.nodes[i];
            r->node_id = (uint8_t)(200u + i);   /* deliberately not equal to i */
            r->flags   = 0;
            r->epoch   = (uint8_t)(50 + i);
            r->loss    = 0;

            const tlm2_chan_def_t *defs = TLM2_CHANNELS[i];
            for (uint8_t c = 0; c < TLM2_CHAN_COUNT[i]; c++)
            {
                r->ch[c] = rand_raw(defs[c].width);
            }
        }

        uint8_t buf[TLM2_FRAME_SIZE_MAX];
        size_t len = tlm2_encode_frame(&frame, buf, sizeof(buf));
        printf("MISLABEL ");
        print_hex(buf, len);
        printf("\n");
    }

    /* 5. CAN page packing across all four nodes. */
    for (int iter = 0; iter < 8; iter++)
    {
        for (uint8_t node = 0; node < TLM2_NODE_COUNT; node++)
        {
            const tlm2_chan_def_t *defs = TLM2_CHANNELS[node];
            uint8_t n = TLM2_CHAN_COUNT[node];

            int32_t raw[TLM2_MAX_CH_PER_NODE];
            for (uint8_t c = 0; c < n; c++)
            {
                raw[c] = rand_raw(defs[c].width);
            }

            uint8_t epoch  = (uint8_t)(next_rand() & 0xFFu);
            uint8_t status = (uint8_t)(next_rand() & 0x07u);

            uint8_t pages[TLM2_MAX_PAGES_PER_NODE][TLM2_CAN_DLC];
            uint8_t page_count = tlm2_can_pack_pages(node, raw, n, epoch, status, pages);

            printf("CANPACK %u %u %u ", node, epoch, status);
            for (uint8_t p = 0; p < page_count; p++)
            {
                print_hex(pages[p], TLM2_CAN_DLC);
            }
            printf("\n");
        }
    }

    /* 6. GPS precision: fixed raw lat/lon (positive + negative), C's own
       engineering-value computation for Python to compare against. */
    {
        const tlm2_chan_def_t *hub = TLM2_CHANNELS[0];
        int32_t raw_lat = 407660321;    /* ~40.7660321 deg (Ada, Ohio) */
        int32_t raw_lon = -838220456;   /* ~-83.8220456 deg            */

        double lat = (double)raw_lat * (double)hub[0].scale + (double)hub[0].offset;
        double lon = (double)raw_lon * (double)hub[1].scale + (double)hub[1].offset;

        printf("GPSRAW %d %d\n", raw_lat, raw_lon);
        printf("GPSENG %.9f %.9f\n", lat, lon);
    }

    /* 7. Torn burst: node 0 (Hub, 4 pages). Epoch 1 completes; only page 0 of
       epoch 2 is ever delivered to the reassembler. */
    {
        const uint8_t node = 0;
        const tlm2_chan_def_t *defs = TLM2_CHANNELS[node];
        uint8_t n = TLM2_CHAN_COUNT[node];

        int32_t raw1[TLM2_MAX_CH_PER_NODE], raw2[TLM2_MAX_CH_PER_NODE];
        for (uint8_t c = 0; c < n; c++) raw1[c] = rand_raw(defs[c].width);
        for (uint8_t c = 0; c < n; c++) raw2[c] = rand_raw(defs[c].width);

        uint8_t pages1[TLM2_MAX_PAGES_PER_NODE][TLM2_CAN_DLC];
        uint8_t pages2[TLM2_MAX_PAGES_PER_NODE][TLM2_CAN_DLC];
        uint8_t pc1 = tlm2_can_pack_pages(node, raw1, n, /*epoch=*/1, TLM2_ST_OK, pages1);
        uint8_t pc2 = tlm2_can_pack_pages(node, raw2, n, /*epoch=*/2, TLM2_ST_OK, pages2);

        for (uint8_t p = 0; p < pc1; p++)
        {
            printf("TORN_PAGE 1 %u ", p);
            print_hex(pages1[p], TLM2_CAN_DLC);
            printf("\n");
        }
        for (uint8_t p = 0; p < pc2; p++)
        {
            printf("TORN_PAGE 2 %u ", p);
            print_hex(pages2[p], TLM2_CAN_DLC);
            printf("\n");
        }

        tlm2_node_reassembler_t r;
        tlm2_reasm_init(&r);
        for (uint8_t p = 0; p < pc1; p++)
        {
            tlm2_reasm_apply_page(&r, node, p, pages1[p], 1000u);
        }
        /* Only page 0 of epoch 2 ever arrives. */
        tlm2_reasm_apply_page(&r, node, 0, pages2[0], 1100u);

        tlm2_node_record_t rec;
        tlm2_reasm_snapshot(&r, node, 1100u, &rec);

        printf("TORN_RESULT %u %u %u ", rec.flags, rec.epoch, rec.loss);
        for (uint8_t c = 0; c < n; c++)
        {
            printf("%08X", (uint32_t)rec.ch[c]);
        }
        printf("\n");
    }

    /* 8. Epoch wraparound loss: node 3 (Motor, 2 pages). Epoch 254 completes,
       epoch 255 is skipped entirely, epoch 0 completes - one lost burst. */
    {
        const uint8_t node = 3;
        const tlm2_chan_def_t *defs = TLM2_CHANNELS[node];
        uint8_t n = TLM2_CHAN_COUNT[node];

        int32_t rawA[TLM2_MAX_CH_PER_NODE], rawB[TLM2_MAX_CH_PER_NODE];
        for (uint8_t c = 0; c < n; c++) rawA[c] = rand_raw(defs[c].width);
        for (uint8_t c = 0; c < n; c++) rawB[c] = rand_raw(defs[c].width);

        uint8_t pagesA[TLM2_MAX_PAGES_PER_NODE][TLM2_CAN_DLC];
        uint8_t pagesB[TLM2_MAX_PAGES_PER_NODE][TLM2_CAN_DLC];
        uint8_t pcA = tlm2_can_pack_pages(node, rawA, n, /*epoch=*/254, TLM2_ST_OK, pagesA);
        uint8_t pcB = tlm2_can_pack_pages(node, rawB, n, /*epoch=*/0,   TLM2_ST_OK, pagesB);

        for (uint8_t p = 0; p < pcA; p++)
        {
            printf("WRAP_PAGE 254 %u ", p);
            print_hex(pagesA[p], TLM2_CAN_DLC);
            printf("\n");
        }
        for (uint8_t p = 0; p < pcB; p++)
        {
            printf("WRAP_PAGE 0 %u ", p);
            print_hex(pagesB[p], TLM2_CAN_DLC);
            printf("\n");
        }

        tlm2_node_reassembler_t r;
        tlm2_reasm_init(&r);
        for (uint8_t p = 0; p < pcA; p++) tlm2_reasm_apply_page(&r, node, p, pagesA[p], 0);
        for (uint8_t p = 0; p < pcB; p++) tlm2_reasm_apply_page(&r, node, p, pagesB[p], 100);

        tlm2_node_record_t rec;
        tlm2_reasm_snapshot(&r, node, 100u, &rec);
        printf("WRAP_RESULT %u %u %u\n", rec.flags, rec.epoch, rec.loss);

        tlm2_node_record_t rec2;
        tlm2_reasm_snapshot(&r, node, 100u, &rec2);
        printf("WRAP_RESULT2 %u\n", rec2.loss);
    }

    /* 9. Corruption must be caught. */
    {
        tlm2_frame_t frame;
        memset(&frame, 0, sizeof(frame));
        frame.seq = 9; frame.t_ms = 555;
        for (uint8_t n = 0; n < TLM2_NODE_COUNT; n++) frame.nodes[n].node_id = n;

        uint8_t buf[TLM2_FRAME_SIZE_MAX];
        size_t len = tlm2_encode_frame(&frame, buf, sizeof(buf));
        buf[20] ^= 0x01u;

        tlm2_frame_t decoded;
        int rc = tlm2_decode_frame(buf, len, &decoded);
        printf("CORRUPT %d\n", rc);
    }

    /* 10. Stream resynchronisation. */
    {
        tlm2_frame_t frame;
        memset(&frame, 0, sizeof(frame));
        frame.seq = 0xBEEF; frame.t_ms = 0x12345678u;

        uint8_t stream[16 + TLM2_FRAME_SIZE_MAX];
        for (int i = 0; i < 16; i++) stream[i] = (uint8_t)(i * 7);
        stream[10] = TLM2_SYNC0;
        stream[11] = TLM2_SYNC1;

        size_t len = tlm2_encode_frame(&frame, &stream[16], sizeof(stream) - 16);
        printf("FIND %d\n", tlm2_find_frame(stream, 16 + len));
    }

    return 0;
}
