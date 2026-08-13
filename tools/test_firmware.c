/**
 * test_firmware.c - Runs the real node and hub firmware on the host.
 *
 * can_node.c, telemetry_hub.c, lora_e22.c and telemetry_proto.c are compiled
 * unmodified against tools/hal_shim/, which fakes the HAL and puts HAL_GetTick()
 * under test control. That makes the timing-dependent behaviour - transmit
 * cadence, node timeouts, sequence-loss arithmetic across the 8-bit wrap -
 * directly testable, which is the part that cannot be checked by reading the
 * code and is miserable to debug on hardware.
 *
 * Node 0 runs for real. Nodes 1-2 are synthesised, because NODE_ID is a
 * compile-time constant and only one value can exist in a single binary.
 *
 * Built and driven by tools/test_firmware.py.
 */

#include "can_node.h"
#include "lora_e22.h"
#include "telemetry_hub.h"
#include "telemetry_proto.h"
#include <stdio.h>
#include <string.h>

void shim_bind_hub_can(CAN_HandleTypeDef *h);

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

/* ------------------------------------------------------------------ */
/* Node                                                                */
/* ------------------------------------------------------------------ */

static CAN_HandleTypeDef node_can;

static void test_node_cadence(void)
{
    printf("-- node: transmit cadence and payload\n");

    shim_reset();
    memset(&node_can, 0, sizeof(node_can));
    node_can.Instance = CAN1;
    can_node_init(&node_can);

    /* Run 5 simulated seconds, polling far faster than the transmit period to
       prove the task is non-blocking and does not fire early. */
    for (uint32_t t = 0; t < 5000; t += 5)
    {
        shim_advance(5);
        can_node_task();
    }

    uint32_t n = shim_tx_count();

    /* 5000 ms / 100 ms, allowing one frame of slack at the boundaries. */
    ok("node sends ~50 frames in 5 s", n >= 49 && n <= 51);

    uint32_t id;
    uint8_t d[8];
    int seq_ok = 1, id_ok = 1, range_ok = 1;

    for (uint32_t i = 0; i < n; i++)
    {
        shim_tx_get(i, &id, d);

        if (id != TLM_CAN_ID_FOR_NODE(0)) id_ok = 0;

        tlm_node_sample_t s;
        tlm_can_unpack(d, &s);

        if (s.seq != (uint8_t)i) seq_ok = 0;

        /* Node 0 is environmental: 15-30 degC, 30-70 %RH, 98-102 kPa.
           Checking engineering ranges rather than raw counts catches a wrong
           scale factor, which raw-value checks would sail straight past. */
        float temp = s.ch[0] * TLM_CHANNELS[0][0].scale;
        float hum  = s.ch[1] * TLM_CHANNELS[0][1].scale;
        float pres = s.ch[2] * TLM_CHANNELS[0][2].scale;

        if (temp < 14.0f || temp > 31.0f) range_ok = 0;
        if (hum  < 29.0f || hum  > 71.0f) range_ok = 0;
        if (pres < 97.0f || pres > 103.0f) range_ok = 0;
    }

    ok("node uses CAN ID 0x100", id_ok);
    ok("sequence increments by one per frame", seq_ok);
    ok("channels decode into plausible engineering ranges", range_ok);

    /* Startup flag must latch then clear. */
    shim_tx_get(0, &id, d);
    tlm_node_sample_t first;
    tlm_can_unpack(d, &first);
    ok("startup flag set on the first frame", (first.status & TLM_ST_STARTUP) != 0);

    shim_tx_get(n - 1, &id, d);
    tlm_node_sample_t last;
    tlm_can_unpack(d, &last);
    ok("startup flag cleared later", (last.status & TLM_ST_STARTUP) == 0);
}

