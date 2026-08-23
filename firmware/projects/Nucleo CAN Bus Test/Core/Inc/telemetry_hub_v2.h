/**
 * telemetry_hub_v2.h - STM32L476RG hub, speaking telemetry_proto_v2 (TLM2_).
 *
 * NEW FILE, parallel to telemetry_hub.c/.h (v1). v1 is proven on real
 * hardware and is not touched by this file at all - see the repo-wide note
 * in protocol/telemetry_proto_v2.h. This is the v2 equivalent, and it is a
 * bigger job than v1's hub because in the agreed v2 topology the hub is not
 * just a CAN aggregator, it is also the ORIGIN of two of the four nodes:
 *
 *   node 0  "Hub - GPS/IMU"    - from sensors wired directly to this board
 *                                 (u-blox NEO-M8N GPS, BNO080 IMU)
 *   node 1  "Front"            - reassembled from a Bluepill's CAN bursts
 *   node 2  "Rear"             - reassembled from a Bluepill's CAN bursts
 *   node 3  "E-CVT / Motor"    - from the ODrive S1's own native CAN Simple
 *                                 traffic on the SAME bus, repackaged
 *
 * ---------------------------------------------------------------------
 * IMPORTANT - do not link this alongside telemetry_hub.c in one firmware
 * image. Both files define the HAL weak callback
 * HAL_CAN_RxFifo0MsgPendingCallback() as a strong symbol; linking both into
 * the same binary is a duplicate-symbol error. A CubeIDE project (or the
 * host test build) must compile exactly ONE of {telemetry_hub.c,
 * telemetry_hub_v2.c}, never both. Because you're not allowed to touch
 * firmware/projects/*, wiring this into an actual board target is left as
 * your job when you're ready to cut the "Nucleo CAN Bus Test" project over
 * to v2 - point main.c at hub2_init()/hub2_task() instead of hub_init()/
 * hub_task(), and drop telemetry_hub.c from the project's source list.
 * ---------------------------------------------------------------------
 *
 * CAN ID routing on one shared bus (500 kbit/s, standard 11-bit IDs):
 *
 *   0x200-0x21F   v2 node pages, nodes 0-3, pages 0-7 each (TLM2_CAN_ID_FOR)
 *   HUB2_ODRIVE_AXIS_NODE_ID<<5 .. +0x1F   ODrive S1 CAN Simple messages
 *   everything else                        foreign, counted and dropped
 *
 * The RX ISR classifies every frame into exactly one of those three buckets
 * BEFORE looking at the payload, using only the CAN ID - see the long
 * comment above hub2_rx_classify_and_queue() in the .c file for the
 * collision analysis (which ODrive axis_node_id values are safe, and which
 * ONE value silently aliases into the v2 node block).
 *
 * Torn-burst rejection for ALL FOUR nodes (not just the two arriving over
 * CAN) is delegated to protocol/telemetry_proto_v2.h's tlm2_reasm_* API.
 * Nodes 0 and 3 are locally "packed" from sensor readings into pages exactly
 * as if they'd been sent by a remote Bluepill, then fed through the SAME
 * tlm2_reasm_apply_page() reassembler nodes 1/2 use. That is a deliberate
 * design choice, not an accident: it means one code path (and one set of
 * ONLINE/STALE/FAULT/PARTIAL semantics, already fully tested in
 * tools/test_roundtrip_v2.c) governs staleness for every node, hub-
 * originated or not, instead of two different mechanisms that could drift
 * out of sync with each other. See "Staleness for hub-originated nodes"
 * below for how a GPS fix loss or an ODrive dropout surfaces honestly
 * through that same machinery.
 *
 * Usage in main.c (once you're ready to cut the Nucleo project to v2):
 *
 *   #include "telemetry_hub_v2.h"
 *
 *   MX_CAN1_Init(); MX_USART1_UART_Init(); MX_USART2_UART_Init(); MX_SPI1_Init();
 *   // TODO: MX_USART3_UART_Init() for the GPS, MX_I2C1_Init() for the IMU,
 *   // once HUB2_USE_REAL_GPS / HUB2_USE_REAL_IMU are turned on - see the
 *   // TODOs in telemetry_hub_v2.c next to hub2_originate_node0().
 *   hub2_init(&hcan1, &huart1, &huart2);
 *
 *   while (1) { hub2_task(); }
 */

#ifndef TELEMETRY_HUB_V2_H
#define TELEMETRY_HUB_V2_H

