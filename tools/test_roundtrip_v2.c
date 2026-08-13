/**
 * test_roundtrip_v2.c - Host-side self-test for telemetry_proto_v2.
 *
 * Standalone (no Python driver, no reference implementation to diff against -
 * there is no proto_v2.py yet, see V2_DESIGN_NOTES.md). Exercises the full
 * pipeline the design doc claims works:
 *
 *   engineering values -> raw ints -> tlm2_can_pack_pages() -> CAN pages
 *   -> tlm2_reasm_apply_page() x N (hub reassembly) -> tlm2_reasm_snapshot()
 *   -> tlm2_encode_frame() -> tlm2_decode_frame() -> values compared back
 *
 * plus the specific failure modes the design doc calls out: torn bursts,
 * epoch wraparound loss counting, CRC corruption, stream resync, and GPS
 * int32 precision.
 *
 * Build (no HAL, no v1 files needed):
 *   gcc -std=c99 -Wall -Wextra -Werror \
 *       -I ../protocol ../protocol/telemetry_proto_v2.c test_roundtrip_v2.c \
 *       -o test_roundtrip_v2 -lm
 *   ./test_roundtrip_v2
 */

#include "telemetry_proto_v2.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static int g_checks = 0;
static int g_fails  = 0;

static void check(int cond, const char *what)
{
    g_checks++;
    if (!cond)
    {
        g_fails++;
        printf("FAIL: %s\n", what);
    }
}

/* Same LCG shape as roundtrip_harness.c, unrelated seed - no cross-file
   dependency, just consistency of style. */
static uint32_t rng_state = 0xB19B00Bu;

static uint32_t next_rand(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state;
}

/** Mirrors sim_to_raw() in can_node.c: engineering value -> raw, clamped to
    the channel's declared width so this test can't itself produce an
    out-of-range value and blame the protocol for it. */
static int32_t eng_to_raw(const tlm2_chan_def_t *d, double value)
{
    double raw = (value - (double)d->offset) / (double)d->scale;
    double lo, hi;

    if (d->width == TLM2_W_I32)
    {
        lo = -2147483648.0;
        hi =  2147483647.0;
    }
    else
    {
        lo = -32768.0;
        hi =  32767.0;
    }

    if (raw > hi) raw = hi;
    if (raw < lo) raw = lo;

    return (int32_t)(raw >= 0.0 ? raw + 0.5 : raw - 0.5);
}

/* ------------------------------------------------------------------ */
/* 1. Structural sanity: sizes, CRC vector, CAN ID macros               */
/* ------------------------------------------------------------------ */

static void test_structure(void)
{
    printf("-- structure --\n");

    check(tlm2_crc16((const uint8_t *)"123456789", 9) == 0x29B1u,
          "CRC-16/CCITT-FALSE check value");

    size_t total_payload = 0;
    for (uint8_t n = 0; n < TLM2_NODE_COUNT; n++)
    {
        size_t wire = tlm2_node_wire_bytes(n);
        uint8_t pages = tlm2_node_page_count(n);
        printf("  node %u (%-14s) chans=%u wire_bytes=%zu pages=%u\n",
               n, TLM2_NODE_NAMES[n], TLM2_CHAN_COUNT[n], wire, pages);
        check(pages >= 1 && pages <= TLM2_MAX_PAGES_PER_NODE, "page count in bounds");
        total_payload += TLM2_REC_HDR_SIZE + wire;
    }

    size_t frame_size = TLM2_HDR_SIZE + total_payload + TLM2_CRC_SIZE;
    printf("  total payload = %zu B, frame = %zu B (v1 was 42 B)\n",
           total_payload, frame_size);

    check(total_payload == 68u, "payload size matches design doc (68 B)");
    check(frame_size == 80u, "frame size matches design doc (80 B)");
    check(frame_size <= TLM2_FRAME_SIZE_MAX, "actual frame size fits the _MAX buffer bound");

    /* CAN ID allocation: round trip, and no overlap with v1's 0x100-0x102. */
    check(TLM2_CAN_DATA_BASE_ID > 0x102u, "v2 CAN base ID clear of v1's 0x100-0x102");

    for (uint8_t node = 0; node < TLM2_NODE_COUNT; node++)
    {
        uint8_t pages = tlm2_node_page_count(node);
        for (uint8_t page = 0; page < pages; page++)
        {
            uint32_t id = TLM2_CAN_ID_FOR(node, page);
            check(TLM2_CAN_ID_IS_NODE(id), "packed ID recognised as a v2 node ID");
            check(TLM2_CAN_NODE_FROM_ID(id) == node, "CAN ID -> node round trip");
            check(TLM2_CAN_PAGE_FROM_ID(id) == page, "CAN ID -> page round trip");
        }
    }
}