static void test_node_mailbox_full(void)
{
    printf("-- node: no free mailbox\n");

    shim_reset();
    memset(&node_can, 0, sizeof(node_can));
    node_can.Instance = CAN1;
    can_node_init(&node_can);

    shim_set_tx_mailboxes_free(0);

    uint32_t before_drops = can_node_drop_count();

    for (uint32_t t = 0; t < 1000; t += 10)
    {
        shim_advance(10);
        can_node_task();
    }

    eq_u32("nothing transmitted", shim_tx_count(), 0);
    ok("drops counted", can_node_drop_count() > before_drops);

    /* The old firmware busy-waited here for up to 50 ms per attempt. The point
       of the change is that the task returns immediately instead. */
    shim_set_tx_mailboxes_free(3);
    shim_advance(200);
    can_node_task();
    ok("recovers once a mailbox frees", shim_tx_count() > 0);
}

static void test_node_busoff(void)
{
    printf("-- node: bus-off recovery\n");

    shim_reset();
    memset(&node_can, 0, sizeof(node_can));
    node_can.Instance = CAN1;
    can_node_init(&node_can);

    uint32_t before = can_node_busoff_count();

    shim_set_can_error(HAL_CAN_ERROR_BOF);
    shim_advance(200);
    can_node_task();

    ok("bus-off detected and recovered", can_node_busoff_count() == before + 1);

    /* After recovery the node must keep transmitting rather than latch off. */
    uint32_t n_before = shim_tx_count();
    for (uint32_t t = 0; t < 500; t += 10)
    {
        shim_advance(10);
        can_node_task();
    }
    ok("still transmitting after recovery", shim_tx_count() > n_before);
}

/* ------------------------------------------------------------------ */
/* Hub                                                                 */
/* ------------------------------------------------------------------ */

static CAN_HandleTypeDef hub_can;
static UART_HandleTypeDef lora_uart, debug_uart;
static USART_TypeDef u1, u2;

static void hub_bringup(void)
{
    memset(&hub_can, 0, sizeof(hub_can));
    hub_can.Instance = CAN1;
    lora_uart.Instance = &u1;  lora_uart.gState = HAL_UART_STATE_READY;
    debug_uart.Instance = &u2; debug_uart.gState = HAL_UART_STATE_READY;

    shim_bind_hub_can(&hub_can);
    hub_init(&hub_can, &lora_uart, &debug_uart);
}

static void inject(uint8_t node, int16_t a, int16_t b, int16_t c, uint8_t seq, uint8_t status)
{
    tlm_node_sample_t s;
    s.ch[0] = a; s.ch[1] = b; s.ch[2] = c;
    s.seq = seq; s.status = status;

    uint8_t d[8];
    tlm_can_pack(&s, d);
    shim_can_inject(TLM_CAN_ID_FOR_NODE(node), d);
}

/** Run the hub forward, returning how many complete frames it emitted. */
static int hub_run(uint32_t ms, uint32_t step)
{
    shim_lora_clear();
    for (uint32_t t = 0; t < ms; t += step)
    {
        shim_advance(step);
        hub_task();
    }
    return (int)(shim_lora_len() / TLM_FRAME_SIZE);
}

static int last_frame(tlm_frame_t *out)
{
    uint32_t n = shim_lora_len();
    if (n < TLM_FRAME_SIZE) return 0;
    const uint8_t *p = shim_lora_data() + (n / TLM_FRAME_SIZE - 1) * TLM_FRAME_SIZE;
    return tlm_decode_frame(p, TLM_FRAME_SIZE, out) == 0;
}

/**
 * Largest loss value reported for @p node across every frame in the capture.
 *
 * Loss is reported once and then cleared, by design. A window boundary can
 * fall anywhere relative to when the frames were injected, so reading only the
 * last frame is a coin flip - scan the whole capture instead.
 */
static uint32_t max_loss(uint8_t node)
{
    uint32_t count = shim_lora_len() / TLM_FRAME_SIZE;
    uint32_t worst = 0;

    for (uint32_t i = 0; i < count; i++)
    {
        tlm_frame_t f;
        if (tlm_decode_frame(shim_lora_data() + i * TLM_FRAME_SIZE,
                             TLM_FRAME_SIZE, &f) == 0)
        {
            if (f.nodes[node].loss > worst)
            {
                worst = f.nodes[node].loss;
            }
        }
    }

    return worst;
}

