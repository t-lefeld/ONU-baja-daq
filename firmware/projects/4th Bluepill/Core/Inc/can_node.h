/**
 * can_node.h - Bluepill (STM32F103C8) CAN telemetry node.
 *
 * Drop-in replacement for the old CAN_Sender_Init / CAN_Sender_Loop pair.
 * The old version called HAL_Delay(200) inside the main loop, which meant the
 * CPU spent 99.9% of its life blocked and could not service anything else.
 * This version is non-blocking and driven off HAL_GetTick().
 *
 * Usage in main.c:
 *
 *   #include "can_node.h"                  // in USER CODE BEGIN Includes
 *
 *   MX_CAN_Init();
 *   can_node_init(&hcan);                  // in USER CODE BEGIN 2
 *
 *   while (1) {
 *       can_node_task();                   // in USER CODE BEGIN 3
 *   }
 *
 * Set the node identity with a preprocessor symbol so all four boards build
 * from identical source:
 *
 *   Project > Properties > C/C++ Build > Settings > MCU GCC Compiler >
 *   Preprocessor > Defined symbols:   NODE_ID=0     (1, 2, 3 on the others)
 */

#ifndef CAN_NODE_H
#define CAN_NODE_H

#include "main.h"
#include "telemetry_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Which node this board is.
 *
 * Defined in each project's Core/Inc/node_id.h rather than as a -D build
 * symbol. Both work, but a header is visible when you open the project, it
 * survives CubeMX regeneration, and it does not silently revert if the build
 * configuration is ever recreated. Flashing two boards with the same ID is the
 * failure mode worth engineering against: they arbitrate happily and you get
 * one node on the dashboard with a sequence counter that jumps around.
 */
#include "node_id.h"

#ifndef NODE_ID
#error "node_id.h must define NODE_ID (0-3). See firmware/bluepill_node/INTEGRATION.md"
#endif

#if (NODE_ID < 0) || (NODE_ID >= TLM_NODE_COUNT)
#error "NODE_ID must be in the range 0 .. TLM_NODE_COUNT-1"
#endif

/** Configure filters, start the peripheral, prime the TX header. */
void can_node_init(CAN_HandleTypeDef *hcan);

/** Call as fast as you like from the main loop. Transmits on schedule. */
void can_node_task(void);

/** Total frames handed to a TX mailbox since reset. */
uint32_t can_node_tx_count(void);

/** Times a transmission was abandoned because no mailbox freed up. */
uint32_t can_node_drop_count(void);

/** Times the node recovered from bus-off. */
uint32_t can_node_busoff_count(void);

#ifdef __cplusplus
}
#endif

#endif /* CAN_NODE_H */