#include "main.h"
#include "telemetry_proto_v2.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Depth of the CAN receive ring buffer. Must be a power of two. Same
 * reasoning as v1's HUB_CAN_QUEUE_LEN: 10 CAN frames/cycle now (vs 3 in v1,
 * see V2_DESIGN_NOTES.md's page-count table) plus a stream of ODrive frames
 * that can arrive much faster than the telemetry cadence (the ODrive's own
 * CAN heartbeat/encoder-estimate messages are commonly configured well above
 * 10 Hz), so this is deliberately deeper than v1's 64.
 */
#define HUB2_CAN_QUEUE_LEN 128u

/** Set to 1 to mirror every frame as text on the ST-Link VCP (USART2). */
#ifndef HUB2_DEBUG_UART
#define HUB2_DEBUG_UART 1
#endif

/**
 * Set to 1 to mirror every frame on the ST-Link VCP (USART2) as the SAME
 * BINARY BYTES sent to the radio, instead of as human-readable text. See the
 * long note on HUB_BINARY_UART in telemetry_hub.h - identical rationale,
 * identical mutual-exclusion rule, and it matters more here: a v2 frame is
 * larger than a v1 one, so the blocking-transmit cost this avoids is bigger.
 *
 * MUTUALLY EXCLUSIVE with HUB2_DEBUG_UART, which it forces off.
 *
 * Defaulted ON (2026-08-22) because the wired full-rate link is now the
 * primary way the ground station is driven on the bench - pc_app's --vcp
 * expects it. The cost is that USART2 no longer carries the human-readable
 * frame dump: if you are debugging by watching a serial terminal rather than
 * by running the dashboard, set this to 0 and HUB2_DEBUG_UART goes back to 1
 * on its own.
 */
#ifndef HUB2_BINARY_UART
#define HUB2_BINARY_UART 1
#endif

#if HUB2_BINARY_UART && HUB2_DEBUG_UART
#undef HUB2_DEBUG_UART
#define HUB2_DEBUG_UART 0
#endif

/** Set to 0 to build without SD support (no FatFs dependency). */
#ifndef HUB2_ENABLE_SD
#define HUB2_ENABLE_SD 1
#endif

/*
 * Set to 0 when node 3 (E-CVT/Motor) is arriving over real CAN instead of
 * being fabricated locally - e.g. during the v2 bench test, 2026-08-14,
 * where a spare Bluepill is temporarily flashed with NODE_ID 3 (see
 * can_node_v2.h) to exercise the reassembler with a real 4th transmitter.
 * MUST be 0 whenever anything else on the bus is actually sending
 * TLM2_CAN_ID_FOR(3, *) pages - hub2_originate_node3() and a real
 * transmitter would both write into s_reasm[3], interleaving two epochs
 * into one torn burst. Leave at the default (1, hub-simulated) once that
 * stand-in board is removed and nothing real has replaced it yet - and
 * leave it at 1 even once the REAL ODrive is wired in (HUB2_USE_REAL_ODRIVE),
 * since the hub originates node 3 from ODrive-decoded values locally too,
 * the same as it does for node 0 - see hub2_originate_node3()'s TODO.
 *
 * MUST be overridden with a compiler command-line flag (-D HUB2_SIMULATE_NODE3=0
 * in platformio.ini's build_flags, or a Preprocessor "Defined symbol" in the
 * CubeIDE project's C/C++ Build settings), NOT a #define placed in main.c.
 * Found the hard way, 2026-08-16: main.c and telemetry_hub_v2.c are separate
 * translation units, each preprocessed independently - telemetry_hub_v2.c
 * does its own #include of this header, which never sees whatever main.c
 * defined before its own #include, so a main.c-local #define here is
 * silently a no-op for every #if HUB2_SIMULATE_NODE3 check in
 * telemetry_hub_v2.c (which is the only place this macro is actually
 * tested). A -D flag is visible to every translation unit, which is what
 * this needs.
 */
#ifndef HUB2_SIMULATE_NODE3
#define HUB2_SIMULATE_NODE3 1
#endif

/*
 * ---------------------------------------------------------------------
 * Simulator vs. real-sensor compile switches for the two hub-originated
 * nodes. Default OFF (simulated) so this file builds and runs - on the host
 * test AND on real L476RG hardware with nothing but a CAN transceiver wired
 * up - before any of the sensors in firmware/sensors/ are physically
 * attached. Flip each one independently once its sensor is wired; see the
 * TODOs beside hub2_originate_node0()/hub2_originate_node3() in the .c file
 * for the exact driver calls each flag switches in.
 *
 * NOTE on imu_bno080.h: as shipped, that header hard-codes
 * `#include "stm32f1xx_hal.h"` (a Bluepill HAL), which does not exist on this
 * L476RG project and will fail to compile here. That header is under
 * firmware/sensors/, which this task treats as read-only - it is a real bug,
 * reported here rather than silently patched. HUB2_USE_REAL_IMU therefore
 * cannot be turned on yet without first fixing that include (swap to
 * stm32l4xx_hal.h) in firmware/sensors/imu_bno080.h yourself.
 */
