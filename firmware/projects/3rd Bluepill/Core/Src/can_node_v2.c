/**
 * can_node_v2.c - Bluepill v2 CAN telemetry node (Front or Rear, by NODE_ID).
 *
 * v1's can_node.c sends one 8-byte frame per cycle. This node has 5 channels
 * across 10 bytes, so a cycle ("burst") is tlm2_node_page_count(NODE_ID)
 * consecutive 8-byte pages (2 for both Front and Rear today - see
 * V2_DESIGN_NOTES.md's page layout table). Everything sensor-related lives
 * in the "simulation" section below; replace sim_read_channels() with real
 * ADC/I2C reads per the TODOs in there and the rest of the file stays as it
 * is - the pack/transmit/epoch machinery does not care where the values
 * came from, same split v1 uses.
 *
 * ---- Mailbox contention, and why this differs from v1 --------------------
 *
 * v1 has one frame per cycle: if no TX mailbox is free, it drops that one
 * sample and moves on - the next sample is 100 ms away and carries fresher
 * data anyway, so losing one is cheap and instantly recoverable.
 *
 * A v2 burst is 2+ frames that only mean something as a SET - the hub's
 * reassembler throws the whole epoch away if even one page never arrives
 * (see tlm2_reasm_apply_page() in telemetry_proto_v2.c). Dropping page 1 of
 * 2 the way v1 would is therefore not "losing one sample", it is "losing the
 * whole burst AND wasting the mailbox time already spent on page 0" - worse
 * than not sending anything.
 *
 * So instead of dropping on the first stall, this file queues the burst's
 * pages and drains them across as many can_node_v2_task() calls as it takes
 * (flush_pending_pages(), called both right after a fresh burst is packed
 * and again at the top of every later task() call). The bxCAN peripheral has
 * 3 TX mailboxes and this burst is 2 frames, so in the overwhelmingly common
 * case both pages clear in the same call they were queued in - the retry
 * path only matters when something upstream (arbitration loss against
 * higher-priority traffic, e.g. the ODrive, or a genuinely wedged bus) is
 * holding mailboxes for longer than that.
 *
 * A burst only counts as truly torn (can_node_v2_torn_burst_count()) if it
 * STILL has not fully drained by the time the next burst is due a whole
 * TLM2_NODE_TX_PERIOD_MS (100 ms) later - at that point further delay would
 * mean sending stale-epoch pages interleaved with a new epoch, which cannot
 * complete either set, so the old pages are abandoned rather than queued
 * indefinitely (an unbounded backlog of stale telemetry is not something a
 * "latest value" stream should ever accumulate).
 */

#include "can_node_v2.h"
#include <string.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/* Module state                                                        */
/* ------------------------------------------------------------------ */

static CAN_HandleTypeDef *s_can;

/* One TX header per page, StdId fixed per page index at init time (the page
   index lives in the CAN ID, not the payload - see TLM2_CAN_ID_FOR()). */
static CAN_TxHeaderTypeDef s_tx_headers[TLM2_MAX_PAGES_PER_NODE];
static uint8_t             s_page_count;   /* fixed for this NODE_ID, computed once */

static uint32_t s_next_tx_ms;
static uint8_t  s_epoch;
static uint8_t  s_status = TLM2_ST_STARTUP;

/* Pages queued for the CURRENT (or most recently packed) burst, and how many
   of them have actually reached a TX mailbox so far. */
static uint8_t s_pending_pages[TLM2_MAX_PAGES_PER_NODE][TLM2_CAN_DLC];
static uint8_t s_pending_next;    /* index of the next unsent page */

static uint32_t s_page_tx_count;      /* pages actually handed to a mailbox */
static uint32_t s_burst_count;        /* burst attempts, one per TX period  */
static uint32_t s_torn_burst_count;   /* bursts that never fully drained    */
static uint32_t s_busoff_count;