/* ------------------------------------------------------------------ */
/* 2. Full pipeline, one burst per node, realistic values               */
/* ------------------------------------------------------------------ */

/* Roughly what vehicle_data.py would produce mid-lap - not copied verbatim,
   just plausible numbers in each channel's real range. */
static const double HUB_VALS[]   = { 40.7660321, -83.8220456, 34.2, 187.44, 10, 0.42, -0.18, 1.03, 21.6 };
static const double FRONT_VALS[] = { 33.1, 31.4, 22.7, 27.9, 410.0 };
static const double REAR_VALS[]  = { 32.6, 30.9, 21.1, 26.4, 96.3 };
static const double MOTOR_VALS[] = { 38.5, 5210.0, 71.8, 52.3, 12.4 };

static const double *node_vals(uint8_t node)
{
    switch (node)
    {
        case 0: return HUB_VALS;
        case 1: return FRONT_VALS;
        case 2: return REAR_VALS;
        default: return MOTOR_VALS;
    }
}

/**
 * Packs one node's declared values, delivers the resulting pages through a
 * reassembler (optionally reordered/dropped by the caller before this is
 * called - see the torn-set test below), and returns whichever page
 * delivery completed the burst.
 */
static bool run_one_burst(uint8_t node, uint8_t epoch, uint8_t status,
                          int32_t raw_out[TLM2_MAX_CH_PER_NODE],
                          uint8_t pages_out[TLM2_MAX_PAGES_PER_NODE][TLM2_CAN_DLC],
                          uint8_t *page_count_out)
{
    const tlm2_chan_def_t *defs = TLM2_CHANNELS[node];
    const double *vals = node_vals(node);
    uint8_t n = TLM2_CHAN_COUNT[node];

    for (uint8_t c = 0; c < n; c++)
    {
        raw_out[c] = eng_to_raw(&defs[c], vals[c]);
    }

    uint8_t pages = tlm2_can_pack_pages(node, raw_out, n, epoch, status, pages_out);
    *page_count_out = pages;
    return pages != 0;
}

