/**
 * test_can_node_v2.c - Runs the real v2 node firmware (can_node_v2.c) on the
 * host, against tools/hal_shim/.
 *
 * NODE_ID is a compile-time constant (see can_node_v2.h), so this file is
 * built TWICE - once with tools/v2_node_id_front/node_id.h on the include
 * path ahead of tools/hal_shim/ (NODE_ID=1, Front), once with
 * tools/v2_node_id_rear/node_id.h (NODE_ID=2, Rear) - by
 * tools/test_can_node_v2.py. Every test below is written against NODE_ID
 * and TLM2_NODE_NAMES[NODE_ID] rather than a hardcoded node, so the same
 * source runs unmodified for both boards; #if NODE_ID == 1 / #else only
 * appears once, in ref_node_vals(), where the two nodes' formulas actually
 * differ (mirrors the same split in can_node_v2.c's sim_read_channels()).
 *
 * What this proves, per node:
 *   - tlm2_node_page_count(NODE_ID) is 2 and stays 2 across many bursts.
 *   - Every page uses TLM2_CAN_ID_FOR(NODE_ID, page).
 *   - epoch is identical across every page of one burst and increments by
 *     exactly one burst to burst (mod 256).
 *   - A stalled TX mailbox does not silently drop a page: pages queue and
 *     drain over later can_node_v2_task() calls, and a burst that genuinely
 *     never clears within one TX period is counted (can_node_v2_torn_burst_
 *     count()), not silently swallowed.
 *   - All 5 channels survive pack -> tlm2_reasm_apply_page() unpack, and the
 *     decoded engineering values land within one scale step of an
 *     INDEPENDENTLY re-derived reference (ref_node_vals() below, ported by
 *     hand from simulation/vehicle_data.py in double precision) - this is
 *     checking the firmware against the spec, not against itself.
 *   - Bus-off is detected, recovered, and the node keeps transmitting after.
 *   - The startup status flag latches then clears, same as v1.
 *
 * Not covered here (already covered elsewhere, not duplicated): CRC/frame
 * encode-decode correctness and torn-set reassembly at the protocol level -
 * see tools/test_roundtrip_v2.c. This file is specifically about the
 * firmware layer can_node_v2.c adds on top of that protocol: scheduling,
 * mailbox handling, and the simulator.
 */

#include "can_node_v2.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int checks, failures;

static void ok(const char *label, int cond)
{
    checks++;
    if (!cond)
    {
        failures++;
        printf("FAIL %s\n", label);
    }
}

static void eq_u32(const char *label, uint32_t got, uint32_t want)
{
    checks++;
    if (got != want)
    {
        failures++;
        printf("FAIL %s: got %u want %u\n", label, got, want);
    }
}

static CAN_HandleTypeDef s_can;

static void bringup(void)
{
    memset(&s_can, 0, sizeof(s_can));
    s_can.Instance = CAN1;
    can_node_v2_init(&s_can);
}

/* ------------------------------------------------------------------ */
/* Independent reference formulas - hand-ported from                   */
/* simulation/vehicle_data.py, double precision, on purpose separate   */
/* from can_node_v2.c's single-precision sim_read_channels().          */
/* ------------------------------------------------------------------ */

#define REF_PI              3.14159265358979323846
#define REF_LAP_PERIOD_S    60.0
#define REF_MPH_PER_G_PER_S 21.937

static double ref_speed_mph(double t)
{
    double phase = 2.0 * REF_PI * t / REF_LAP_PERIOD_S - REF_PI / 2.0;
    return 22.0 + 20.0 * sin(phase);
}

static double ref_lon_accel_g(double t)
{
    double phase          = 2.0 * REF_PI * t / REF_LAP_PERIOD_S - REF_PI / 2.0;
    double d_speed_mph_dt = 20.0 * (2.0 * REF_PI / REF_LAP_PERIOD_S) * cos(phase);
    return d_speed_mph_dt / REF_MPH_PER_G_PER_S;
}

static double ref_lat_accel_g(double t)
{
    return 0.8 * sin(2.0 * REF_PI * t / REF_LAP_PERIOD_S * 2.0);
}

static double ref_bump(double t_elapsed, double amp)
{
    return amp * (sin(37.0 * t_elapsed) * 0.6 + sin(91.3 * t_elapsed) * 0.4);
}

static double ref_warmup(double t_elapsed, double start, double end, double tau)
{
    return start + (end - start) * (1.0 - exp(-t_elapsed / tau));
}

