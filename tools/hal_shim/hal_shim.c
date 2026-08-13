/**
 * hal_shim.c - Implementation of the host-side HAL stand-in.
 */

#include "main.h"
#include <stdio.h>

static CAN_TypeDef   s_can1;
static GPIO_TypeDef  s_gpioa = { 0 };
static GPIO_TypeDef  s_gpiob = { 1 };

CAN_TypeDef  *const CAN1  = &s_can1;
GPIO_TypeDef *const GPIOA = &s_gpioa;
GPIO_TypeDef *const GPIOB = &s_gpiob;

/* ---------------- clock ---------------- */

static uint32_t s_tick;

uint32_t HAL_GetTick(void) { return s_tick; }
void shim_set_tick(uint32_t ms) { s_tick = ms; }
void shim_advance(uint32_t ms) { s_tick += ms; }

/*
 * HAL_Delay advances virtual time instead of sleeping. Without this the LoRa
 * driver's mode-change delays would make the test take real seconds; with it,
 * the delays still show up in the timeline the code sees.
 */
void HAL_Delay(uint32_t ms) { s_tick += ms; }

void Error_Handler(void)
{
    fprintf(stderr, "Error_Handler() called\n");
    /* Not exit(): a test wants to see what happened after, and the real
       handler halts rather than unwinding. */
}

/* ---------------- CAN ---------------- */

#define TXQ_MAX 4096
#define RXQ_MAX 256

typedef struct { uint32_t std_id; uint8_t data[8]; } frame_t;

static frame_t  s_txq[TXQ_MAX];
static uint32_t s_txn;

static frame_t  s_rxq[RXQ_MAX];
static uint32_t s_rx_head, s_rx_tail;

static uint32_t s_can_error;
static uint32_t s_mailboxes_free = 3;
static bool     s_aux_busy;

HAL_StatusTypeDef HAL_CAN_ConfigFilter(CAN_HandleTypeDef *h, CAN_FilterTypeDef *f)
{ (void)h; (void)f; return HAL_OK; }

HAL_StatusTypeDef HAL_CAN_Start(CAN_HandleTypeDef *h) { (void)h; return HAL_OK; }
HAL_StatusTypeDef HAL_CAN_Stop(CAN_HandleTypeDef *h)  { (void)h; return HAL_OK; }

HAL_StatusTypeDef HAL_CAN_ActivateNotification(CAN_HandleTypeDef *h, uint32_t its)
{ (void)h; (void)its; return HAL_OK; }

uint32_t HAL_CAN_GetError(CAN_HandleTypeDef *h) { (void)h; return s_can_error; }

HAL_StatusTypeDef HAL_CAN_ResetError(CAN_HandleTypeDef *h)
{ (void)h; s_can_error = HAL_CAN_ERROR_NONE; return HAL_OK; }

uint32_t HAL_CAN_GetTxMailboxesFreeLevel(CAN_HandleTypeDef *h)
{ (void)h; return s_mailboxes_free; }

HAL_StatusTypeDef HAL_CAN_AddTxMessage(CAN_HandleTypeDef *h, CAN_TxHeaderTypeDef *hdr,
                                       uint8_t *data, uint32_t *mailbox)
{
    (void)h;
    if (s_txn >= TXQ_MAX) return HAL_ERROR;
    s_txq[s_txn].std_id = hdr->StdId;
    memcpy(s_txq[s_txn].data, data, 8);
    s_txn++;
    if (mailbox) *mailbox = 0;
    return HAL_OK;
}

uint32_t HAL_CAN_GetRxFifoFillLevel(CAN_HandleTypeDef *h, uint32_t fifo)
{ (void)h; (void)fifo; return s_rx_head - s_rx_tail; }

HAL_StatusTypeDef HAL_CAN_GetRxMessage(CAN_HandleTypeDef *h, uint32_t fifo,
                                       CAN_RxHeaderTypeDef *hdr, uint8_t *data)
{
    (void)h; (void)fifo;
    if (s_rx_head == s_rx_tail) return HAL_ERROR;

    const frame_t *f = &s_rxq[s_rx_tail % RXQ_MAX];
    memset(hdr, 0, sizeof(*hdr));
    hdr->StdId = f->std_id;
    hdr->IDE   = CAN_ID_STD;
    hdr->RTR   = CAN_RTR_DATA;
    hdr->DLC   = 8;
    memcpy(data, f->data, 8);
    s_rx_tail++;
    return HAL_OK;
}

/* Declared by telemetry_hub.c; the shim calls it to emulate the interrupt. */
void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *hcan);