#ifndef HUB2_USE_REAL_GPS
#define HUB2_USE_REAL_GPS 0
#endif

#ifndef HUB2_USE_REAL_IMU
#define HUB2_USE_REAL_IMU 0
#endif

#ifndef HUB2_USE_REAL_ODRIVE
#define HUB2_USE_REAL_ODRIVE 0
#endif

/*
 * ODrive's configured CAN Simple axis_node_id. TODO: confirm against
 * whatever you actually set with odrivetool / the GUI and update this
 * default - see odrive_can.h's own TODO, this project does not own that
 * configuration.
 *
 * SAFE RANGE: 0-15 or 17-63 (assuming an 11-bit CAN ID, i.e. axis_node_id
 * <= 63). axis_node_id == 16 is the ONE value that aliases exactly onto the
 * v2 node-page block (0x200-0x21F is precisely axis 16's 32-ID range) and
 * would cause ODrive traffic to be silently decoded as node telemetry pages -
 * see the collision comment in telemetry_hub_v2.c. hub2_init() asserts
 * (via Error_Handler(), matching this project's existing pattern) if this
 * is left at 16.
 */
#ifndef HUB2_ODRIVE_AXIS_NODE_ID
#define HUB2_ODRIVE_AXIS_NODE_ID 0u
#endif

/*
 * How long without a fresh ODrive frame before node 3 stops originating new
 * bursts (and therefore ages into TLM2_NF_STALE via the normal reassembler
 * timeout, same as any CAN node going quiet). Deliberately shorter than
 * TLM2_NODE_TIMEOUT_MS so "ODrive stopped responding" is detected promptly
 * rather than only once the whole node has already gone stale.
 */
#define HUB2_ODRIVE_QUIET_MS 300u

/*
 * How long without a GPS fix before node 0's GPS-derived channels are
 * considered faulted (TLM2_ST_SENSOR_FAULT). IMU channels in the SAME node
 * keep updating independently - see the design note in
 * hub2_originate_node0() for why a GPS fix loss surfaces as FAULT rather
 * than the whole node going STALE, and the real limitation that trade-off
 * has (the v2 protocol has one status byte per node, not per channel).
 */
#define HUB2_GPS_FIX_GRACE_MS 2000u

/*
 * How long without a fresh IMU report before node 0 stops originating
 * bursts entirely (IMU dead, not just GPS unlucky) - this one DOES let node
 * 0 go STALE, because without the IMU there is nothing live left in that
 * node to report.
 */
#define HUB2_IMU_QUIET_MS 1000u

typedef struct {
    uint32_t can_rx;          /* v2 node-page CAN frames accepted            */
    uint32_t can_dropped;     /* frames lost because the ring buffer was full*/
    uint32_t can_foreign;     /* frames matching neither v2 node nor ODrive  */
    uint32_t can_odrive_rx;   /* ODrive CAN Simple frames accepted           */
    uint32_t frames_sent;     /* radio snapshots handed to the LoRa driver   */
    uint32_t lora_busy;       /* snapshots skipped, radio still transmitting */
    uint32_t sd_errors;       /* failed SD writes                            */
    uint32_t busoff_events;
    uint32_t vcp_dropped;     /* binary mirror frames skipped, VCP still busy */
} hub2_stats_t;

/**
 * @param hcan   CAN1, already initialised by MX_CAN1_Init()
 * @param lora   UART wired to the E22 module (DMA on TX required)
 * @param debug  UART for human-readable output, or NULL
 */
void hub2_init(CAN_HandleTypeDef *hcan,
               UART_HandleTypeDef *lora,
               UART_HandleTypeDef *debug);

/** Call continuously from the main loop. Never blocks for long. */
void hub2_task(void);

/** Snapshot of the running counters. */
void hub2_get_stats(hub2_stats_t *out);

/** Most recently built radio frame, for anything else that wants to see it. */
const tlm2_frame_t *hub2_last_frame(void);

#ifdef __cplusplus
}
#endif

#endif /* TELEMETRY_HUB_V2_H */