/* How long TLM2_ST_STARTUP stays latched after reset. Same value as v1. */
#define STARTUP_HOLD_MS 2000u

/* Front, Rear, and the Motor/E-CVT bench stand-in (see can_node_v2.h's
   header comment) all have exactly 5 channels today (TLM2_CHAN_COUNT[NODE_ID]
   - see FRONT_CHANNELS / REAR_CHANNELS / MOTOR_CHANNELS in
   telemetry_proto_v2.c). sim_read_channels() below is hand-written for
   exactly this many; the check in can_node_v2_init() turns a future
   channel-table change into a boot-time Error_Handler() call instead of a
   burst silently packed with garbage in the channels this file never learned
   to fill. */
#define SIM_CHAN_COUNT 5u

/* ------------------------------------------------------------------ */
/* Simulation                                                          */
/* ------------------------------------------------------------------ */
/*
 * Deterministic, matches the waveforms simulation/vehicle_data.py produces
 * for the Front and Rear nodes exactly (same formulas, same clamps), so
 * bench output from real firmware is recognisable against the themed
 * dashboards under pc_app/static/themes without needing real sensors wired
 * up first. vehicle_data.py is the source of truth for the shapes; this is a
 * hand-port to single-precision C, not an independent design.
 *
 * Values are computed in engineering units, exactly like vehicle_data.py's
 * sample_frame(), then converted to the wire's raw integer using the same
 * scale/offset TLM2_CHANNELS carries - see sim_to_raw() below, which mirrors
 * v1's sim_to_raw() in can_node.c and the eng_to_raw_i16() example in
 * firmware/NODE_INTEGRATION_V2.md step 4.
 */

#define SIM_PI               3.14159265358979323846f
#define SIM_LAP_PERIOD_S     60.0f     /* one simulated lap - LAP_PERIOD_S in vehicle_data.py */
#define SIM_MPH_PER_G_PER_S  21.937f   /* MPH_PER_G_PER_S in vehicle_data.py */

static float clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static float maxf0(float v) { return v > 0.0f ? v : 0.0f; }

/* _speed_mph() in vehicle_data.py. `t` is lap-relative (t_elapsed mod
   SIM_LAP_PERIOD_S), same split vehicle_data.py's sample_frame() makes. */
static float sim_speed_mph(float t)
{
    float phase = 2.0f * SIM_PI * t / SIM_LAP_PERIOD_S - SIM_PI / 2.0f;
    return 22.0f + 20.0f * sinf(phase);
}

/* _lon_accel_g() in vehicle_data.py: d(speed)/dt converted to g. */
static float sim_lon_accel_g(float t)
{
    float phase          = 2.0f * SIM_PI * t / SIM_LAP_PERIOD_S - SIM_PI / 2.0f;
    float d_speed_mph_dt = 20.0f * (2.0f * SIM_PI / SIM_LAP_PERIOD_S) * cosf(phase);
    return d_speed_mph_dt / SIM_MPH_PER_G_PER_S;
}

/* _lat_accel_g() in vehicle_data.py. */
static float sim_lat_accel_g(float t)
{
    return 0.8f * sinf(2.0f * SIM_PI * t / SIM_LAP_PERIOD_S * 2.0f);
}

/* _bump() in vehicle_data.py - deterministic high-frequency texture standing
   in for track chatter. Driven off raw elapsed time, NOT lap-relative t,
   exactly like the Python version. */
static float sim_bump(float t_elapsed, float amp)
{
    return amp * (sinf(37.0f * t_elapsed) * 0.6f + sinf(91.3f * t_elapsed) * 0.4f);
}

/* _warmup() in vehicle_data.py - rises from start toward end with time
   constant tau seconds. Also driven off raw elapsed time. */
static float sim_warmup(float t_elapsed, float start, float end, float tau)
{
    return start + (end - start) * (1.0f - expf(-t_elapsed / tau));
}

