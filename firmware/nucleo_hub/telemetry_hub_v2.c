/**
 * telemetry_hub_v2.c - Multi-frame CAN aggregation, snapshot framing, radio and SD dispatch.
 *
 * Implements the telemetry_hub_v2.h contract for STM32L476RG Nucleo Hub.
 */

#include "telemetry_hub_v2.h"
#include "lora_e22.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

#if HUB2_ENABLE_SD
#include "sd_log.h"
#endif

/* ------------------------------------------------------------------ */
/* Math & Simulation Constants (matching simulation/vehicle_data.py)  */
/* ------------------------------------------------------------------ */

#define SIM_PI               3.14159265358979323846f
#define SIM_LAP_PERIOD_S     60.0f
#define SIM_MPH_PER_G_PER_S  21.937f
#define TRACK_CENTER_LAT     40.7660f
#define TRACK_CENTER_LON     -83.8220f
#define TRACK_RADIUS_DEG     0.00090f

static inline float clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static inline float maxf0(float v) { return v > 0.0f ? v : 0.0f; }

static inline float sim_speed_mph(float t)
{
    float phase = 2.0f * SIM_PI * t / SIM_LAP_PERIOD_S - SIM_PI / 2.0f;
    return 22.0f + 20.0f * sinf(phase);
}

static inline float sim_lon_accel_g(float t)
{
    float phase          = 2.0f * SIM_PI * t / SIM_LAP_PERIOD_S - SIM_PI / 2.0f;
    float d_speed_mph_dt = 20.0f * (2.0f * SIM_PI / SIM_LAP_PERIOD_S) * cosf(phase);
    return d_speed_mph_dt / SIM_MPH_PER_G_PER_S;
}

static inline float sim_lat_accel_g(float t)
{
    return 0.8f * sinf(2.0f * SIM_PI * t / SIM_LAP_PERIOD_S * 2.0f);
}

static inline float sim_bump(float t_elapsed, float amp)
{
    return amp * (sinf(37.0f * t_elapsed) * 0.6f + sinf(91.3f * t_elapsed) * 0.4f);
}

static inline float sim_warmup(float t_elapsed, float start, float end, float tau)
{
    return start + (end - start) * (1.0f - expf(-t_elapsed / tau));
}

static int32_t sim_to_raw(uint8_t node_id, unsigned ch, float value)
{
    const tlm2_chan_def_t *d = &TLM2_CHANNELS[node_id][ch];
    float raw = (value - d->offset) / d->scale;

    if (d->width == TLM2_W_I32)
    {
        if (raw >  2147483647.0f) raw =  2147483647.0f;
        if (raw < -2147483648.0f) raw = -2147483648.0f;
    }
    else
    {
        if (raw >  32767.0f) raw =  32767.0f;
        if (raw < -32768.0f) raw = -32768.0f;
    }

    return (int32_t)(raw >= 0.0f ? raw + 0.5f : raw - 0.5f);
}

/* ------------------------------------------------------------------ */
/* Lock-free single-producer / single-consumer ring buffer             */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t std_id;
    uint8_t  data[TLM2_CAN_DLC];
    uint32_t t_ms;
} can2_evt_t;

static can2_evt_t        s_q[HUB2_CAN_QUEUE_LEN];
static volatile uint32_t s_q_head;   /* written by ISR       */
static volatile uint32_t s_q_tail;   /* written by main loop */

#define Q_MASK (HUB2_CAN_QUEUE_LEN - 1u)
#if (HUB2_CAN_QUEUE_LEN & Q_MASK) != 0
#error "HUB2_CAN_QUEUE_LEN must be a power of two"
#endif

static inline bool q_push(const can2_evt_t *e)
{
    uint32_t head = s_q_head;
    uint32_t next = (head + 1u) & Q_MASK;

    if (next == s_q_tail)
    {
        return false;   /* full */
    }

    s_q[head] = *e;
    __DMB();
    s_q_head = next;
    return true;
}

static inline bool q_pop(can2_evt_t *out)
{
    uint32_t tail = s_q_tail;

    if (tail == s_q_head)
    {
        return false;   /* empty */
    }

    *out = s_q[tail];
    __DMB();
    s_q_tail = (tail + 1u) & Q_MASK;
    return true;
}

