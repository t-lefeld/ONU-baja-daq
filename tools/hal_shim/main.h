/**
 * main.h - Host-side stand-in for the CubeMX HAL.
 *
 * Lets the real firmware .c files be compiled and executed on a PC so the
 * logic can be tested without hardware: CAN aggregation, sequence-loss
 * arithmetic, node timeouts, transmit scheduling and frame construction are
 * all target-independent, and they are where the bugs live.
 *
 * This deliberately implements only what the firmware actually calls. It is
 * not a HAL emulator, and it makes no attempt to model peripheral timing.
 * What it gives you is the ability to drive HAL_GetTick() by hand and inject
 * CAN frames, which is exactly what a scheduling and aggregation test needs.
 */

#ifndef SHIM_MAIN_H
#define SHIM_MAIN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ---------------- basic HAL types ---------------- */

typedef enum { HAL_OK = 0, HAL_ERROR, HAL_BUSY, HAL_TIMEOUT } HAL_StatusTypeDef;
typedef enum { DISABLE = 0, ENABLE = 1 } FunctionalState;
typedef enum { GPIO_PIN_RESET = 0, GPIO_PIN_SET = 1 } GPIO_PinState;

#define __DMB() ((void)0)
#define __disable_irq() ((void)0)

/* ---------------- CAN ---------------- */

#define CAN_ID_STD    0x00000000u
#define CAN_ID_EXT    0x00000004u
#define CAN_RTR_DATA  0x00000000u
#define CAN_RX_FIFO0  0x00000000u

#define CAN_FILTERMODE_IDMASK 0x00000000u
#define CAN_FILTERSCALE_32BIT 0x00000001u
#define CAN_IT_RX_FIFO0_MSG_PENDING 0x00000002u

#define CAN_MODE_NORMAL 0x00000000u
#define CAN_SJW_1TQ     0x00000000u
#define CAN_BS1_6TQ     0x00050000u
#define CAN_BS1_13TQ    0x000C0000u
#define CAN_BS2_1TQ     0x00000000u
#define CAN_BS2_2TQ     0x00100000u

#define HAL_CAN_ERROR_NONE 0x00000000u
#define HAL_CAN_ERROR_BOF  0x00000400u

typedef struct { uint32_t dummy; } CAN_TypeDef;
extern CAN_TypeDef *const CAN1;

typedef struct {
    uint32_t Prescaler, Mode, SyncJumpWidth, TimeSeg1, TimeSeg2;
    FunctionalState TimeTriggeredMode, AutoBusOff, AutoWakeUp,
                    AutoRetransmission, ReceiveFifoLocked, TransmitFifoPriority;
} CAN_InitTypeDef;

typedef struct {
    CAN_TypeDef *Instance;
    CAN_InitTypeDef Init;
    uint32_t ErrorCode;
} CAN_HandleTypeDef;

typedef struct {
    uint32_t StdId, ExtId, IDE, RTR, DLC;
    FunctionalState TransmitGlobalTime;
} CAN_TxHeaderTypeDef;

typedef struct {
    uint32_t StdId, ExtId, IDE, RTR, DLC, Timestamp, FilterMatchIndex;
} CAN_RxHeaderTypeDef;

typedef struct {
    uint32_t FilterIdHigh, FilterIdLow, FilterMaskIdHigh, FilterMaskIdLow;
    uint32_t FilterFIFOAssignment, FilterBank, FilterMode, FilterScale;
    uint32_t FilterActivation, SlaveStartFilterBank;
} CAN_FilterTypeDef;

/* ---------------- UART ---------------- */

typedef enum { HAL_UART_STATE_READY = 0x20u, HAL_UART_STATE_BUSY_TX = 0x21u } HAL_UART_StateTypeDef;

typedef struct { uint32_t dummy; } USART_TypeDef;

typedef struct {
    USART_TypeDef *Instance;
    HAL_UART_StateTypeDef gState;
} UART_HandleTypeDef;

#define __HAL_UART_CLEAR_OREFLAG(h) ((void)(h))

/* ---------------- GPIO ---------------- */

typedef struct { uint32_t idx; } GPIO_TypeDef;
extern GPIO_TypeDef *const GPIOA;
extern GPIO_TypeDef *const GPIOB;

#define GPIO_PIN_0  (1u << 0)
#define GPIO_PIN_1  (1u << 1)
#define GPIO_PIN_2  (1u << 2)
#define GPIO_PIN_4  (1u << 4)

/* ---------------- API used by the firmware ---------------- */

uint32_t HAL_GetTick(void);
void     HAL_Delay(uint32_t ms);
void     Error_Handler(void);

HAL_StatusTypeDef HAL_CAN_ConfigFilter(CAN_HandleTypeDef *h, CAN_FilterTypeDef *f);
HAL_StatusTypeDef HAL_CAN_Start(CAN_HandleTypeDef *h);
HAL_StatusTypeDef HAL_CAN_Stop(CAN_HandleTypeDef *h);
HAL_StatusTypeDef HAL_CAN_ActivateNotification(CAN_HandleTypeDef *h, uint32_t its);
uint32_t          HAL_CAN_GetError(CAN_HandleTypeDef *h);
HAL_StatusTypeDef HAL_CAN_ResetError(CAN_HandleTypeDef *h);
uint32_t          HAL_CAN_GetTxMailboxesFreeLevel(CAN_HandleTypeDef *h);
HAL_StatusTypeDef HAL_CAN_AddTxMessage(CAN_HandleTypeDef *h, CAN_TxHeaderTypeDef *hdr,
                                       uint8_t *data, uint32_t *mailbox);
uint32_t          HAL_CAN_GetRxFifoFillLevel(CAN_HandleTypeDef *h, uint32_t fifo);
HAL_StatusTypeDef HAL_CAN_GetRxMessage(CAN_HandleTypeDef *h, uint32_t fifo,
                                       CAN_RxHeaderTypeDef *hdr, uint8_t *data);

HAL_StatusTypeDef HAL_UART_Transmit(UART_HandleTypeDef *h, uint8_t *p, uint16_t n, uint32_t t);
HAL_StatusTypeDef HAL_UART_Receive(UART_HandleTypeDef *h, uint8_t *p, uint16_t n, uint32_t t);
HAL_StatusTypeDef HAL_UART_Transmit_DMA(UART_HandleTypeDef *h, uint8_t *p, uint16_t n);

GPIO_PinState HAL_GPIO_ReadPin(GPIO_TypeDef *port, uint16_t pin);
void          HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState st);

/* ---------------- test control surface ---------------- */

void     shim_reset(void);
void     shim_set_tick(uint32_t ms);
void     shim_advance(uint32_t ms);
void     shim_set_can_error(uint32_t err);
void     shim_set_tx_mailboxes_free(uint32_t n);
void     shim_set_aux_busy(bool busy);

/** Frames the node handed to a TX mailbox. */
uint32_t shim_tx_count(void);
bool     shim_tx_get(uint32_t i, uint32_t *std_id, uint8_t *data8);
void     shim_tx_clear(void);

/** Push a frame into the hub's RX FIFO, then run its interrupt callback. */
void     shim_can_inject(uint32_t std_id, const uint8_t *data8);

/** Bytes the hub sent to the LoRa UART. */
uint32_t shim_lora_len(void);
const uint8_t *shim_lora_data(void);
void     shim_lora_clear(void);

/** Text the hub sent to the debug UART. */
const char *shim_debug_text(void);
void     shim_debug_clear(void);

#endif /* SHIM_MAIN_H */