static void test_full_pipeline(void)
{
    printf("-- full pipeline (pack -> pages -> reassemble -> snapshot -> radio) --\n");

    tlm2_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.seq  = 4242;
    frame.t_ms = 123456u;

    for (uint8_t node = 0; node < TLM2_NODE_COUNT; node++)
    {
        int32_t raw[TLM2_MAX_CH_PER_NODE];
        uint8_t pages[TLM2_MAX_PAGES_PER_NODE][TLM2_CAN_DLC];
        uint8_t page_count;

        check(run_one_burst(node, /*epoch=*/7, TLM2_ST_OK, raw, pages, &page_count),
              "pack succeeds for a valid node");

        tlm2_node_reassembler_t r;
        tlm2_reasm_init(&r);

        bool completed_on_last_page = false;
        for (uint8_t p = 0; p < page_count; p++)
        {
            uint32_t can_id = TLM2_CAN_ID_FOR(node, p);
            bool complete = tlm2_reasm_apply_page(&r, TLM2_CAN_NODE_FROM_ID(can_id),
                                                  TLM2_CAN_PAGE_FROM_ID(can_id),
                                                  pages[p], /*now_ms=*/1000u);
            if (p + 1 == page_count)
            {
                completed_on_last_page = complete;
            }
            else
            {
                check(!complete, "burst not complete before its last page arrives");
            }
        }
        check(completed_on_last_page, "burst completes exactly on its last page");

        tlm2_node_record_t rec;
        tlm2_reasm_snapshot(&r, node, /*now_ms=*/1000u, &rec);

        check((rec.flags & TLM2_NF_ONLINE) != 0, "fresh burst -> ONLINE");
        check((rec.flags & TLM2_NF_STALE) == 0, "fresh burst -> not STALE");
        check((rec.flags & TLM2_NF_PARTIAL) == 0, "complete burst -> not PARTIAL");
        check(rec.epoch == 7, "snapshot carries the burst's epoch");

        int match = 1;
        for (uint8_t c = 0; c < TLM2_CHAN_COUNT[node]; c++)
        {
            if (rec.ch[c] != raw[c]) { match = 0; break; }
        }
        check(match, "reassembled channel values match what was packed");

        frame.nodes[node] = rec;
    }

    uint8_t buf[TLM2_FRAME_SIZE_MAX];
    size_t len = tlm2_encode_frame(&frame, buf, sizeof(buf));
    check(len == 80u, "encoded frame is 80 bytes");

    tlm2_frame_t decoded;
    int rc = tlm2_decode_frame(buf, len, &decoded);
    check(rc == 0, "decode succeeds");
    check(decoded.seq == frame.seq, "seq survives the radio frame");
    check(decoded.t_ms == frame.t_ms, "t_ms survives the radio frame");

    for (uint8_t node = 0; node < TLM2_NODE_COUNT; node++)
    {
        const tlm2_node_record_t *a = &frame.nodes[node];
        const tlm2_node_record_t *b = &decoded.nodes[node];

        check(a->node_id == b->node_id && a->flags == b->flags &&
              a->epoch == b->epoch && a->loss == b->loss,
              "record header survives encode/decode");

        int ch_match = 1;
        for (uint8_t c = 0; c < TLM2_CHAN_COUNT[node]; c++)
        {
            if (a->ch[c] != b->ch[c]) { ch_match = 0; break; }
        }
        check(ch_match, "channel values survive encode/decode");
    }

    /* And the values are recoverable engineering numbers, not just equal
       raw ints - proves the scale table round-trips too. */
    const tlm2_chan_def_t *hub = TLM2_CHANNELS[0];
    double lat_back = (double)decoded.nodes[0].ch[0] * hub[0].scale + hub[0].offset;
    check(fabs(lat_back - HUB_VALS[0]) < 2e-7, "gps_lat survives as an engineering value");
}

/* ------------------------------------------------------------------ */
/* 3. GPS int32 precision, explicitly                                   */
/* ------------------------------------------------------------------ */

static void test_gps_precision(void)
{
    printf("-- GPS precision --\n");

    const tlm2_chan_def_t *hub = TLM2_CHANNELS[0];
    double lat = 40.7660123;
    double lon = -83.8220987;

    int32_t raw_lat = eng_to_raw(&hub[0], lat);
    int32_t raw_lon = eng_to_raw(&hub[1], lon);

    double back_lat = (double)raw_lat * hub[0].scale + hub[0].offset;
    double back_lon = (double)raw_lon * hub[1].scale + hub[1].offset;

    double err_lat_m = fabs(back_lat - lat) * 111320.0;   /* ~m per degree latitude */
    double err_lon_m = fabs(back_lon - lon) * 111320.0;   /* close enough at this latitude for a bound */

    printf("  lat %.7f -> raw %d -> %.7f  (err %.4f m)\n", lat, raw_lat, back_lat, err_lat_m);
    printf("  lon %.7f -> raw %d -> %.7f  (err %.4f m)\n", lon, raw_lon, back_lon, err_lon_m);

    check(err_lat_m < 0.02, "gps_lat precision within ~2 cm");
    check(err_lon_m < 0.02, "gps_lon precision within ~2 cm");

    /* And the claimed global range genuinely fits in int32, with no overflow
       and no meaningful precision loss even at the pole. (scale is stored as
       a float, so the raw int is compared with slack rather than for exact
       equality - float32(1e-7) isn't bit-identical to double 1e-7.) */
    int32_t raw_extreme = eng_to_raw(&hub[0], 90.0);
    double  back_extreme = (double)raw_extreme * hub[0].scale + hub[0].offset;
    check(raw_extreme > 800000000 && raw_extreme < 1000000000,
          "gps_lat raw at +90 deg lands in the expected int32 magnitude");
    check(fabs(back_extreme - 90.0) < 0.001, "gps_lat precision holds even at +90 deg");
}

/* ------------------------------------------------------------------ */
/* 4. Torn burst: hub gets page 0 but never page 1/2/3                  */
/* ------------------------------------------------------------------ */

