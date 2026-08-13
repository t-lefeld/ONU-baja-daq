/**
 * lora_e22.c - E22-900T22D/S UART driver.
 */

#include "lora_e22.h"
#include <string.h>

#define E22_MAX_PACKET 240u

static UART_HandleTypeDef *s_uart;
static volatile bool       s_tx_active;
static uint8_t             s_buf[E22_MAX_PACKET];

/*
 * The module raises AUX low while it is busy and releases it when idle. After
 * AUX goes high the datasheet asks for a further 2 ms before the next byte.
 */
#define E22_AUX_SETTLE_MS   3u
#define E22_AUX_TIMEOUT_MS  1000u
#define E22_MODE_DELAY_MS   20u

/* ------------------------------------------------------------------ */
/* AUX / mode handling                                                 */
/* ------------------------------------------------------------------ */

static void aux_wait_idle(void)
{
#if E22_HAS_AUX_PIN
    uint32_t deadline = HAL_GetTick() + E22_AUX_TIMEOUT_MS;

    while (HAL_GPIO_ReadPin(E22_AUX_PORT, E22_AUX_PIN) == GPIO_PIN_RESET)
    {
        if ((int32_t)(HAL_GetTick() - deadline) >= 0)
        {
            return;   /* module unresponsive; carry on rather than hang */
        }
    }

    HAL_Delay(E22_AUX_SETTLE_MS);
#else
    HAL_Delay(E22_MODE_DELAY_MS);
#endif
}

void lora_e22_set_mode(e22_mode_t mode)
{
#if E22_HAS_MODE_PINS
    HAL_GPIO_WritePin(E22_M0_PORT, E22_M0_PIN,
                      (mode & 0x1u) ? GPIO_PIN_SET : GPIO_PIN_RESET);
    HAL_GPIO_WritePin(E22_M1_PORT, E22_M1_PIN,
                      (mode & 0x2u) ? GPIO_PIN_SET : GPIO_PIN_RESET);

    HAL_Delay(E22_MODE_DELAY_MS);
    aux_wait_idle();
#else
    (void)mode;
#endif
}

/* ------------------------------------------------------------------ */
/* Transmit                                                            */
/* ------------------------------------------------------------------ */

void lora_e22_init(UART_HandleTypeDef *huart)
{
    s_uart      = huart;
    s_tx_active = false;

    lora_e22_set_mode(E22_MODE_NORMAL);
}

bool lora_e22_send(const uint8_t *data, size_t len)
{
    if (s_uart == NULL || len == 0u || len > E22_MAX_PACKET)
    {
        return false;
    }

    if (s_tx_active || s_uart->gState != HAL_UART_STATE_READY)
    {
        return false;
    }

#if E22_HAS_AUX_PIN
    /*
     * Non-blocking check. If the module is still chewing on the previous
     * packet we decline rather than spin - the caller has a fresher sample
     * coming in 500 ms regardless.
     */
    if (HAL_GPIO_ReadPin(E22_AUX_PORT, E22_AUX_PIN) == GPIO_PIN_RESET)
    {
        return false;
    }
#endif

    /*
     * Copy into a driver-owned buffer. The DMA reads this asynchronously and
     * the caller's buffer may be reused the moment we return.
     */
    memcpy(s_buf, data, len);
    s_tx_active = true;

    if (HAL_UART_Transmit_DMA(s_uart, s_buf, (uint16_t)len) != HAL_OK)
    {
        s_tx_active = false;
        return false;
    }

    return true;
}

bool lora_e22_busy(void)
{
    return s_tx_active;
}

void lora_e22_task(void)
{
    /*
     * Nothing to do in the common case; the DMA completion callback clears the
     * flag. This exists as the hook point for retries or an outgoing queue if
     * you later add downlink or acknowledgements.
     */
}

/**
 * HAL weak callback, fires when the DMA has pushed the last byte into the UART
 * data register. Note that the final byte is still shifting out on the wire at
 * this point; the module's own AUX line is what tells us when it is truly done,
 * which is why lora_e22_send() checks AUX as well as this flag.
 */
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart == s_uart)
    {
        s_tx_active = false;
    }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart == s_uart)
    {
        s_tx_active = false;
        __HAL_UART_CLEAR_OREFLAG(huart);
    }
}

/* ------------------------------------------------------------------ */
/* Register configuration                                              */
/* ------------------------------------------------------------------ */
/*
 * Command layout:  C0 <start_reg> <length> <data...>
 * The module echoes C1 <start_reg> <length> <data...> on success.
 *
 * Register map:
 *   00H  ADDH
 *   01H  ADDL
 *   02H  NETID
 *   03H  REG0   bits 7-5 UART baud, 4-3 parity, 2-0 air data rate
 *   04H  REG1   bits 7-6 sub-packet size, 5 RSSI noise, 1-0 transmit power
 *   05H  REG2   channel = frequency_MHz - 850.125
 *   06H  REG3   bit 7 RSSI byte, bit 6 fixed-point mode, bit 4 LBT, 2-0 WOR
 *
 * Config mode always runs at 9600 8N1 regardless of the configured UART baud,
 * so if you change the baud rate you must reinitialise the STM32 UART to match
 * before returning to normal mode.
 */
bool lora_e22_configure(uint16_t address,
                        uint8_t  netid,
                        uint8_t  channel,
                        e22_air_rate_t air_rate,
                        e22_power_t    power)
{
    if (s_uart == NULL)
    {
        return false;
    }

    lora_e22_set_mode(E22_MODE_CONFIG);

    uint8_t cmd[12];
    cmd[0]  = 0xC0;                       /* write, save to flash        */
    cmd[1]  = 0x00;                       /* starting register           */
    cmd[2]  = 0x07;                       /* number of registers         */
    cmd[3]  = (uint8_t)(address >> 8);    /* 00H ADDH                    */
    cmd[4]  = (uint8_t)(address & 0xFF);  /* 01H ADDL                    */
    cmd[5]  = netid;                      /* 02H NETID                   */
    cmd[6]  = (uint8_t)(0x60u |           /* 03H  9600 baud, 8N1, ...    */
                        ((uint8_t)air_rate & 0x07u));
    cmd[7]  = (uint8_t)(0x00u |           /* 04H  240-byte sub-packet    */
                        ((uint8_t)power & 0x03u));
    cmd[8]  = channel;                    /* 05H                         */
    cmd[9]  = 0x03;                       /* 06H  transparent, no RSSI   */

    aux_wait_idle();

    if (HAL_UART_Transmit(s_uart, cmd, 10u, 200u) != HAL_OK)
    {
        lora_e22_set_mode(E22_MODE_NORMAL);
        return false;
    }

    /* Read the echo back so a silent module is reported rather than assumed OK. */
    uint8_t reply[10] = {0};
    HAL_StatusTypeDef st = HAL_UART_Receive(s_uart, reply, 10u, 500u);

    lora_e22_set_mode(E22_MODE_NORMAL);

    return (st == HAL_OK) && (reply[0] == 0xC1);
}
