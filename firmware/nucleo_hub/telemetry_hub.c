/**
 * telemetry_hub.c - CAN aggregation, snapshot framing, radio and SD dispatch.
 */

#include "telemetry_hub.h"
#include "lora_e22.h"
#include <string.h>
#include <stdio.h>

#if HUB_ENABLE_SD
#include "sd_log.h"
#endif

/* ------------------------------------------------------------------ */
/* Lock-free single-producer / single-consumer ring buffer             */
/* ------------------------------------------------------------------ */
/*
 * Producer is the CAN RX interrupt, consumer is the main loop. With exactly
 * one writer of head and one writer of tail, and a power-of-two capacity so
 * the index wrap is a single AND, no critical section is required on
 * Cortex-M: 32-bit aligned loads and stores are atomic.
 *
 * The counters are declared volatile so the compiler cannot cache them across
 * the interrupt boundary.
 */

typedef struct {
    uint8_t  node_id;
    uint8_t  data[TLM_CAN_DLC];
    uint32_t t_ms;
} can_evt_t;

static can_evt_t         s_q[HUB_CAN_QUEUE_LEN];
static volatile uint32_t s_q_head;   /* written by ISR       */
static volatile uint32_t s_q_tail;   /* written by main loop */

#define Q_MASK (HUB_CAN_QUEUE_LEN - 1u)
#if (HUB_CAN_QUEUE_LEN & Q_MASK) != 0
#error "HUB_CAN_QUEUE_LEN must be a power of two"
#endif

static inline bool q_push(const can_evt_t *e)
{
    uint32_t head = s_q_head;
    uint32_t next = (head + 1u) & Q_MASK;

    if (next == s_q_tail)
    {
        return false;   /* full - caller counts the drop */
    }

    s_q[head] = *e;
    __DMB();            /* payload visible before the index moves */
    s_q_head = next;
    return true;
}

static inline bool q_pop(can_evt_t *out)
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

typedef struct {
    int16_t  ch[TLM_CH_PER_NODE];
    uint8_t  last_seq;
    uint8_t  status;
    uint16_t loss_accum;     /* frames missed since the last snapshot */
    uint32_t last_rx_ms;
    bool     ever_seen;
    bool     seen_this_window;
} node_state_t;

static CAN_HandleTypeDef  *s_can;
static UART_HandleTypeDef *s_debug;

static node_state_t s_nodes[TLM_NODE_COUNT];
static tlm_frame_t  s_frame;
static hub_stats_t  s_stats;

static uint32_t s_next_frame_ms;
static uint16_t s_frame_seq;

static uint8_t s_txbuf[TLM_FRAME_SIZE];

/* ------------------------------------------------------------------ */
/* CAN receive interrupt                                               */
/* ------------------------------------------------------------------ */
/*
 * This is the HAL weak callback. CubeMX must have CAN1 RX0 interrupt enabled
 * in the NVIC tab, otherwise it never fires and the hub sits there reporting
 * every node as offline - the single most common way to wire this up wrong.
 */