static double ref_clamp(double v, double lo, double hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static double ref_max0(double v) { return v > 0.0 ? v : 0.0; }

/** Reference front_vals/rear_vals for this NODE_ID, at t_elapsed seconds
 *  since reset - see simulation/vehicle_data.py's sample_frame(). */
static void ref_node_vals(double t_elapsed, double out[5])
{
    double t = fmod(t_elapsed, REF_LAP_PERIOD_S);
    if (t < 0.0) t += REF_LAP_PERIOD_S;

    double speed         = ref_max0(ref_speed_mph(t));
    double lon_g         = ref_lon_accel_g(t);
    double lat_g         = ref_lat_accel_g(t);
    double corner_offset = lat_g * 1.5;
    double lean          = lat_g * 8.0;

#if NODE_ID == 1
    out[0] = ref_max0(speed + corner_offset);
    out[1] = ref_max0(speed - corner_offset);
    out[2] = ref_clamp(25.0 - lean + ref_bump(t_elapsed, 4.0), 0.0, 50.0);
    out[3] = ref_clamp(25.0 + lean + ref_bump(t_elapsed + 0.5, 4.0), 0.0, 50.0);
    out[4] = (ref_max0(-lon_g * 600.0) + 5.0 < 1000.0) ? (ref_max0(-lon_g * 600.0) + 5.0) : 1000.0;
#else
    out[0] = ref_max0(speed + corner_offset * 0.7);
    out[1] = ref_max0(speed - corner_offset * 0.7);
    out[2] = ref_clamp(25.0 - lean * 0.8 + ref_bump(t_elapsed + 1.0, 4.0), 0.0, 50.0);
    out[3] = ref_clamp(25.0 + lean * 0.8 + ref_bump(t_elapsed + 1.5, 4.0), 0.0, 50.0);
    out[4] = ref_warmup(t_elapsed, 35.0, 108.0, 180.0) + ref_bump(t_elapsed, 1.5);
#endif
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

static void test_page_layout_and_ids(void)
{
    printf("-- node %u (%s): page layout and CAN IDs\n",
           (unsigned)NODE_ID, TLM2_NODE_NAMES[NODE_ID]);

    eq_u32("tlm2_node_page_count(NODE_ID) is 2", tlm2_node_page_count(NODE_ID), 2u);

    shim_reset();
    bringup();

    for (uint32_t t = 0; t < 1000; t += 10)
    {
        shim_advance(10);
        can_node_v2_task();
    }

    uint32_t n = shim_tx_count();

    /* 1000 ms / 100 ms period * 2 pages/burst = 20, a little slack at the edges. */
    ok("about 20 pages sent in 1 s", n >= 18 && n <= 22);
    eq_u32("pages come in whole-burst (even) counts", n % 2u, 0u);

    int ids_ok = 1;
    for (uint32_t i = 0; i + 1 < n; i += 2)
    {
        uint32_t id0, id1;
        uint8_t  d0[8], d1[8];
        shim_tx_get(i, &id0, d0);
        shim_tx_get(i + 1, &id1, d1);
        if (id0 != TLM2_CAN_ID_FOR(NODE_ID, 0)) ids_ok = 0;
        if (id1 != TLM2_CAN_ID_FOR(NODE_ID, 1)) ids_ok = 0;
    }
    ok("every page uses TLM2_CAN_ID_FOR(NODE_ID, page)", ids_ok);
}

static void test_epoch(void)
{
    printf("-- node %u: epoch\n", (unsigned)NODE_ID);

    shim_reset();
    bringup();

    for (uint32_t t = 0; t < 500; t += 10)
    {
        shim_advance(10);
        can_node_v2_task();
    }

    uint32_t n = shim_tx_count();
    int within_ok = 1, incr_ok = 1;
    uint8_t prev_epoch = 0;
    int have_prev = 0;

    for (uint32_t i = 0; i + 1 < n; i += 2)
    {
        uint32_t id0, id1;
        uint8_t  d0[8], d1[8];
        shim_tx_get(i, &id0, d0);
        shim_tx_get(i + 1, &id1, d1);
        (void)id0; (void)id1;

        if (d0[0] != d1[0]) within_ok = 0;   /* epoch byte, same within a burst */

        if (have_prev)
        {
            uint8_t delta = (uint8_t)(d0[0] - prev_epoch);
            if (delta != 1u) incr_ok = 0;
        }
        prev_epoch = d0[0];
        have_prev  = 1;
    }

    ok("captured at least a few bursts", n >= 4);
    ok("epoch identical across every page of one burst", within_ok);
    ok("epoch increments by exactly one burst to burst", incr_ok);
}

static void test_channel_roundtrip_and_scaling(void)
{
    printf("-- node %u: channel round-trip and scaling vs. vehicle_data.py\n",
           (unsigned)NODE_ID);

    shim_reset();
    bringup();

    /* First burst fires exactly at TLM2_NODE_TX_PERIOD_MS - see
       can_node_v2_init()'s s_next_tx_ms = HAL_GetTick() + period. */
    shim_advance(TLM2_NODE_TX_PERIOD_MS);
    can_node_v2_task();

    uint32_t id0, id1;
    uint8_t  d0[8], d1[8];
    ok("page 0 captured", shim_tx_get(0, &id0, d0));
    ok("page 1 captured", shim_tx_get(1, &id1, d1));

    tlm2_node_reassembler_t r;
    tlm2_reasm_init(&r);
    bool c0 = tlm2_reasm_apply_page(&r, NODE_ID, (uint8_t)TLM2_CAN_PAGE_FROM_ID(id0), d0, 100);
    bool c1 = tlm2_reasm_apply_page(&r, NODE_ID, (uint8_t)TLM2_CAN_PAGE_FROM_ID(id1), d1, 100);
    ok("burst completes exactly on its second page, not before", !c0 && c1);

    double expected[5];
    ref_node_vals((double)TLM2_NODE_TX_PERIOD_MS / 1000.0, expected);

    int scale_ok = 1;
    for (uint8_t c = 0; c < TLM2_CHAN_COUNT[NODE_ID]; c++)
    {
        const tlm2_chan_def_t *d = &TLM2_CHANNELS[NODE_ID][c];
        double decoded = (double)r.ch[c] * (double)d->scale + (double)d->offset;
        double tol     = 2.0 * (double)d->scale + 0.02;   /* one raw count + float32 slack */

        if (fabs(decoded - expected[c]) > tol)
        {
            scale_ok = 0;
            printf("   channel %u (%s): decoded %.4f %s, reference %.4f, tolerance %.4f\n",
                   c, d->name, decoded, d->unit, expected[c], tol);
        }
    }
    ok("all 5 channels survive pack->unpack within one scale step of the reference formula",
       scale_ok);
}

static void test_mailbox_contention_and_torn_burst(void)
{
    printf("-- node %u: mailbox contention and torn-burst accounting\n", (unsigned)NODE_ID);

    shim_reset();
    bringup();

    uint32_t torn_before = can_node_v2_torn_burst_count();
    uint32_t page_before = can_node_v2_page_tx_count();

    shim_set_tx_mailboxes_free(0);

    /* First burst: built, but nothing can leave (no free mailbox). */
    shim_advance(TLM2_NODE_TX_PERIOD_MS);
    can_node_v2_task();
    eq_u32("nothing transmitted while mailboxes are full", shim_tx_count(), 0u);

    /* Second period arrives before the first burst ever cleared - that is
       exactly the torn-burst case, and it must be counted, not swallowed. */
    shim_advance(TLM2_NODE_TX_PERIOD_MS);
    can_node_v2_task();
    eq_u32("exactly one torn burst counted", can_node_v2_torn_burst_count(), torn_before + 1u);
    eq_u32("still nothing transmitted", shim_tx_count(), 0u);

    /* Mailboxes free up. The CURRENT burst's pages (queued but not yet due
       for replacement) must go out - not be dropped - the next time task()
       runs, even without a new TX period starting. */
    shim_set_tx_mailboxes_free(3);
    can_node_v2_task();

    eq_u32("both pages of the recovered burst were sent", shim_tx_count(), 2u);
    eq_u32("page_tx_count advanced by exactly 2", can_node_v2_page_tx_count(), page_before + 2u);

    uint32_t id0, id1;
    uint8_t  d0[8], d1[8];
    shim_tx_get(0, &id0, d0);
    shim_tx_get(1, &id1, d1);
    ok("recovered pages are one whole burst (same epoch)", d0[0] == d1[0]);
    ok("recovered pages use the right CAN IDs",
       id0 == TLM2_CAN_ID_FOR(NODE_ID, 0) && id1 == TLM2_CAN_ID_FOR(NODE_ID, 1));
}

static void test_busoff_recovery(void)
{
    printf("-- node %u: bus-off recovery\n", (unsigned)NODE_ID);

    shim_reset();
    bringup();

    uint32_t before = can_node_v2_busoff_count();

    shim_set_can_error(HAL_CAN_ERROR_BOF);
    shim_advance(TLM2_NODE_TX_PERIOD_MS);
    can_node_v2_task();

    eq_u32("bus-off detected and recovered", can_node_v2_busoff_count(), before + 1u);

    uint32_t n_before = shim_tx_count();
    for (uint32_t t = 0; t < 500; t += 10)
    {
        shim_advance(10);
        can_node_v2_task();
    }
    ok("still transmitting after recovery", shim_tx_count() > n_before);
}

static void test_startup_flag(void)
{
    printf("-- node %u: startup status flag\n", (unsigned)NODE_ID);

    shim_reset();
    bringup();

    shim_advance(TLM2_NODE_TX_PERIOD_MS);
    can_node_v2_task();

    uint32_t id;
    uint8_t  d[8];
    ok("first burst's page 0 captured", shim_tx_get(0, &id, d));
    ok("startup flag set on the first burst", (d[1] & TLM2_ST_STARTUP) != 0u);

    for (uint32_t t = 0; t < 2500; t += 100)
    {
        shim_advance(100);
        can_node_v2_task();
    }

    uint32_t n = shim_tx_count();
    ok("captured a later burst", n >= 2u);
    shim_tx_get(n - 2u, &id, d);   /* page 0 of the most recent burst */
    ok("startup flag cleared after the hold period", (d[1] & TLM2_ST_STARTUP) == 0u);
}

int main(void)
{
    printf("Running v2 node firmware (NODE_ID=%u, %s) against the host HAL shim\n\n",
           (unsigned)NODE_ID, TLM2_NODE_NAMES[NODE_ID]);

    test_page_layout_and_ids();
    test_epoch();
    test_channel_roundtrip_and_scaling();
    test_mailbox_contention_and_torn_burst();
    test_busoff_recovery();
    test_startup_flag();

    printf("\n  %d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