/** Convert an engineering value to the raw int the wire format carries,
 *  clamped to this channel's width (not truncated - tlm2_can_pack_pages()
 *  deliberately does NOT do this for the caller, see the TODO on it in
 *  telemetry_proto_v2.h and firmware/NODE_INTEGRATION_V2.md step 4). Front
 *  and Rear only have TLM2_W_I16 channels today, but this stays width-aware
 *  so it does not quietly become wrong the day a channel table changes. */
static int32_t sim_to_raw(unsigned ch, float value)
{
    const tlm2_chan_def_t *d = &TLM2_CHANNELS[NODE_ID][ch];

    float raw = (value - d->offset) / d->scale;

    if (d->width == TLM2_W_I32)
    {
        /* Not exercised by Front/Rear today (both all-I16) - kept correct
           for when this file's channel table grows, same reasoning as the
           SIM_CHAN_COUNT check in can_node_v2_init(). float only has ~24
           bits of exact integer precision, well short of int32_t's range;
           the clamp bound is kept inside that precision on purpose so the
           comparison itself is exact rather than silently lossy. */
        if (raw >  8388608.0f) raw =  8388608.0f;
        if (raw < -8388608.0f) raw = -8388608.0f;
    }
    else
    {
        /* Saturate rather than wrap - a pegged channel is obvious on a plot;
           a wrapped one looks like real data going the wrong way. */
        if (raw >  32767.0f) raw =  32767.0f;
        if (raw < -32768.0f) raw = -32768.0f;
    }

    return (int32_t)(raw >= 0.0f ? raw + 0.5f : raw - 0.5f);
}