static void test_torn_burst(void)
{
    printf("-- torn burst / PARTIAL flag --\n");

    const uint8_t node = 0;   /* Hub, 4 pages, worth testing on the widest node */

    int32_t raw[TLM2_MAX_CH_PER_NODE];
    uint8_t pages[TLM2_MAX_PAGES_PER_NODE][TLM2_CAN_DLC];
    uint8_t page_count;

    tlm2_node_reassembler_t r;
    tlm2_reasm_init(&r);

    /* First, a clean complete burst so there is a known-good baseline. */
    run_one_burst(node, /*epoch=*/1, TLM2_ST_OK, raw, pages, &page_count);
    for (uint8_t p = 0; p < page_count; p++)
    {
        tlm2_reasm_apply_page(&r, node, p, pages[p], 1000u);
    }
    tlm2_node_record_t baseline;
    tlm2_reasm_snapshot(&r, node, 1000u, &baseline);
    check((baseline.flags & TLM2_NF_PARTIAL) == 0, "baseline burst is complete, not PARTIAL");

    /* Now a second burst where only page 0 shows up - the rest are "lost". */
    int32_t raw2[TLM2_MAX_CH_PER_NODE];
    run_one_burst(node, /*epoch=*/2, TLM2_ST_OK, raw2, pages, &page_count);
    bool completed = tlm2_reasm_apply_page(&r, node, 0, pages[0], 1100u);
    check(!completed, "single page of a multi-page burst never reports complete");

    tlm2_node_record_t torn;
    tlm2_reasm_snapshot(&r, node, 1100u, &torn);

    check((torn.flags & TLM2_NF_PARTIAL) != 0, "incomplete burst at snapshot time -> PARTIAL");
    check((torn.flags & TLM2_NF_ONLINE) != 0, "a page DID arrive this window -> still ONLINE");

    int still_baseline = 1;
    for (uint8_t c = 0; c < TLM2_CHAN_COUNT[node]; c++)
    {
        if (torn.ch[c] != raw[c]) { still_baseline = 0; break; }
    }
    check(still_baseline, "torn burst does NOT overwrite ch[] - reader still sees last good data");
    (void)raw2;   /* packed but never delivered - the whole point of this test */
}

/* ------------------------------------------------------------------ */
/* 5. Epoch wraparound loss counting                                    */
/* ------------------------------------------------------------------ */

static void test_epoch_wrap_loss(void)
{
    printf("-- epoch wraparound loss counting --\n");

    const uint8_t node = 3;   /* Motor, 2 pages - simplest multi-page node */
    int32_t raw[TLM2_MAX_CH_PER_NODE];
    uint8_t pages[TLM2_MAX_PAGES_PER_NODE][TLM2_CAN_DLC];
    uint8_t page_count;

    tlm2_node_reassembler_t r;
    tlm2_reasm_init(&r);

    /* Complete epoch 254. */
    run_one_burst(node, 254, TLM2_ST_OK, raw, pages, &page_count);
    for (uint8_t p = 0; p < page_count; p++) tlm2_reasm_apply_page(&r, node, p, pages[p], 0);

    /* Epoch 255 never shows up at all (both pages lost). Epoch 0 completes -
       wraps past 255, delta should be 2 (0 - 254 mod 256), so loss += 1. */
    run_one_burst(node, 0, TLM2_ST_OK, raw, pages, &page_count);
    for (uint8_t p = 0; p < page_count; p++) tlm2_reasm_apply_page(&r, node, p, pages[p], 100);

    tlm2_node_record_t rec;
    tlm2_reasm_snapshot(&r, node, 100, &rec);
    check(rec.loss == 1, "one fully-skipped epoch across the 8-bit wrap counts as 1 lost burst");

    /* loss_accum resets after a snapshot - a second snapshot with no new
       activity should read back 0, matching v1's build_frame() behaviour. */
    tlm2_node_record_t rec2;
    tlm2_reasm_snapshot(&r, node, 100, &rec2);
    check(rec2.loss == 0, "loss counter resets after being read");
}

/* ------------------------------------------------------------------ */
/* 6. CRC corruption is caught, stream resync finds the real frame      */
/* ------------------------------------------------------------------ */