static void test_hub_basic(void)
{
    printf("-- hub: aggregation and framing\n");

    shim_reset();
    hub_bringup();

    for (int cycle = 0; cycle < 6; cycle++)
    {
        for (uint8_t n = 0; n < TLM_NODE_COUNT; n++)
        {
            inject(n, (int16_t)(100 + n), (int16_t)(-200 - n), (int16_t)(3000 + n),
                   (uint8_t)cycle, TLM_ST_OK);
        }
        shim_advance(100);
        hub_task();
    }

    int frames = hub_run(1200, 50);
    ok("hub emits frames on schedule", frames >= 2 && frames <= 3);

    /* Everything the radio sent must decode: sync, version, length and CRC. */
    int all_decode = 1;
    for (int i = 0; i < frames; i++)
    {
        tlm_frame_t f;
        if (tlm_decode_frame(shim_lora_data() + i * TLM_FRAME_SIZE,
                             TLM_FRAME_SIZE, &f) != 0)
        {
            all_decode = 0;
        }
    }
    ok("every emitted frame decodes with a valid CRC", all_decode);

    eq_u32("frame is 42 bytes", (uint32_t)TLM_FRAME_SIZE, 42u);
}

static void test_hub_values(void)
{
    printf("-- hub: latest values reach the frame\n");

    shim_reset();
    hub_bringup();

    for (uint8_t n = 0; n < TLM_NODE_COUNT; n++)
    {
        inject(n, (int16_t)(1000 + n), (int16_t)(-1000 - n), (int16_t)(n * 7), 0, TLM_ST_OK);
    }
    hub_task();

    /* Overwrite with a second sample: the frame must carry the newer one. */
    for (uint8_t n = 0; n < TLM_NODE_COUNT; n++)
    {
        inject(n, (int16_t)(2000 + n), (int16_t)(-2000 - n), (int16_t)(n * 9), 1, TLM_ST_OK);
    }

    hub_run(600, 50);

    tlm_frame_t f;
    ok("frame decodes", last_frame(&f));

    int values_ok = 1, online_ok = 1;
    for (uint8_t n = 0; n < TLM_NODE_COUNT; n++)
    {
        if (f.nodes[n].node_id != n) values_ok = 0;
        if (f.nodes[n].ch[0] != (int16_t)(2000 + n)) values_ok = 0;
        if (f.nodes[n].ch[1] != (int16_t)(-2000 - n)) values_ok = 0;
        if (f.nodes[n].ch[2] != (int16_t)(n * 9)) values_ok = 0;
        if (!(f.nodes[n].flags & TLM_NF_ONLINE)) online_ok = 0;
        if (f.nodes[n].flags & TLM_NF_STALE) online_ok = 0;
    }
    ok("latest value from each node is carried", values_ok);
    ok("all three nodes marked online", online_ok);
}

static void test_hub_loss(void)
{
    printf("-- hub: sequence-loss arithmetic\n");

    shim_reset();
    hub_bringup();

    /* Establish a baseline, then skip three sequence numbers on node 2. */
    inject(2, 1, 2, 3, 10, TLM_ST_OK);
    hub_task();
    hub_run(600, 50);          /* flush the window that saw seq 10 */

    inject(2, 4, 5, 6, 14, TLM_ST_OK);   /* 11, 12, 13 missing */
    hub_run(1100, 50);

    tlm_frame_t f;
    ok("frame decodes", last_frame(&f));
    eq_u32("three missed frames reported", max_loss(2), 3u);
    eq_u32("last sequence number reported", f.nodes[2].can_seq, 14u);

    /* Loss must reset once reported rather than accumulate forever. */
    inject(2, 7, 8, 9, 15, TLM_ST_OK);
    hub_run(1100, 50);
    ok("frame decodes", last_frame(&f));
    eq_u32("loss counter resets after being reported", max_loss(2), 0u);

    /* The 8-bit wrap: 254 -> 1 means two frames went missing, not 253. */
    inject(2, 1, 1, 1, 254, TLM_ST_OK);
    hub_run(1100, 50);
    inject(2, 2, 2, 2, 1, TLM_ST_OK);
    hub_run(1100, 50);
    ok("frame decodes", last_frame(&f));
    eq_u32("loss correct across the 255->0 wrap", max_loss(2), 2u);
    eq_u32("sequence number after wrap", f.nodes[2].can_seq, 1u);
}