static void sim_read_channels(int32_t *raw)
{
    float t_elapsed = (float)HAL_GetTick() * 0.001f;   /* seconds since reset */
    float t         = fmodf(t_elapsed, SIM_LAP_PERIOD_S);
    if (t < 0.0f) t += SIM_LAP_PERIOD_S;   /* HAL_GetTick() never goes negative,
                                               but fmodf() can for a negative
                                               input - keep this defensive since
                                               t feeds straight into sinf/cosf. */

    float speed          = maxf0(sim_speed_mph(t));
    float lon_g          = sim_lon_accel_g(t);
    float lat_g          = sim_lat_accel_g(t);
    float corner_offset  = lat_g * 1.5f;   /* corner_offset in vehicle_data.py */
    float lean           = lat_g * 8.0f;   /* lean in vehicle_data.py */

#if NODE_ID == 3
    /* Motor/E-CVT bench stand-in (see can_node_v2.h's header comment) -
       mirrors hub2_originate_node3()'s formulas in telemetry_hub_v2.c
       exactly, channel-for-channel, so the numbers on the dashboard read the
       same whether node 3 came from the hub's own local simulation or from
       this stand-in board's real CAN frames - the only thing this bench
       test is supposed to prove is that the CAN/reassembly/radio/SD path
       carries a 4th real transmitter correctly, not that the values differ.
       [0] motor_current [1] motor_velocity [2] motor_temp [3] bus_voltage
       [4] brake_resistor_w - see MOTOR_CHANNELS in telemetry_proto_v2.c. */
    float motor_current    = clampf(5.0f + 40.0f * fabsf(lon_g), 0.0f, 60.0f);
    float motor_velocity   = clampf(speed * 130.0f, 0.0f, 6000.0f);
    float motor_temp       = sim_warmup(t_elapsed, 30.0f, 92.0f, 240.0f) + 0.3f * motor_current;
    float bus_voltage      = 55.0f - 4.0f * (motor_current / 60.0f) + sim_bump(t_elapsed, 0.15f);
    float brake_resistor_w = clampf(-lon_g * 45.0f, 0.0f, 50.0f);

    raw[0] = sim_to_raw(0, motor_current);
    raw[1] = sim_to_raw(1, motor_velocity);
    raw[2] = sim_to_raw(2, motor_temp);
    raw[3] = sim_to_raw(3, bus_voltage);
    raw[4] = sim_to_raw(4, brake_resistor_w);

#elif NODE_ID == 1
    /* Front - mirrors vehicle_data.py's front_vals channel-for-channel:
       [0] wheel_speed_fl [1] wheel_speed_fr [2] suspension_fl
       [3] suspension_fr [4] brake_pressure_f */

    /* TODO: replace with wheel_encoder_update(&enc_fl) / wheel_encoder_update(&enc_fr)
       (firmware/sensors/wheel_encoder.h) - one instance per wheel, each
       configured via wheel_encoder_init() with its own pin/tooth-count/
       circumference, and fed pulses from its own HAL_GPIO_EXTI_Callback(). */
    float wheel_fl = maxf0(speed + corner_offset);
    float wheel_fr = maxf0(speed - corner_offset);

    /* TODO: replace with suspension_pot_read(&sp_fl) / suspension_pot_read(&sp_fr)
       (firmware/sensors/suspension_pot.h) - one instance per corner, each
       calibrated by hand (volts_at_min_travel / volts_at_max_travel) before
       the reading means anything - see that header's comment. */
    float susp_fl = clampf(25.0f - lean + sim_bump(t_elapsed, 4.0f), 0.0f, 50.0f);
    float susp_fr = clampf(25.0f + lean + sim_bump(t_elapsed + 0.5f, 4.0f), 0.0f, 50.0f);

    /* TODO: replace with pressure_transducer_read(&pt_front)
       (firmware/sensors/pressure_transducer.h) - needs its voltage divider
       sized and calibrated first, see that header's TODOs. */
    float brake_f = fminf(1000.0f, maxf0(-lon_g * 600.0f) + 5.0f);

    raw[0] = sim_to_raw(0, wheel_fl);
    raw[1] = sim_to_raw(1, wheel_fr);
    raw[2] = sim_to_raw(2, susp_fl);
    raw[3] = sim_to_raw(3, susp_fr);
    raw[4] = sim_to_raw(4, brake_f);

#else /* NODE_ID == 2, Rear */
    /* Rear - mirrors vehicle_data.py's rear_vals channel-for-channel:
       [0] wheel_speed_rl [1] wheel_speed_rr [2] suspension_rl
       [3] suspension_rr [4] cvt_temp */

    /* TODO: replace with wheel_encoder_update(&enc_rl) / wheel_encoder_update(&enc_rr)
       (firmware/sensors/wheel_encoder.h), same pattern as the Front corners. */
    float wheel_rl = maxf0(speed + corner_offset * 0.7f);
    float wheel_rr = maxf0(speed - corner_offset * 0.7f);

    /* TODO: replace with suspension_pot_read(&sp_rl) / suspension_pot_read(&sp_rr)
       (firmware/sensors/suspension_pot.h). */
    float susp_rl = clampf(25.0f - lean * 0.8f + sim_bump(t_elapsed + 1.0f, 4.0f), 0.0f, 50.0f);
    float susp_rr = clampf(25.0f + lean * 0.8f + sim_bump(t_elapsed + 1.5f, 4.0f), 0.0f, 50.0f);

    /* TODO: replace with cvt_temp_read(&cvt, &degc) (firmware/sensors/cvt_temp_mlx90614.h).
       That call returns bool and leaves *out_degc UNTOUCHED on an I2C error -
       hold the last-known-good engineering value on a false return instead
       of substituting 0.0 degC, which would look like a real (very cold and
       very wrong) reading on the dashboard rather than a fault. */
    float cvt_temp = sim_warmup(t_elapsed, 35.0f, 108.0f, 180.0f) + sim_bump(t_elapsed, 1.5f);

    raw[0] = sim_to_raw(0, wheel_rl);
    raw[1] = sim_to_raw(1, wheel_rr);
    raw[2] = sim_to_raw(2, susp_rl);
    raw[3] = sim_to_raw(3, susp_rr);
    raw[4] = sim_to_raw(4, cvt_temp);
#endif
}