static void test_corruption_and_resync(void)
{
    printf("-- corruption + resync --\n");

    tlm2_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.seq = 9; frame.t_ms = 555;
    for (uint8_t n = 0; n < TLM2_NODE_COUNT; n++) frame.nodes[n].node_id = n;

    uint8_t buf[TLM2_FRAME_SIZE_MAX];
    size_t len = tlm2_encode_frame(&frame, buf, sizeof(buf));

    buf[20] ^= 0x01u;   /* flip a bit inside the payload */

    tlm2_frame_t decoded;
    int rc = tlm2_decode_frame(buf, len, &decoded);
    check(rc == -5, "single bit flip is caught by the CRC");

    /* Stream resync: junk, a false sync pair, then the real frame. */
    uint8_t good[TLM2_FRAME_SIZE_MAX];
    size_t good_len = tlm2_encode_frame(&frame, good, sizeof(good));

    uint8_t stream[16 + TLM2_FRAME_SIZE_MAX];
    for (int i = 0; i < 16; i++) stream[i] = (uint8_t)(i * 13);
    stream[9]  = TLM2_SYNC0;   /* false sync in the junk */
    stream[10] = TLM2_SYNC1;
    memcpy(&stream[16], good, good_len);

    int off = tlm2_find_frame(stream, 16 + good_len);
    check(off == 16, "find_frame skips the false sync and locates the real frame");
}

/* ------------------------------------------------------------------ */
/* 7. Randomised round trips, catches width/offset arithmetic bugs      */
/* ------------------------------------------------------------------ */

static void test_randomised(void)
{
    printf("-- randomised round trips --\n");

    for (int iter = 0; iter < 200; iter++)
    {
        /*
         * memset first: tlm2_frame_t has a 2-byte compiler-inserted gap
         * between seq (u16) and t_ms (u32, needs 4-byte alignment). That gap
         * is never written by tlm2_decode_frame() (it sets named fields
         * only, not the whole struct), so without this the two sides'
         * padding bytes are just whatever the stack happened to hold and a
         * full-struct memcmp() below is comparing noise, not the protocol.
         */
        tlm2_frame_t frame;
        memset(&frame, 0, sizeof(frame));
        frame.seq  = (uint16_t)(next_rand() & 0xFFFFu);
        frame.t_ms = next_rand();

        for (uint8_t node = 0; node < TLM2_NODE_COUNT; node++)
        {
            const tlm2_chan_def_t *defs = TLM2_CHANNELS[node];
            uint8_t n = TLM2_CHAN_COUNT[node];

            int32_t raw[TLM2_MAX_CH_PER_NODE];
            for (uint8_t c = 0; c < n; c++)
            {
                uint32_t r = next_rand();
                raw[c] = (defs[c].width == TLM2_W_I32) ? (int32_t)r : (int32_t)(int16_t)(r & 0xFFFFu);
            }

            uint8_t pages[TLM2_MAX_PAGES_PER_NODE][TLM2_CAN_DLC];
            uint8_t epoch = (uint8_t)(next_rand() & 0xFFu);
            uint8_t page_count = tlm2_can_pack_pages(node, raw, n, epoch, TLM2_ST_OK, pages);

            tlm2_node_reassembler_t r_asm;
            tlm2_reasm_init(&r_asm);
            for (uint8_t p = 0; p < page_count; p++)
            {
                tlm2_reasm_apply_page(&r_asm, node, p, pages[p], 0);
            }

            tlm2_reasm_snapshot(&r_asm, node, 0, &frame.nodes[node]);

            int match = 1;
            for (uint8_t c = 0; c < n; c++)
            {
                if (frame.nodes[node].ch[c] != raw[c]) { match = 0; break; }
            }
            check(match, "randomised pack/reassemble round trip");
        }

        uint8_t buf[TLM2_FRAME_SIZE_MAX];
        size_t len = tlm2_encode_frame(&frame, buf, sizeof(buf));
        check(len > 0, "randomised frame encodes");

        tlm2_frame_t decoded;
        memset(&decoded, 0, sizeof(decoded));   /* same padding reasoning as above */
        int rc = tlm2_decode_frame(buf, len, &decoded);
        check(rc == 0, "randomised frame decodes");
        check(memcmp(&frame, &decoded, sizeof(frame)) == 0, "randomised frame round trip byte-exact");
    }
}

int main(void)
{
    test_structure();
    test_full_pipeline();
    test_gps_precision();
    test_torn_burst();
    test_epoch_wrap_loss();
    test_corruption_and_resync();
    test_randomised();

    printf("\n%d checks, %d failed\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
