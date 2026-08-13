/*
 * v2_node_test_stubs.c - HAL callback stubs for the v2 node host test.
 *
 * tools/hal_shim/hal_shim.c calls back into firmware for two events that a
 * transmit-only CAN node has no reason to implement:
 *
 *   HAL_CAN_RxFifo0MsgPendingCallback - the hub's job (telemetry_hub*.c)
 *   HAL_UART_TxCpltCallback           - the radio driver's job (lora_e22.c)
 *
 * v1's test (tools/test_firmware.py) never needed these stubs because it
 * links can_node.c, telemetry_hub.c and lora_e22.c together, so the hub and
 * radio supply their own. The v2 node test links can_node_v2.c on its own -
 * deliberately, so a node-level failure can't be masked by hub behavior - and
 * therefore has to fill the gap itself.
 *
 * Delete this once telemetry_hub_v2.c exists and the v2 tests link the whole
 * v2 firmware set the way v1's do.
 */

#include "main.h"

void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *hcan)
{
    (void)hcan;
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    (void)huart;
}