/* ------------------------------------------------------------------ */
/* Bus health                                                          */
/* ------------------------------------------------------------------ */
/* Identical reasoning to v1's check_bus_health() - see can_node.c. */

static void check_bus_health(void)
{
    uint32_t err = HAL_CAN_GetError(s_can);

    if (err & HAL_CAN_ERROR_BOF)
    {
        HAL_CAN_Stop(s_can);
        HAL_CAN_ResetError(s_can);
        HAL_CAN_Start(s_can);

        s_busoff_count++;
        s_status |= TLM2_ST_CAN_ERROR;
    }
    else if (err != HAL_CAN_ERROR_NONE)
    {
        HAL_CAN_ResetError(s_can);
        s_status |= TLM2_ST_CAN_ERROR;
    }
}

/* ------------------------------------------------------------------ */
/* Mailbox draining                                                    */
/* ------------------------------------------------------------------ */

/**
 * Send as many of the current burst's not-yet-sent pages as there are free
 * TX mailboxes right now, without blocking. Called both right after a fresh
 * burst is packed and, if that first pass could not clear it, again at the
 * top of every later can_node_v2_task() call until the burst is fully out -
 * see the file header comment for why retrying across calls (instead of
 * dropping on the first stall, the way v1's single-frame task does) is the
 * right call for a multi-page burst.
 */