static void test_hub_stale(void)
{
    printf("-- hub: node timeout\n");

    shim_reset();
    hub_bringup();

    for (uint8_t n = 0; n < TLM_NODE_COUNT; n++)
    {
        inject(n, 5, 5, 5, 0, TLM_ST_OK);
    }
    hub_run(600, 50);

    tlm_frame_t f;
    last_frame(&f);
    ok("node 1 online while transmitting", (f.nodes[1].flags & TLM_NF_ONLINE) != 0);

    /* Keep nodes 0, 2 and 3 alive; let node 1 go quiet. */
    for (int i = 0; i < 40; i++)
    {
        shim_advance(50);
        hub_task();
        if (i % 2 == 0)
        {
            inject(0, 5, 5, 5, (uint8_t)i, TLM_ST_OK);
            inject(2, 5, 5, 5, (uint8_t)i, TLM_ST_OK);
            inject(3, 5, 5, 5, (uint8_t)i, TLM_ST_OK);
        }
    }

    hub_run(600, 50);
    ok("frame decodes", last_frame(&f));

    ok("silent node marked stale",   (f.nodes[1].flags & TLM_NF_STALE) != 0);
    ok("silent node not marked online", (f.nodes[1].flags & TLM_NF_ONLINE) == 0);
    ok("live node still online",     (f.nodes[0].flags & TLM_NF_ONLINE) != 0);
    ok("live node not stale",        (f.nodes[0].flags & TLM_NF_STALE) == 0);

    /* And it must come back when the node does. */
    inject(1, 9, 9, 9, 200, TLM_ST_OK);
    hub_run(600, 50);
    last_frame(&f);
    ok("recovers when the node returns", (f.nodes[1].flags & TLM_NF_ONLINE) != 0);
}

static void test_hub_fault_and_foreign(void)
{
    printf("-- hub: fault flag and ID filtering\n");

    shim_reset();
    hub_bringup();

    inject(0, 1, 1, 1, 0, TLM_ST_SENSOR_FAULT);
    hub_run(600, 50);

    tlm_frame_t f;
    last_frame(&f);
    ok("sensor fault propagates to the frame", (f.nodes[0].flags & TLM_NF_FAULT) != 0);

    hub_stats_t before, after;
    hub_get_stats(&before);

    /* An ID outside 0x100-0x103 must be counted and discarded, not decoded as
       a node record. In hardware the filter rejects these; the software path
       is the backstop for a misconfigured filter. */
    uint8_t junk[8] = {1,2,3,4,5,6,7,8};
    shim_can_inject(0x200, junk);
    shim_can_inject(0x7FF, junk);
    hub_task();

    hub_get_stats(&after);
    eq_u32("foreign IDs counted", after.can_foreign - before.can_foreign, 2u);
    eq_u32("foreign IDs not accepted", after.can_rx - before.can_rx, 0u);
}

static void test_hub_queue_overflow(void)
{
    printf("-- hub: ring buffer overflow\n");

    shim_reset();
    hub_bringup();

    hub_stats_t st;

    /* Flood past the queue depth without letting the main loop drain it. The
       ISR must count the loss and keep the buffer coherent rather than
       corrupting it or wedging. */
    for (uint32_t i = 0; i < HUB_CAN_QUEUE_LEN * 3u; i++)
    {
        inject((uint8_t)(i % TLM_NODE_COUNT), (int16_t)i, 0, 0, (uint8_t)i, TLM_ST_OK);
    }

    hub_get_stats(&st);
    ok("overflow counted", st.can_dropped > 0);

    /* Draining must still work and produce a valid frame afterwards. */
    hub_run(600, 50);
    tlm_frame_t f;
    ok("hub still emits a valid frame after overflow", last_frame(&f));
}

int main(void)
{
    printf("Running node and hub firmware against the host HAL shim\n\n");

    test_node_cadence();
    test_node_mailbox_full();
    test_node_busoff();

    test_hub_basic();
    test_hub_values();
    test_hub_loss();
    test_hub_stale();
    test_hub_fault_and_foreign();
    test_hub_queue_overflow();

    printf("\n  %d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