void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *hcan)
{
    CAN_RxHeaderTypeDef header;
    uint8_t             data[TLM_CAN_DLC];

    if (hcan->Instance != CAN1)
    {
        return;
    }

    /*
     * Drain the whole FIFO. It is three deep, and if we only took one frame
     * per interrupt we could fall behind under burst traffic.
     */
    while (HAL_CAN_GetRxFifoFillLevel(hcan, CAN_RX_FIFO0) > 0u)
    {
        if (HAL_CAN_GetRxMessage(hcan, CAN_RX_FIFO0, &header, data) != HAL_OK)
        {
            break;
        }

        if (header.IDE != CAN_ID_STD || !TLM_CAN_ID_IS_NODE(header.StdId))
        {
            s_stats.can_foreign++;
            continue;
        }

        can_evt_t evt;
        evt.node_id = TLM_CAN_NODE_FROM_ID(header.StdId);
        evt.t_ms    = HAL_GetTick();
        memcpy(evt.data, data, TLM_CAN_DLC);

        if (q_push(&evt))
        {
            s_stats.can_rx++;
        }
        else
        {
            s_stats.can_dropped++;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Aggregation                                                         */
/* ------------------------------------------------------------------ */

static void apply_event(const can_evt_t *e)
{
    node_state_t *n = &s_nodes[e->node_id];

    tlm_node_sample_t s;
    tlm_can_unpack(e->data, &s);

    /*
     * Sequence numbers are 8-bit and wrap. The unsigned subtraction below
     * gives the correct gap across the wrap point: if last_seq is 254 and we
     * receive 1, the delta is 3, meaning two frames went missing.
     */
    if (n->ever_seen)
    {
        uint8_t delta = (uint8_t)(s.seq - n->last_seq);

        if (delta > 1u)
        {
            n->loss_accum += (uint16_t)(delta - 1u);
        }
        /* delta == 0 means a duplicate or a node reset; not counted as loss. */
    }

    memcpy(n->ch, s.ch, sizeof(n->ch));
    n->last_seq         = s.seq;
    n->status           = s.status;
    n->last_rx_ms       = e->t_ms;
    n->ever_seen        = true;
    n->seen_this_window = true;
}

static void build_frame(uint32_t now)
{
    s_frame.seq  = s_frame_seq++;
    s_frame.t_ms = now;

    for (unsigned i = 0; i < TLM_NODE_COUNT; i++)
    {
        node_state_t      *n = &s_nodes[i];
        tlm_node_record_t *r = &s_frame.nodes[i];

        uint8_t flags = 0u;

        if (n->seen_this_window)
        {
            flags |= TLM_NF_ONLINE;
        }

        /*
         * "Stale" is deliberately separate from "not seen this window". At
         * 2 Hz snapshots and 10 Hz node transmissions, missing one window is
         * routine jitter; being quiet for TLM_NODE_TIMEOUT_MS means the node
         * is genuinely gone and the values on screen should not be trusted.
         */
        if (!n->ever_seen ||
            (uint32_t)(now - n->last_rx_ms) > TLM_NODE_TIMEOUT_MS)
        {
            flags |= TLM_NF_STALE;
            flags &= (uint8_t)~TLM_NF_ONLINE;
        }

        if (n->status & TLM_ST_SENSOR_FAULT)
        {
            flags |= TLM_NF_FAULT;
        }

        r->node_id = (uint8_t)i;
        r->flags   = flags;
        r->can_seq = n->last_seq;
        r->loss    = (n->loss_accum > 255u) ? 255u : (uint8_t)n->loss_accum;

        memcpy(r->ch, n->ch, sizeof(r->ch));

        n->loss_accum       = 0u;
        n->seen_this_window = false;
    }
}

#if HUB_DEBUG_UART
static void debug_print(const tlm_frame_t *f)
{
    if (s_debug == NULL)
    {
        return;
    }

    char line[160];

    int len = snprintf(line, sizeof(line),
                       "#%u t=%lu",
                       (unsigned)f->seq, (unsigned long)f->t_ms);

    for (unsigned i = 0; i < TLM_NODE_COUNT; i++)
    {
        const tlm_node_record_t *r = &f->nodes[i];

        len += snprintf(line + len, sizeof(line) - (size_t)len,
                        " | N%u%s %d,%d,%d",
                        i,
                        (r->flags & TLM_NF_STALE) ? "!" : "",
                        r->ch[0], r->ch[1], r->ch[2]);

        if ((size_t)len >= sizeof(line) - 24u)
        {
            break;
        }
    }

    len += snprintf(line + len, sizeof(line) - (size_t)len, "\r\n");

    /*
     * Blocking, but on the main loop rather than in an ISR, and only when the
     * debug build flag is on. 160 bytes at 115200 baud is ~14 ms out of a
     * 500 ms budget.
     */
    HAL_UART_Transmit(s_debug, (uint8_t *)line, (uint16_t)len, 50u);
}
#endif

static void check_bus_health(void)
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

void hub_init(CAN_HandleTypeDef *hcan,
              UART_HandleTypeDef *lora,
              UART_HandleTypeDef *debug)
{
    s_can   = hcan;
    s_debug = debug;

    memset(s_nodes, 0, sizeof(s_nodes));
    memset(&s_stats, 0, sizeof(s_stats));

    /*
     * Accept only 0x100..0x103 instead of the accept-everything mask the old
     * firmware used. Filtering in hardware means noise or unrelated traffic on
     * a shared bus never reaches the CPU at all.
     *
     * For a 32-bit mask filter on standard IDs the identifier sits in the top
     * 11 bits of the {FilterIdHigh, FilterIdLow} pair, so it is shifted left
     * by 5 within FilterIdHigh. Mask 0x7FC matches the upper 9 bits, letting
     * the low 2 bits vary - the hardware filter still passes IDs 0x100-0x103
     * (4 possible node numbers) even though only 0x100-0x102 (nodes 0-2) are
     * used now; TLM_CAN_ID_IS_NODE() in software is what actually enforces
     * TLM_NODE_COUNT, so a stray 0x103 frame would be counted as foreign
     * rather than accepted.
     */
    CAN_FilterTypeDef filter = {0};
    filter.FilterBank           = 0;
    filter.FilterMode           = CAN_FILTERMODE_IDMASK;
    filter.FilterScale          = CAN_FILTERSCALE_32BIT;
    filter.FilterIdHigh         = (uint16_t)(TLM_CAN_DATA_BASE_ID << 5);
    filter.FilterIdLow          = 0x0000u;
    filter.FilterMaskIdHigh     = (uint16_t)(0x7FCu << 5);
    filter.FilterMaskIdLow      = 0x0000u;   /* IDE/RTR bits left don't-care */
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

#if HUB_ENABLE_SD
    if (!sd_log_init())
    {
        s_stats.sd_errors++;
    }
#endif

    s_next_frame_ms = HAL_GetTick() + TLM_FRAME_PERIOD_MS;

#if HUB_DEBUG_UART
    if (s_debug != NULL)
    {
        const char *banner = "\r\n== telemetry hub ready ==\r\n";
        HAL_UART_Transmit(s_debug, (uint8_t *)banner,
                          (uint16_t)strlen(banner), 100u);
    }
#endif
}

void hub_task(void)
{
    /* 1. Drain everything the ISR queued. */
    can_evt_t evt;
    while (q_pop(&evt))
    {
        apply_event(&evt);
    }

    /* 2. Let the radio driver finish any pending work. */
    lora_e22_task();

    /* 3. On schedule, snapshot and dispatch. */
    uint32_t now = HAL_GetTick();

    if ((int32_t)(now - s_next_frame_ms) < 0)
    {
        return;
    }

    s_next_frame_ms += TLM_FRAME_PERIOD_MS;
    if ((int32_t)(now - s_next_frame_ms) > (int32_t)(TLM_FRAME_PERIOD_MS * 4u))
    {
        s_next_frame_ms = now + TLM_FRAME_PERIOD_MS;
    }

    check_bus_health();
    build_frame(now);

    size_t n = tlm_encode_frame(&s_frame, s_txbuf, sizeof(s_txbuf));
    if (n == 0u)
    {
        return;   /* cannot happen with a correctly sized buffer */
    }

    if (lora_e22_send(s_txbuf, n))
    {
        s_stats.frames_sent++;
    }
    else
    {
        s_stats.lora_busy++;
    }

#if HUB_ENABLE_SD
    /*
     * The SD log gets the identical bytes that went over the air, so a lost
     * radio link costs you nothing: replay the .tlm file through the same
     * decoder the live app uses and the result is bit-identical.
     */
    if (!sd_log_write(s_txbuf, n))
    {
        s_stats.sd_errors++;
    }
#endif

#if HUB_BINARY_UART
    /*
     * Mirror the EXACT bytes that just went to the radio and the SD card -
     * s_txbuf/n, not a second encode. That identity is the whole feature: the
     * PC side reuses the existing frame parser unchanged, and a mirror that
     * re-serialised independently could drift from the real wire format
     * without any test noticing.
     *
     * Non-blocking, and dropped rather than deferred, on purpose. A 34-byte
     * frame at 115200 baud is ~3 ms of blocking HAL_UART_Transmit. The frame
     * period is TLM_FRAME_PERIOD_MS and HUB_CAN_QUEUE_LEN is sized to absorb
     * SD stalls, not additional UART stalls - so spending milliseconds here
     * would eat into exactly the margin that keeps CAN frames from being
     * dropped. The mirror is a diagnostic convenience; CAN capture and SD
     * logging are the mission. If the VCP is still busy with the previous
     * frame (host not reading, or a slow terminal), skip this one and count
     * it. A gap in the mirror stream is harmless - the parser resyncs on the
     * next frame's sync bytes, and the SD log remains complete regardless.
     */
    if (s_debug != NULL)
    {
        if (HAL_UART_Transmit_DMA(s_debug, s_txbuf, (uint16_t)n) != HAL_OK)
        {
            s_stats.vcp_dropped++;
        }
    }
#endif

#if HUB_DEBUG_UART
    debug_print(&s_frame);
#endif
}

void hub_get_stats(hub_stats_t *out)
{
    *out = s_stats;
}

const tlm_frame_t *hub_last_frame(void)
{
    return &s_frame;
}