static void flush_pending_pages(void)
{
    while (s_pending_next < s_page_count)
    {
        if (HAL_CAN_GetTxMailboxesFreeLevel(s_can) == 0u)
        {
            return;   /* try the rest again next call - don't block, don't drop yet */
        }

        uint32_t mailbox;
        HAL_StatusTypeDef st = HAL_CAN_AddTxMessage(s_can, &s_tx_headers[s_pending_next],
                                                     s_pending_pages[s_pending_next], &mailbox);
        if (st != HAL_OK)
        {
            return;   /* mailbox reported free but rejected us - try again next call */
        }

        s_page_tx_count++;
        s_pending_next++;
    }

    /* Whole burst is out. Mirrors v1's can_node_task(): report a CAN error
       once, on the burst it happened in, then clear it so the next burst's
       status byte starts clean instead of latching the flag forever. Safe to
       run every time the burst is (still) fully drained, not just the call
       that finished it. */
    s_status &= (uint8_t)~TLM2_ST_CAN_ERROR;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void can_node_v2_init(CAN_HandleTypeDef *hcan)
{
    s_can = hcan;

    if (TLM2_CHAN_COUNT[NODE_ID] != SIM_CHAN_COUNT)
    {
        /* See the comment on SIM_CHAN_COUNT above - sim_read_channels() only
           knows how to fill exactly this many raw[] slots. A silent mismatch
           here would mean packing a burst with uninitialised trailing
           channels instead of a board that refuses to boot. */
        Error_Handler();
    }

    /*
     * Transmit-only node, same reasoning as v1's can_node_init(): the F1 CAN
     * peripheral needs at least one filter bank configured to leave
     * initialisation mode cleanly, and an accept-nothing filter keeps RX
     * FIFO0 empty so nothing here ever has to service it.
     *
     * ID 0xFFFF with mask 0xFFFF matches nothing either protocol's node IDs
     * can produce (v1 tops out at 0x102, v2's node block tops out at 0x21F).
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

    s_page_count = tlm2_node_page_count(NODE_ID);

    for (uint8_t p = 0; p < s_page_count; p++)
    {
        s_tx_headers[p].StdId              = TLM2_CAN_ID_FOR(NODE_ID, p);
        s_tx_headers[p].ExtId              = 0;
        s_tx_headers[p].IDE                = CAN_ID_STD;
        s_tx_headers[p].RTR                = CAN_RTR_DATA;
        s_tx_headers[p].DLC                = TLM2_CAN_DLC;
        s_tx_headers[p].TransmitGlobalTime = DISABLE;
    }

    s_pending_next = s_page_count;   /* nothing pending - "already flushed" */

    s_next_tx_ms = HAL_GetTick() + TLM2_NODE_TX_PERIOD_MS;
}

void can_node_v2_task(void)
{
    uint32_t now = HAL_GetTick();

    /* Always try to clear out anything left from the previous burst first -
       this is what lets a page that stalled on a full mailbox actually go
       out a few task() calls later instead of being dropped outright. */
    flush_pending_pages();

    /* Signed comparison handles the 49-day tick rollover correctly. */
    if ((int32_t)(now - s_next_tx_ms) < 0)
    {
        return;
    }

    /*
     * Advance by a fixed period rather than "now + period" so the transmit
     * cadence does not drift. If we ever fall far behind (debugger halt, long
     * blocking call) resynchronise instead of trying to catch up with a burst.
     * Identical logic to v1's can_node_task().
     */
    s_next_tx_ms += TLM2_NODE_TX_PERIOD_MS;
    if ((int32_t)(now - s_next_tx_ms) > (int32_t)(TLM2_NODE_TX_PERIOD_MS * 4u))
    {
        s_next_tx_ms = now + TLM2_NODE_TX_PERIOD_MS;
    }

    check_bus_health();

    if (now > STARTUP_HOLD_MS)
    {
        s_status &= (uint8_t)~TLM2_ST_STARTUP;
    }

    s_burst_count++;

    /*
     * The previous burst's pages are STILL not all out despite the flush
     * above - mailboxes have been busy for a full TLM2_NODE_TX_PERIOD_MS,
     * which means something is genuinely wrong with the bus rather than
     * momentarily busy. Sending its leftover pages now, interleaved with the
     * new epoch about to start, would not complete a valid set for either
     * epoch - the hub would just see two different partial bursts instead of
     * one. Count it as torn and move on with fresh data; the alternative
     * (queueing bursts indefinitely) trades an unbounded backlog of stale
     * data for a "latest value" stream that is supposed to only ever show
     * the newest thing anyway.
     */
    if (s_pending_next < s_page_count)
    {
        s_torn_burst_count++;
    }

    int32_t raw[SIM_CHAN_COUNT];
    sim_read_channels(raw);

    uint8_t page_count = tlm2_can_pack_pages(NODE_ID, raw, (uint8_t)SIM_CHAN_COUNT,
                                              s_epoch, s_status, s_pending_pages);

    /* page_count is a pure function of NODE_ID's channel table (see
       layout_node() in telemetry_proto_v2.c), so it never actually changes
       call to call - this just keeps s_page_count (computed once, in init)
       honest against what the packer says today rather than trusting it
       silently forever. */
    if (page_count != s_page_count)
    {
        Error_Handler();
        return;
    }

    s_pending_next = 0;

    /*
     * epoch increments once per burst, after every page of THIS burst has
     * been queued for send - not once per page. See TLM2_CHANNELS' header
     * comment in telemetry_proto_v2.h and firmware/NODE_INTEGRATION_V2.md
     * step 5. Incrementing unconditionally here (whether or not the flush
     * below manages to clear the mailbox queue immediately) means a stalled
     * burst still shows up to the hub as a gap in epoch numbers, the same
     * way v1 always burns s_seq even when a frame gets dropped.
     */
    s_epoch++;

    /* Try to get this burst out immediately - the common case (mailboxes
       free) clears it in the same task() call it was built in. */
    flush_pending_pages();
}

uint32_t can_node_v2_page_tx_count(void)    { return s_page_tx_count; }
uint32_t can_node_v2_burst_count(void)      { return s_burst_count; }
uint32_t can_node_v2_torn_burst_count(void) { return s_torn_burst_count; }
uint32_t can_node_v2_busoff_count(void)     { return s_busoff_count; }