/* ------------------------------------------------------------------ */
/* Module state                                                        */
/* ------------------------------------------------------------------ */

static CAN_HandleTypeDef  *s_can;
static UART_HandleTypeDef *s_debug;

static tlm2_node_reassembler_t s_reasm[TLM2_NODE_COUNT];
static tlm2_frame_t            s_frame;
static hub2_stats_t            s_stats;

static uint32_t s_next_frame_ms;
static uint32_t s_next_node0_tx_ms;
static uint32_t s_next_node3_tx_ms;
static uint16_t s_frame_seq;

static uint8_t  s_epoch_hub;
static uint8_t  s_epoch_motor;

static uint8_t  s_txbuf[TLM2_FRAME_SIZE_MAX];

/* ------------------------------------------------------------------ */
/* CAN receive interrupt                                               */
/* ------------------------------------------------------------------ */

void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *hcan)
{
    CAN_RxHeaderTypeDef header;
    uint8_t             data[TLM2_CAN_DLC];

    if (hcan->Instance != CAN1)
    {
        return;
    }

    while (HAL_CAN_GetRxFifoFillLevel(hcan, CAN_RX_FIFO0) > 0u)
    {
        if (HAL_CAN_GetRxMessage(hcan, CAN_RX_FIFO0, &header, data) != HAL_OK)
        {
            break;
        }

        if (header.IDE != CAN_ID_STD)
        {
            s_stats.can_foreign++;
            continue;
        }

        can2_evt_t evt;
        evt.std_id = header.StdId;
        evt.t_ms   = HAL_GetTick();
        memcpy(evt.data, data, TLM2_CAN_DLC);

        uint32_t odrive_base = (uint32_t)(HUB2_ODRIVE_AXIS_NODE_ID << 5);

        if (TLM2_CAN_ID_IS_NODE(header.StdId))
        {
            if (q_push(&evt))
            {
                s_stats.can_rx++;
            }
            else
            {
                s_stats.can_dropped++;
            }
        }
        else if (header.StdId >= odrive_base && header.StdId < odrive_base + 0x20u)
        {
            if (q_push(&evt))
            {
                s_stats.can_odrive_rx++;
            }
            else
            {
                s_stats.can_dropped++;
            }
        }
        else
        {
            s_stats.can_foreign++;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Local Node Origination                                              */
/* ------------------------------------------------------------------ */

static void hub2_originate_node0(uint32_t now)
{
    int32_t raw[9];
    uint8_t status = TLM2_ST_OK;

    float t_elapsed = (float)now * 0.001f;
    float t         = fmodf(t_elapsed, SIM_LAP_PERIOD_S);
    if (t < 0.0f) t += SIM_LAP_PERIOD_S;

    float speed   = maxf0(sim_speed_mph(t));
    float lon_g   = sim_lon_accel_g(t);
    float lat_g   = sim_lat_accel_g(t);
    float heading = fmodf(t_elapsed / SIM_LAP_PERIOD_S * 360.0f, 360.0f);
    if (heading < 0.0f) heading += 360.0f;

    float gps_lat  = TRACK_CENTER_LAT + TRACK_RADIUS_DEG * sinf(2.0f * SIM_PI * t / SIM_LAP_PERIOD_S);
    float gps_lon  = TRACK_CENTER_LON + TRACK_RADIUS_DEG * cosf(2.0f * SIM_PI * t / SIM_LAP_PERIOD_S);
    float gps_sats = 9.0f + roundf(sinf(t_elapsed / 13.0f));
    float accel_z  = 1.0f + sim_bump(t_elapsed, 0.06f);
    float gyro_z   = lat_g * 60.0f;

    raw[0] = sim_to_raw(0, 0, gps_lat);
    raw[1] = sim_to_raw(0, 1, gps_lon);
    raw[2] = sim_to_raw(0, 2, speed);
    raw[3] = sim_to_raw(0, 3, heading);
    raw[4] = sim_to_raw(0, 4, gps_sats);
    raw[5] = sim_to_raw(0, 5, lat_g);
    raw[6] = sim_to_raw(0, 6, lon_g);
    raw[7] = sim_to_raw(0, 7, accel_z);
    raw[8] = sim_to_raw(0, 8, gyro_z);

    uint8_t pages[TLM2_MAX_PAGES_PER_NODE][TLM2_CAN_DLC];
    uint8_t n_pages = tlm2_can_pack_pages(0, raw, 9, s_epoch_hub++, status, pages);

    for (uint8_t p = 0; p < n_pages; p++)
    {
        tlm2_reasm_apply_page(&s_reasm[0], 0, p, pages[p], now);
    }
}

static void hub2_originate_node3(uint32_t now)
{
    int32_t raw[5];
    uint8_t status = TLM2_ST_OK;

    float t_elapsed = (float)now * 0.001f;
    float t         = fmodf(t_elapsed, SIM_LAP_PERIOD_S);
    if (t < 0.0f) t += SIM_LAP_PERIOD_S;

    float speed = maxf0(sim_speed_mph(t));
    float lon_g = sim_lon_accel_g(t);

    float motor_current    = clampf(5.0f + 40.0f * fabsf(lon_g), 0.0f, 60.0f);
    float motor_velocity   = clampf(speed * 130.0f, 0.0f, 6000.0f);
    float motor_temp       = sim_warmup(t_elapsed, 30.0f, 92.0f, 240.0f) + 0.3f * motor_current;
    float bus_voltage      = 55.0f - 4.0f * (motor_current / 60.0f) + sim_bump(t_elapsed, 0.15f);
    float brake_resistor_w = clampf(-lon_g * 45.0f, 0.0f, 50.0f);

    raw[0] = sim_to_raw(3, 0, motor_current);
    raw[1] = sim_to_raw(3, 1, motor_velocity);
    raw[2] = sim_to_raw(3, 2, motor_temp);
    raw[3] = sim_to_raw(3, 3, bus_voltage);
    raw[4] = sim_to_raw(3, 4, brake_resistor_w);

    uint8_t pages[TLM2_MAX_PAGES_PER_NODE][TLM2_CAN_DLC];
    uint8_t n_pages = tlm2_can_pack_pages(3, raw, 5, s_epoch_motor++, status, pages);

    for (uint8_t p = 0; p < n_pages; p++)
    {
        tlm2_reasm_apply_page(&s_reasm[3], 3, p, pages[p], now);
    }
}

/* ------------------------------------------------------------------ */
/* Debug UART Print                                                   */
/* ------------------------------------------------------------------ */

#if HUB2_DEBUG_UART
static void debug_print_v2(const tlm2_frame_t *f)
{
    if (s_debug == NULL)
    {
        return;
    }

    /* busoff/sd_errors/can_dropped added 2026-08-16 for the SD + fault-
       recovery bench stages (PROJECT_STATUS.md Track A) - these counters
       already existed in s_stats but were never printed anywhere, so there
       was no way to actually see "did a bus-off get detected and recovered"
       or "did an SD write fail" while running the test, only to infer it
       from the node STALE flags (which only tell you about missing CAN
       pages, not the hub's own SD/bus-health bookkeeping). */
    /* SIM3=%d added 2026-08-16: a one-time boot-banner print of
       HUB2_SIMULATE_NODE3 turned out to be useless for bench debugging -
       it fires within ms of power-up, before a human can plug in a monitor
       and see it, so a stale-build question ("is the chip really running
       node3-not-simulated firmware?") could never actually get answered by
       reading it. Printing it on every line instead means it's just always
       sitting on screen - no timing race, no re-flash-and-catch-it dance. */
    char line[256];
    int len = snprintf(line, sizeof(line),
                       "#%u t=%lu | N0[GPS/IMU]%s | N1[Front]%s | N2[Rear]%s | N3[Motor]%s "
                       "| busoff=%lu sd_err=%lu can_drop=%lu SIM3=%d\r\n",
                       (unsigned)f->seq, (unsigned long)f->t_ms,
                       (f->nodes[0].flags & TLM2_NF_STALE) ? "!" : "",
                       (f->nodes[1].flags & TLM2_NF_STALE) ? "!" : "",
                       (f->nodes[2].flags & TLM2_NF_STALE) ? "!" : "",
                       (f->nodes[3].flags & TLM2_NF_STALE) ? "!" : "",
                       (unsigned long)s_stats.busoff_events,
                       (unsigned long)s_stats.sd_errors,
                       (unsigned long)s_stats.can_dropped,
                       (int)HUB2_SIMULATE_NODE3);

    HAL_UART_Transmit(s_debug, (uint8_t *)line, (uint16_t)len, 50u);
}
#endif

static void check_bus_health_v2(void)
{
    uint32_t err = HAL_CAN_GetError(s_can);

    if (err & HAL_CAN_ERROR_BOF)
    {
        HAL_CAN_Stop(s_can);
        HAL_CAN_ResetError(s_can);
        HAL_CAN_Start(s_can);
        HAL_CAN_ActivateNotification(s_can, CAN_IT_RX_FIFO0_MSG_PENDING);
        s_stats.busoff_events++;
    }
    else if (err != HAL_CAN_ERROR_NONE)
    {
        HAL_CAN_ResetError(s_can);
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void hub2_init(CAN_HandleTypeDef *hcan,
               UART_HandleTypeDef *lora,
               UART_HandleTypeDef *debug)
{
    s_can   = hcan;
    s_debug = debug;

    memset(&s_stats, 0, sizeof(s_stats));
    for (uint8_t i = 0; i < TLM2_NODE_COUNT; i++)
    {
        tlm2_reasm_init(&s_reasm[i]);
    }

    if (HUB2_ODRIVE_AXIS_NODE_ID == 16u)
    {
        Error_Handler();
    }

    CAN_FilterTypeDef filter = {0};
    filter.FilterBank           = 0;
    filter.FilterMode           = CAN_FILTERMODE_IDMASK;
    filter.FilterScale          = CAN_FILTERSCALE_32BIT;
    filter.FilterIdHigh         = (uint16_t)(TLM2_CAN_DATA_BASE_ID << 5);
    filter.FilterIdLow          = 0x0000u;
    filter.FilterMaskIdHigh     = (uint16_t)(0x7E0u << 5); /* match 0x200..0x21F */
    filter.FilterMaskIdLow      = 0x0000u;
    filter.FilterFIFOAssignment = CAN_RX_FIFO0;
    filter.FilterActivation     = ENABLE;
    filter.SlaveStartFilterBank = 14;

    if (HAL_CAN_ConfigFilter(s_can, &filter) != HAL_OK)
    {
        Error_Handler();
    }

    if (HAL_CAN_Start(s_can) != HAL_OK)
    {
        Error_Handler();
    }

    if (HAL_CAN_ActivateNotification(s_can, CAN_IT_RX_FIFO0_MSG_PENDING) != HAL_OK)
    {
        Error_Handler();
    }

    lora_e22_init(lora);

#if HUB2_ENABLE_SD
    if (!sd_log_init())
    {
        s_stats.sd_errors++;
    }
#endif

    uint32_t now = HAL_GetTick();
    s_next_frame_ms     = now + TLM2_FRAME_PERIOD_MS;
    s_next_node0_tx_ms  = now + TLM2_NODE_TX_PERIOD_MS;
    s_next_node3_tx_ms  = now + TLM2_NODE_TX_PERIOD_MS;

#if HUB2_DEBUG_UART
    if (s_debug != NULL)
    {
        /* HUB2_SIMULATE_NODE3 printed here, 2026-08-16: bench testing kept
           showing the E-CVT stand-in as "always live" even with its power
           unplugged, which only happens if the hub is still fabricating
           node 3 locally - i.e. this macro is evaluating to 1 in whatever
           binary is actually on the chip, regardless of what main.c's
           source says. Rather than keep inferring that from node behavior,
           print the compiled-in value straight into the boot banner so a
           stale build is obvious the instant the board powers up. */
        char banner[64];
        int blen = snprintf(banner, sizeof(banner),
                            "\r\n== telemetry hub v2 ready == SIMULATE_NODE3=%d\r\n",
                            (int)HUB2_SIMULATE_NODE3);
        HAL_UART_Transmit(s_debug, (uint8_t *)banner, (uint16_t)blen, 100u);
    }
#endif
}

void hub2_task(void)
{
    /* 1. Drain everything queued by CAN RX ISR */
    can2_evt_t evt;
    while (q_pop(&evt))
    {
        if (TLM2_CAN_ID_IS_NODE(evt.std_id))
        {
            uint8_t node = TLM2_CAN_NODE_FROM_ID(evt.std_id);
            uint8_t page = TLM2_CAN_PAGE_FROM_ID(evt.std_id);
            if (node < TLM2_NODE_COUNT)
            {
                tlm2_reasm_apply_page(&s_reasm[node], node, page, evt.data, evt.t_ms);
            }
        }
    }

    /* 2. Service LoRa */
    lora_e22_task();

    /* 3. Local node origination at 10 Hz */
    uint32_t now = HAL_GetTick();

    if ((int32_t)(now - s_next_node0_tx_ms) >= 0)
    {
        s_next_node0_tx_ms += TLM2_NODE_TX_PERIOD_MS;
        hub2_originate_node0(now);
    }

#if HUB2_SIMULATE_NODE3
    if ((int32_t)(now - s_next_node3_tx_ms) >= 0)
    {
        s_next_node3_tx_ms += TLM2_NODE_TX_PERIOD_MS;
        hub2_originate_node3(now);
    }
#endif
    /* HUB2_SIMULATE_NODE3 == 0: node 3's pages arrive over real CAN instead
       (0x218-0x21F), same as nodes 1/2 - handled by the RX ISR + the drain
       loop above, nothing extra needed here. See HUB2_SIMULATE_NODE3's
       comment in telemetry_hub_v2.h. */

    /* 4. Periodic Snapshot & Radio/SD Dispatch at 2 Hz */
    if ((int32_t)(now - s_next_frame_ms) < 0)
    {
        return;
    }

    s_next_frame_ms += TLM2_FRAME_PERIOD_MS;
    if ((int32_t)(now - s_next_frame_ms) > (int32_t)(TLM2_FRAME_PERIOD_MS * 4u))
    {
        s_next_frame_ms = now + TLM2_FRAME_PERIOD_MS;
    }

    check_bus_health_v2();

    s_frame.seq  = s_frame_seq++;
    s_frame.t_ms = now;

    for (uint8_t i = 0; i < TLM2_NODE_COUNT; i++)
    {
        tlm2_reasm_snapshot(&s_reasm[i], i, now, &s_frame.nodes[i]);
    }

    size_t n = tlm2_encode_frame(&s_frame, s_txbuf, sizeof(s_txbuf));
    if (n == 0u)
    {
        return;
    }

    if (lora_e22_send(s_txbuf, n))
    {
        s_stats.frames_sent++;
    }
    else
    {
        s_stats.lora_busy++;
    }

#if HUB2_ENABLE_SD
    if (!sd_log_write(s_txbuf, n))
    {
        s_stats.sd_errors++;
    }
#endif

#if HUB2_BINARY_UART
    /*
     * Same bytes as the radio and the SD card (s_txbuf/n), non-blocking, and
     * dropped rather than deferred if the VCP is still busy. See the long
     * comment on the equivalent block in telemetry_hub.c for the full
     * reasoning - the short version is that the CAN ring buffer's margin is
     * budgeted for SD stalls, not UART stalls, so the mirror must never be
     * allowed to cost a CAN frame.
     */
    if (s_debug != NULL)
    {
        if (HAL_UART_Transmit_DMA(s_debug, s_txbuf, (uint16_t)n) != HAL_OK)
        {
            s_stats.vcp_dropped++;
        }
    }
#endif

#if HUB2_DEBUG_UART
    debug_print_v2(&s_frame);
#endif
}

void hub2_get_stats(hub2_stats_t *out)
{
    *out = s_stats;
}

const tlm2_frame_t *hub2_last_frame(void)
{
    return &s_frame;
}