static CAN_HandleTypeDef *s_hub_can;

void shim_can_inject(uint32_t std_id, const uint8_t *data8)
{
    s_rxq[s_rx_head % RXQ_MAX].std_id = std_id;
    memcpy(s_rxq[s_rx_head % RXQ_MAX].data, data8, 8);
    s_rx_head++;

    if (s_hub_can)
    {
        HAL_CAN_RxFifo0MsgPendingCallback(s_hub_can);
    }
}

void shim_bind_hub_can(CAN_HandleTypeDef *h) { s_hub_can = h; }

/* ---------------- UART ---------------- */

#define LORA_MAX  65536
#define DEBUG_MAX 65536

static uint8_t s_lora[LORA_MAX];
static uint32_t s_lora_n;

static char s_debug[DEBUG_MAX];
static uint32_t s_debug_n;

static UART_HandleTypeDef *s_lora_uart;

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart);

HAL_StatusTypeDef HAL_UART_Transmit_DMA(UART_HandleTypeDef *h, uint8_t *p, uint16_t n)
{
    s_lora_uart = h;

    if (s_lora_n + n <= LORA_MAX)
    {
        memcpy(&s_lora[s_lora_n], p, n);
        s_lora_n += n;
    }

    /*
     * Complete immediately. Real DMA takes ~44 ms for 42 bytes at 9600 baud,
     * but the driver's contract is "busy until TxCplt fires", and testing that
     * contract is what matters here - not the wall-clock duration.
     */
    HAL_UART_TxCpltCallback(h);
    return HAL_OK;
}

HAL_StatusTypeDef HAL_UART_Transmit(UART_HandleTypeDef *h, uint8_t *p, uint16_t n, uint32_t t)
{
    (void)t;

    if (h == s_lora_uart)
    {
        if (s_lora_n + n <= LORA_MAX) { memcpy(&s_lora[s_lora_n], p, n); s_lora_n += n; }
    }
    else if (s_debug_n + n < DEBUG_MAX)
    {
        memcpy(&s_debug[s_debug_n], p, n);
        s_debug_n += n;
        s_debug[s_debug_n] = '\0';
    }
    return HAL_OK;
}

HAL_StatusTypeDef HAL_UART_Receive(UART_HandleTypeDef *h, uint8_t *p, uint16_t n, uint32_t t)
{ (void)h; (void)t; memset(p, 0, n); return HAL_TIMEOUT; }

uint32_t       shim_lora_len(void)  { return s_lora_n; }
const uint8_t *shim_lora_data(void) { return s_lora; }
void           shim_lora_clear(void){ s_lora_n = 0; }

const char *shim_debug_text(void)  { return s_debug; }
void        shim_debug_clear(void) { s_debug_n = 0; s_debug[0] = '\0'; }

/* ---------------- GPIO ---------------- */

GPIO_PinState HAL_GPIO_ReadPin(GPIO_TypeDef *port, uint16_t pin)
{
    (void)port;
    /* E22 AUX moved from PB2 to PA1 so the whole module fits on the Nucleo's
       Arduino header (see lora_e22.h). GPIO_PIN_1 here means AUX specifically
       because it's the only pin lora_e22.c ever reads - M0/M1 are outputs. */
    if (pin == GPIO_PIN_1)   /* E22 AUX */
    {
        return s_aux_busy ? GPIO_PIN_RESET : GPIO_PIN_SET;
    }
    return GPIO_PIN_SET;
}

void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState st)
{ (void)port; (void)pin; (void)st; }

/* ---------------- test controls ---------------- */

void shim_set_can_error(uint32_t err)       { s_can_error = err; }
void shim_set_tx_mailboxes_free(uint32_t n) { s_mailboxes_free = n; }
void shim_set_aux_busy(bool busy)           { s_aux_busy = busy; }

uint32_t shim_tx_count(void) { return s_txn; }
void     shim_tx_clear(void) { s_txn = 0; }

bool shim_tx_get(uint32_t i, uint32_t *std_id, uint8_t *data8)
{
    if (i >= s_txn) return false;
    if (std_id) *std_id = s_txq[i].std_id;
    if (data8)  memcpy(data8, s_txq[i].data, 8);
    return true;
}

void shim_reset(void)
{
    s_tick = 0;
    s_txn = 0;
    s_rx_head = s_rx_tail = 0;
    s_can_error = HAL_CAN_ERROR_NONE;
    s_mailboxes_free = 3;
    s_aux_busy = false;
    s_lora_n = 0;
    s_debug_n = 0;
    s_debug[0] = '\0';
}
