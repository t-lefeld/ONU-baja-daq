/**
 * telemetry_hub.h - STM32L476RG main node.
 *
 * Receives CAN from the three Bluepills, keeps a latest-value table, and every
 * TLM_FRAME_PERIOD_MS emits one snapshot frame to:
 *
 *   - the LoRa module over UART (DMA)
 *   - the SD card (same bytes, appended to a .tlm file)
 *   - optionally the ST-Link virtual COM port, for bench debugging
 *
 * Design note. The previous firmware did a blocking HAL_UART_Transmit of a
 * 64-character string from inside HAL_CAN_RxFifo0MsgPendingCallback. At
 * 115200 baud that is roughly 5.5 ms spent in interrupt context per frame.
 * It survived at 20 frames/second, but adding SD writes (which can block for
 * tens of milliseconds during a flash erase) on top of that would have caused
 * dropped CAN frames that were essentially impossible to diagnose.
 *
 * Here the ISR does nothing but copy the frame into a lock-free ring buffer
 * and return - a few hundred nanoseconds. All parsing, radio and filesystem
 * work happens in the main loop where it can safely take as long as it needs.
 *
 * Usage in main.c:
 *
 *   #include "telemetry_hub.h"
 *
 *   MX_CAN1_Init(); MX_USART1_UART_Init(); MX_USART2_UART_Init(); MX_SPI1_Init();
 *   hub_init(&hcan1, &huart1, &huart2);
 *
 *   while (1) { hub_task(); }
 */

#ifndef TELEMETRY_HUB_H
#define TELEMETRY_HUB_H

#include "main.h"
#include "telemetry_proto.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Depth of the CAN receive ring buffer. Must be a power of two.
 *
 * 64 entries at 30 frames/second is a bit over 2 seconds of buffering, which
 * is far more than the main loop should ever need. The margin exists to absorb SD
 * card write stalls: a cheap card doing an internal garbage collection pass
 * can block for 100 ms or more, and that must not cost us CAN frames.
 */
#define HUB_CAN_QUEUE_LEN 64u

/** Set to 1 to mirror every frame as text on the ST-Link VCP (USART2). */
#ifndef HUB_DEBUG_UART
#define HUB_DEBUG_UART 1
#endif

/**
 * Set to 1 to mirror every frame on the ST-Link VCP (USART2) as the SAME
 * BINARY BYTES sent to the radio, instead of as human-readable text.
 *
 * Why: LoRa is deliberately a low-bandwidth summary link (see the project
 * notes on why SD logging and telemetry are separate paths). The ST-Link USB
 * cable is already plugged in for flashing, so it is a free wired path that
 * can carry the full frame rate with no radio in the way. Because the bytes
 * are identical to the radio's, the ground station needs no new parser -
 * pc_app's VcpSource is just SerialSource pointed at a different port.
 *
 * MUTUALLY EXCLUSIVE with HUB_DEBUG_UART: one UART cannot carry human text
 * and binary frames at once without the text corrupting every frame a parser
 * tries to sync on. Enabling this therefore forces HUB_DEBUG_UART off rather
 * than letting the two silently interleave.
 */
#ifndef HUB_BINARY_UART
#define HUB_BINARY_UART 0
#endif

#if HUB_BINARY_UART && HUB_DEBUG_UART
#undef HUB_DEBUG_UART
#define HUB_DEBUG_UART 0
#endif

/** Set to 0 to build without SD support (no FatFs dependency). */
#ifndef HUB_ENABLE_SD
#define HUB_ENABLE_SD 1
#endif

typedef struct {
    uint32_t can_rx;         /* CAN frames accepted                        */
    uint32_t can_dropped;    /* frames lost because the ring buffer was full*/
    uint32_t can_foreign;    /* frames with IDs outside our node range      */
    uint32_t frames_sent;    /* snapshots handed to the radio               */
    uint32_t lora_busy;      /* snapshots skipped, radio still transmitting */
    uint32_t sd_errors;      /* failed writes                               */
    uint32_t busoff_events;
    uint32_t vcp_dropped;    /* binary mirror frames skipped, VCP still busy */
} hub_stats_t;

/**
 * @param hcan   CAN1, already initialised by MX_CAN1_Init()
 * @param lora   UART wired to the E22 module (DMA on TX required)
 * @param debug  UART for human-readable output, or NULL
 */
void hub_init(CAN_HandleTypeDef *hcan,
              UART_HandleTypeDef *lora,
              UART_HandleTypeDef *debug);

/** Call continuously from the main loop. Never blocks for long. */
void hub_task(void);

/** Snapshot of the running counters. */
void hub_get_stats(hub_stats_t *out);

/** Most recently built frame, for anything else that wants to see it. */
const tlm_frame_t *hub_last_frame(void);

#ifdef __cplusplus
}
#endif

#endif /* TELEMETRY_HUB_H */
