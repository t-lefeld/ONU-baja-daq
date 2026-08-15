/**
 * can_node_v2.h - Bluepill (STM32F103C8) v2 CAN telemetry node - Front/Rear.
 *
 * v2's multi-page counterpart to can_node.h/.c. A NEW file, parallel to v1 -
 * can_node.c/.h are untouched and stay exactly as flashed on hardware today.
 * See the header of can_node_v2.c for the page/epoch/mailbox design.
 *
 * Usage in main.c, once you are ready to build a v2 project instead of a v1
 * one (see firmware/NODE_INTEGRATION_V2.md for the full walk-through of
 * wiring a real sensor in afterward):
 *
 *   #include "can_node_v2.h"                // in USER CODE BEGIN Includes
 *
 *   MX_CAN_Init();
 *   can_node_v2_init(&hcan);                // in USER CODE BEGIN 2
 *
 *   while (1) {
 *       can_node_v2_task();                 // in USER CODE BEGIN 3
 *   }
 *
 * Node identity uses the exact same mechanism v1's can_node.h documents:
 * Core/Inc/node_id.h, NOT a -D build symbol - visible when the project is
 * open, survives CubeMX regeneration, does not silently revert if a build
 * configuration is ever recreated. can_node_v2.h #errors if it is missing,
 * same as v1.
 *
 * Node roles for THIS file: 1 (Front) and 2 (Rear) are the real, permanent
 * roles - see the "Board mapping" in firmware/NODE_INTEGRATION_V2.md's Open
 * Questions section. Node 0 (Hub) is the Nucleo, not a Bluepill, and reads
 * its GPS/IMU locally.
 *
 * Node 3 (E-CVT/Motor) is a special case, added 2026-08-14: in the FINAL
 * system it has no physical Bluepill at all - the hub reads it straight off
 * the ODrive's own native CAN Simple traffic (see odrive_can.h) and
 * originates that node's record itself, the same way it does for node 0.
 * NODE_ID 3 is allowed to build here anyway, ONLY as a temporary bench
 * stand-in: for the v2 hardware bring-up, before the ODrive is on the bench,
 * flashing a spare Bluepill with NODE_ID 3 makes it transmit real
 * tlm2_can_pack_pages() bursts on the 0x218-0x21F block, exercising the
 * hub's reassembler for all four nodes over real CAN instead of leaving node
 * 3 entirely hub-internal during the test. This is NOT what node 3 looks
 * like in the final system - the real ODrive does not speak this protocol at
 * all, it speaks CAN Simple, addressed by axis_node_id, not
 * TLM2_CAN_ID_FOR(3, page). Swapping in the real ODrive later means
 * unflashing/removing this stand-in board entirely, not "upgrading" it - see
 * HUB2_SIMULATE_NODE3 in telemetry_hub_v2.h for the hub-side half of this
 * switch (it must be OFF while this stand-in is transmitting, or the hub's
 * own local origination and this board's real CAN frames fight over the same
 * reassembler slot).
 */

#ifndef CAN_NODE_V2_H
#define CAN_NODE_V2_H

#include "main.h"
#include "telemetry_proto_v2.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Same node_id.h mechanism as v1 - see can_node.h's header comment for the
   full reasoning. This is deliberately the SAME node_id.h file v1 reads (one
   file per physical board), not a separate v2-specific one: a board is one
   NODE_ID no matter which protocol version its firmware happens to speak. */
#include "node_id.h"

#ifndef NODE_ID
#error "node_id.h must define NODE_ID (1, 2, or the 3-as-bench-stand-in case - see the file header above) for this v2 Bluepill firmware. See firmware/NODE_INTEGRATION_V2.md"
#endif

#if (NODE_ID != 1) && (NODE_ID != 2) && (NODE_ID != 3)
#error "can_node_v2 only targets Front (1), Rear (2), or a temporary Motor/E-CVT bench stand-in (3). Node 0 (Hub) is the Nucleo hub - see the file header above for why 3 is a stand-in only, not the final Motor node."
#endif

/** Configure filters, start the peripheral, prime the per-page TX headers. */
void can_node_v2_init(CAN_HandleTypeDef *hcan);

/** Call as fast as you like from the main loop. Transmits a full burst of
 *  this node's pages on schedule, and also drains any pages left over from a
 *  burst that could not fully clear its TX mailboxes last time it ran - see
 *  can_node_v2.c for why that retry matters more here than it does in v1. */
void can_node_v2_task(void);

/** Total CAN pages (frames) handed to a TX mailbox since reset. Two per
 *  burst for both Front and Rear today - see tlm2_node_page_count(). */
uint32_t can_node_v2_page_tx_count(void);

/** Total burst attempts (one per TLM2_NODE_TX_PERIOD_MS) since reset. */
uint32_t can_node_v2_burst_count(void);

/** Bursts where at least one page never reached a mailbox before the next
 *  burst was due - i.e. a torn set from the hub's point of view. */
uint32_t can_node_v2_torn_burst_count(void);

/** Times the node recovered from bus-off. */
uint32_t can_node_v2_busoff_count(void);

#ifdef __cplusplus
}
#endif

#endif /* CAN_NODE_V2_H */
