/*
 * test_paths_agree.c - Prove the three data paths carry the same thing.
 *
 * The claim this file exists to enforce: what the hub sends over the radio,
 * what it writes to the SD card, and what actually arrived over CAN are all
 * the same data. Until now that was true only "by construction" - you had to
 * read telemetry_hub.c and notice both calls are handed the same s_txbuf.
 * Nothing caught a regression, and the main firmware suite compiles the SD
 * path out entirely (-DHUB_ENABLE_SD=0), so the SD leg was never exercised
 * at all.
 *
 * That is a bad thing to leave untested, because the failure mode is silent.
 * If someone later adds a timestamp to the SD record, or reorders a field on
 * one path, everything still runs and the logs quietly stop matching the live
 * telemetry. You would not find out until you replayed a log months later and
 * the numbers disagreed with what you remembered seeing at the track.
 *
 * Built with HUB_ENABLE_SD=1 and a capturing sd_log stub. The LoRa side needs
 * no stub - hal_shim already records everything the radio driver transmits.
 *
 *   leg 1  LoRa bytes == SD bytes
 *   leg 2  those bytes decode back to the values that went in over CAN
 *   leg 3  a deliberately corrupted byte IS caught (the check has teeth)
 *
 * Leg 2 is what makes this more than a tautology: it proves the encode step
 * did not mangle anything, not merely that both paths got the same buffer.
 */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "main.h"
#include "telemetry_proto.h"
#include "telemetry_hub.h"

static int checks = 0, failures = 0;

static void ok(const char *what, bool cond)
{
    checks++;
    if (!cond) { failures++; printf("  FAIL: %s\n", what); }
}

static void eq_long(const char *what, long got, long want)
{
    checks++;
    if (got != want) {
        failures++;
        printf("  FAIL: %s (got %ld, want %ld)\n", what, got, want);
    }
}

/* ------------------------------------------------------------------ */
/* Capturing sd_log stub. sd_log.c itself is not in this build - it is */
/* SPI/FAT32 register code with its own suite (test_fat32.py); what    */
/* matters here is only WHAT the hub hands it.                         */
/* ------------------------------------------------------------------ */

#define SD_MAX 4096
static uint8_t  sd_buf[SD_MAX];
static uint32_t sd_used;
static unsigned sd_calls;

bool sd_log_init(void)            { return true; }
bool sd_log_flush(void)           { return true; }
void sd_log_close(void)           {}
bool sd_log_ready(void)           { return true; }
uint32_t sd_log_bytes(void)       { return sd_used; }
const char *sd_log_filename(void) { return "TEST0001.TLM"; }

bool sd_log_write(const uint8_t *data, size_t len)
{
    sd_calls++;
    if (sd_used + len <= SD_MAX) {
        memcpy(sd_buf + sd_used, data, len);
        sd_used += (uint32_t)len;
    }
    return true;
}

/* ------------------------------------------------------------------ */

/* Provided by hal_shim.c but not declared in main.h - same local declaration
   tools/test_firmware.c uses. */
void shim_bind_hub_can(CAN_HandleTypeDef *h);

static CAN_HandleTypeDef  hub_can;
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

static void inject(uint8_t node, int16_t a, int16_t b, int16_t c, uint8_t seq)
{
    tlm_node_sample_t s;
    s.ch[0] = a; s.ch[1] = b; s.ch[2] = c;
    s.seq = seq; s.status = TLM_ST_OK;

    uint8_t d[8];
    tlm_can_pack(&s, d);
    shim_can_inject(TLM_CAN_ID_FOR_NODE(node), d);
}

int main(void)
{
    printf("Do the CAN, LoRa and SD paths agree?\n\n");

    shim_reset();
    sd_used = 0; sd_calls = 0;
    hub_bringup();

    /* Distinguishable per node and per channel, including negatives, so a
       byte-order or sign bug cannot slip through looking plausible. */
    static const int16_t VALS[TLM_NODE_COUNT][3] = {
        {  2250,  5013, 10050 },
        { 12870, -1234,  4321 },
        {  -450,   450,  1000 },
    };

    printf("-- feeding CAN, running the hub\n");
    for (unsigned n = 0; n < TLM_NODE_COUNT; n++)
        inject((uint8_t)n, VALS[n][0], VALS[n][1], VALS[n][2], 0u);

    shim_lora_clear();
    for (uint32_t t = 0; t < 1600u; t += 50u) {
        shim_advance(50);
        hub_task();
        /* keep every node fresh so none goes stale mid-run */
        for (unsigned n = 0; n < TLM_NODE_COUNT; n++)
            inject((uint8_t)n, VALS[n][0], VALS[n][1], VALS[n][2],
                   (uint8_t)(t / 50u));
    }

    const uint32_t lora_bytes = shim_lora_len();
    const unsigned frames = (unsigned)(lora_bytes / TLM_FRAME_SIZE);

    printf("-- radio sent %u byte(s) = %u frame(s); SD took %u write(s), %u byte(s)\n",
           lora_bytes, frames, sd_calls, sd_used);

    ok("the hub emitted at least two frames", frames >= 2);
    eq_long("SD write count matches frame count", (long)sd_calls, (long)frames);
    eq_long("SD byte count matches radio byte count", (long)sd_used, (long)lora_bytes);

    /* ---- leg 1: byte-for-byte ---- */
    printf("-- leg 1: LoRa bytes vs SD bytes\n");
    ok("every SD byte is identical to what went over the air",
       sd_used == lora_bytes && memcmp(sd_buf, shim_lora_data(), sd_used) == 0);

    /* ---- leg 2: those bytes still mean what CAN said ---- */
    printf("-- leg 2: decoded bytes vs what CAN carried\n");
    for (unsigned i = 0; i < frames; i++) {
        const uint8_t *p = shim_lora_data() + (size_t)i * TLM_FRAME_SIZE;

        tlm_frame_t f;
        int rc = tlm_decode_frame(p, TLM_FRAME_SIZE, &f);
        eq_long("frame decodes cleanly (sync, version, length, CRC)", rc, 0);
        if (rc != 0) continue;

        for (unsigned n = 0; n < TLM_NODE_COUNT; n++) {
            /* An early snapshot can predate a node's first CAN frame, so only
               assert on nodes the hub itself considers online. */
            if (!(f.nodes[n].flags & TLM_NF_ONLINE)) continue;
            eq_long("node id lands in the right slot", f.nodes[n].node_id, (long)n);
            eq_long("channel 0 survived CAN -> frame", f.nodes[n].ch[0], VALS[n][0]);
            eq_long("channel 1 survived CAN -> frame", f.nodes[n].ch[1], VALS[n][1]);
            eq_long("channel 2 survived CAN -> frame", f.nodes[n].ch[2], VALS[n][2]);
        }
    }

    /* ---- leg 3: make sure leg 1 could actually fail ---- */
    printf("-- leg 3: would a divergence be caught?\n");
    if (sd_used > TLM_OFF_PAYLOAD) {
        uint8_t saved = sd_buf[TLM_OFF_PAYLOAD];
        sd_buf[TLM_OFF_PAYLOAD] = (uint8_t)(saved ^ 0xFFu);
        ok("flipping one SD byte is detected (leg 1 is not vacuous)",
           memcmp(sd_buf, shim_lora_data(), sd_used) != 0);
        sd_buf[TLM_OFF_PAYLOAD] = saved;
    }

    printf("\n  %d/%d checks passed\n", checks - failures, checks);
    if (failures == 0)
        printf("  CAN, LoRa and SD all carry the same data.\n");
    return failures == 0 ? 0 : 1;
}
