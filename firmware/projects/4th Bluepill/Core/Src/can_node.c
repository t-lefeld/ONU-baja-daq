/**
 * can_node.c - Bluepill CAN telemetry node.
 *
 * Transmits one 8-byte NODE_DATA frame every TLM_NODE_TX_PERIOD_MS on
 * ID 0x100 + NODE_ID. Everything sensor-related lives in the "simulation"
 * section at the top; replace sim_read_channels() with real ADC/I2C reads and
 * the rest of the file stays as it is.
 */

#include "can_node.h"
#include <string.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/* Module state                                                        */
/* ------------------------------------------------------------------ */

static CAN_HandleTypeDef *s_can;

static CAN_TxHeaderTypeDef s_tx_header;
static uint32_t            s_next_tx_ms;
static uint8_t             s_seq;
static uint8_t             s_status = TLM_ST_STARTUP;

static uint32_t s_tx_count;
static uint32_t s_drop_count;
static uint32_t s_busoff_count;

/* How long TLM_ST_STARTUP stays latched after reset. */
#define STARTUP_HOLD_MS 2000u

/* ------------------------------------------------------------------ */
/* Simulation                                                          */
/* ------------------------------------------------------------------ */
/*
 * Each node produces a different, immediately recognisable shape so you can
 * tell at a glance on the dashboard which trace belongs to which board and
 * whether the link is actually live:
 *
 *   node 0  environmental - slow sines, tens of seconds per cycle
 *   node 1  power         - sawtooth load with a matching voltage sag
 *   node 2  motion        - fast sines in quadrature, gravity on Z
 *   node 3  drivetrain    - engine-like ramp, throttle steps, lagging coolant
 *
 * Values are computed in engineering units and then converted to the int16 the
 * wire format carries, using the same scale factors the PC will use to convert
 * them back. That round trip is the thing most worth getting right, so it is
 * exercised here rather than hidden behind hand-tuned constants.
 */

/* Deterministic small PRNG - avoids pulling in rand() and is repeatable. */
static uint32_t s_rng = 0x1234567u ^ (NODE_ID * 0x9E3779B9u);

static float sim_noise(float amplitude)
{
    s_rng = s_rng * 1664525u + 1013904223u;
    /* top 16 bits, mapped to -1.0 .. +1.0 */
    float u = (float)((s_rng >> 16) & 0xFFFFu) / 32767.5f - 1.0f;
    return u * amplitude;
}

/** Convert an engineering value to the raw int16 the wire format carries. */
static int16_t sim_to_raw(unsigned ch, float value)
{
    const tlm_chan_def_t *d = &TLM_CHANNELS[NODE_ID][ch];

    float raw = (value - d->offset) / d->scale;

    /* Saturate rather than wrap. A pegged channel is obvious on a plot; a
       wrapped one looks like real data going the wrong way. */
    if (raw >  32767.0f) raw =  32767.0f;
    if (raw < -32768.0f) raw = -32768.0f;

    return (int16_t)(raw >= 0.0f ? raw + 0.5f : raw - 0.5f);
}

static void sim_read_channels(int16_t *out)
{
    float t = (float)HAL_GetTick() * 0.001f;   /* seconds since reset */

#if NODE_ID == 0
    /* Environmental: slow, smooth, small noise. */
    float temp     = 22.5f + 7.0f  * sinf(t * 0.10f) + sim_noise(0.15f);
    float humidity = 50.0f + 18.0f * sinf(t * 0.07f + 1.2f) + sim_noise(0.4f);
    float pressure = 100.5f + 1.2f * sinf(t * 0.04f) + sim_noise(0.05f);

    out[0] = sim_to_raw(0, temp);
    out[1] = sim_to_raw(1, humidity);
    out[2] = sim_to_raw(2, pressure);

#elif NODE_ID == 1
    /* Power: a sawtooth current draw with voltage sagging under load. */
    float phase   = fmodf(t, 12.0f) / 12.0f;          /* 0 .. 1 ramp */
    float current = 0.5f + 7.5f * phase + sim_noise(0.05f);
    float volts   = 12.9f - 0.11f * current + sim_noise(0.01f);
    float watts   = volts * current;

    out[0] = sim_to_raw(0, volts);
    out[1] = sim_to_raw(1, current);
    out[2] = sim_to_raw(2, watts);

#elif NODE_ID == 2
    /* Motion: X and Y in quadrature, Z sitting on 1 g like a real IMU. */
    float ax = 0.45f * sinf(t * 1.7f) + sim_noise(0.01f);
    float ay = 0.45f * cosf(t * 1.7f) + sim_noise(0.01f);
    float az = 1.00f + 0.08f * sinf(t * 3.1f) + sim_noise(0.01f);

    out[0] = sim_to_raw(0, ax);
    out[1] = sim_to_raw(1, ay);
    out[2] = sim_to_raw(2, az);

#else /* NODE_ID == 3 */
    /* Drivetrain: throttle steps, rpm chasing it, coolant lagging both. */
    float cycle    = fmodf(t, 20.0f);
    float throttle = (cycle < 8.0f)  ? 15.0f
                   : (cycle < 14.0f) ? 85.0f
                                     : 40.0f;
    float rpm      = 800.0f + throttle * 55.0f + sim_noise(60.0f);
    float coolant  = 78.0f + 12.0f * sinf(t * 0.05f) + sim_noise(0.2f);

    out[0] = sim_to_raw(0, rpm);
    out[1] = sim_to_raw(1, throttle + sim_noise(0.3f));
    out[2] = sim_to_raw(2, coolant);
#endif
}

/* ------------------------------------------------------------------ */
/* Bus health                                                          */
/* ------------------------------------------------------------------ */
/*
 * With AutoBusOff enabled in CubeMX the controller re-joins the bus by itself
 * after 128 consecutive recessive sequences. The stock projects had it
 * DISABLED, which means one wiring fault leaves the node permanently mute with
 * no outward sign. Enable it in the .ioc; this function is the belt-and-braces
 * fallback for the case where it is still off.
 */
static void check_bus_health(void)
{
    uint32_t err = HAL_CAN_GetError(s_can);

    if (err & HAL_CAN_ERROR_BOF)
    {
        HAL_CAN_Stop(s_can);
        HAL_CAN_ResetError(s_can);
        HAL_CAN_Start(s_can);

        s_busoff_count++;
        s_status |= TLM_ST_CAN_ERROR;
    }
    else if (err != HAL_CAN_ERROR_NONE)
    {
        HAL_CAN_ResetError(s_can);
        s_status |= TLM_ST_CAN_ERROR;
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void can_node_init(CAN_HandleTypeDef *hcan)
{
    s_can = hcan;

    /*
     * A transmit-only node still needs at least one filter bank configured.
     * The F1 CAN peripheral will not leave initialisation mode cleanly without
     * one, and an accept-nothing filter keeps the RX FIFO empty so we never
     * have to service it.
     *
     * ID 0xFFFF with mask 0xFFFF matches nothing that our IDs can produce.
     */
    CAN_FilterTypeDef filter = {0};
    filter.FilterBank           = 0;
    filter.FilterMode           = CAN_FILTERMODE_IDMASK;
    filter.FilterScale          = CAN_FILTERSCALE_32BIT;
    filter.FilterIdHigh         = 0xFFFFu;
    filter.FilterIdLow          = 0xFFFFu;
    filter.FilterMaskIdHigh     = 0xFFFFu;
    filter.FilterMaskIdLow      = 0xFFFFu;
    filter.FilterFIFOAssignment = CAN_RX_FIFO0;
    filter.FilterActivation     = ENABLE;

    if (HAL_CAN_ConfigFilter(s_can, &filter) != HAL_OK)
    {
        Error_Handler();
    }

    if (HAL_CAN_Start(s_can) != HAL_OK)
    {
        Error_Handler();
    }

    s_tx_header.StdId              = TLM_CAN_ID_FOR_NODE(NODE_ID);
    s_tx_header.ExtId              = 0;
    s_tx_header.IDE                = CAN_ID_STD;
    s_tx_header.RTR                = CAN_RTR_DATA;
    s_tx_header.DLC                = TLM_CAN_DLC;
    s_tx_header.TransmitGlobalTime = DISABLE;

    s_next_tx_ms = HAL_GetTick() + TLM_NODE_TX_PERIOD_MS;
}

void can_node_task(void)
{
    uint32_t now = HAL_GetTick();

    /* Signed comparison handles the 49-day tick rollover correctly. */
    if ((int32_t)(now - s_next_tx_ms) < 0)
    {
        return;
    }

    /*
     * Advance by a fixed period rather than "now + period" so the transmit
     * cadence does not drift. If we ever fall far behind (debugger halt, long
     * blocking call) resynchronise instead of trying to catch up with a burst.
     */
    s_next_tx_ms += TLM_NODE_TX_PERIOD_MS;
    if ((int32_t)(now - s_next_tx_ms) > (int32_t)(TLM_NODE_TX_PERIOD_MS * 4u))
    {
        s_next_tx_ms = now + TLM_NODE_TX_PERIOD_MS;
    }

    check_bus_health();

    if (now > STARTUP_HOLD_MS)
    {
        s_status &= (uint8_t)~TLM_ST_STARTUP;
    }

    tlm_node_sample_t sample;
    sim_read_channels(sample.ch);
    sample.seq    = s_seq;
    sample.status = s_status;

    uint8_t data[TLM_CAN_DLC];
    tlm_can_pack(&sample, data);

    /*
     * Never block waiting for a mailbox. Three mailboxes at 500 kbit/s drain in
     * well under a millisecond; if all three are full something is wrong with
     * the bus and stalling here would only make the node less responsive.
     * Dropping the sample is the right call for periodic telemetry - the next
     * one is 100 ms away and carries fresher data anyway.
     */
    if (HAL_CAN_GetTxMailboxesFreeLevel(s_can) == 0u)
    {
        s_drop_count++;
        s_seq++;              /* still burn the sequence number, so the hub */
        return;               /* sees the gap and reports it as loss        */
    }

    uint32_t mailbox;
    if (HAL_CAN_AddTxMessage(s_can, &s_tx_header, data, &mailbox) == HAL_OK)
    {
        s_tx_count++;
        /* Errors reported cleanly once, then cleared for the next window. */
        s_status &= (uint8_t)~TLM_ST_CAN_ERROR;
    }
    else
    {
        s_drop_count++;
    }

    s_seq++;
}

uint32_t can_node_tx_count(void)     { return s_tx_count; }
uint32_t can_node_drop_count(void)   { return s_drop_count; }
uint32_t can_node_busoff_count(void) { return s_busoff_count; }
