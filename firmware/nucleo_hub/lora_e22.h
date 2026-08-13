/**
 * lora_e22.h - Driver for Ebyte E22-900T22D / E22-900T22S (SX1262) over UART.
 *
 * IMPORTANT, because the part numbers are one letter apart:
 *
 *   E22-900T22U  is a USB dongle. It plugs into the PC. It is the RECEIVER.
 *   E22-900T22D  is a DIP module with a UART header.   <- the hub needs this
 *   E22-900T22S  is the SMD version of the same thing. <- or this
 *
 * A -U cannot be wired to the Nucleo's UART pins; its serial lines go through
 * an onboard USB-serial bridge.
 *
 * Wiring (D/S variant to the Nucleo-L476RG):
 *
 *   E22 M0   -> PB0    A3    output, mode select bit 0
 *   E22 M1   -> PA4    A2    output, mode select bit 1
 *   E22 AUX  -> PA1    A1    input with pull-up, module-busy indicator (open drain)
 *   E22 RXD  -> PA9    D8    USART1_TX
 *   E22 TXD  -> PA10   D2    USART1_RX
 *   E22 VCC  -> 3.3V   see the note on current below
 *   E22 GND  -> GND
 *
 * The "A1/A2/A3/D2/D8" column is the Nucleo-64 Arduino Uno R3 header position
 * for each pin (UM1724 Table 23). All five E22 signal wires land on that
 * header - PB1 and PB2 (the previous M1/AUX pins) do not, so they would have
 * needed the ST Morpho connector instead. Moving M1/AUX to PA4/PA1 means the
 * whole module can be wired through one connector.
 *
 * Power. At 22 dBm the module draws well over 600 mA in transmit bursts. The
 * Nucleo's 3.3V regulator, when the board is USB-powered, cannot supply that.
 * Give the module its own 3.3V supply with a bulk capacitor (470 uF or more)
 * close to the pins and tie the grounds together. Skipping this produces the
 * classic symptom of a link that works on the bench with short packets and
 * browns out the instant you raise the transmit power or packet length.
 *
 * Frequency. Factory default is 868.125 MHz, which is the European band. In
 * the United States the licence-free band is 902-928 MHz. Channel is set by
 * register 05H as (frequency_MHz - 850.125), so channel 0x45 (69) gives
 * 919.125 MHz. Set the same channel on both ends. See lora_e22_configure().
 */

#ifndef LORA_E22_H
#define LORA_E22_H

#include "main.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Pin assignments. Change to match your board. ------------------ */
#define E22_M0_PORT   GPIOB
#define E22_M0_PIN    GPIO_PIN_0
#define E22_M1_PORT   GPIOA
#define E22_M1_PIN    GPIO_PIN_4
#define E22_AUX_PORT  GPIOA
#define E22_AUX_PIN   GPIO_PIN_1

/** Set to 0 if you tied M0/M1 low in hardware and have no GPIO control. */
#ifndef E22_HAS_MODE_PINS
#define E22_HAS_MODE_PINS 1
#endif

/** Set to 0 if AUX is not connected. Timing then falls back to a fixed delay. */
#ifndef E22_HAS_AUX_PIN
#define E22_HAS_AUX_PIN 1
#endif

typedef enum {
    E22_MODE_NORMAL    = 0,  /* M0=0 M1=0  transparent transmission        */
    E22_MODE_WOR_TX    = 1,  /* M0=1 M1=0                                  */
    E22_MODE_WOR_RX    = 2,  /* M0=0 M1=1                                  */
    E22_MODE_CONFIG    = 3,  /* M0=1 M1=1  register access, always 9600 8N1 */
} e22_mode_t;

/* Register 03H, bits 2-0. Both ends must agree. */
typedef enum {
    E22_AIR_2K4  = 0x02,
    E22_AIR_4K8  = 0x03,
    E22_AIR_9K6  = 0x04,
    E22_AIR_19K2 = 0x05,
} e22_air_rate_t;

/* Register 04H, bits 1-0. */
typedef enum {
    E22_PWR_22DBM = 0x00,
    E22_PWR_17DBM = 0x01,
    E22_PWR_13DBM = 0x02,
    E22_PWR_10DBM = 0x03,
} e22_power_t;

/**
 * Bind the driver to a UART. TX must have DMA configured in CubeMX, and the
 * USART global interrupt must be enabled so the DMA-complete callback fires.
 */
void lora_e22_init(UART_HandleTypeDef *huart);

/** Service pending state. Call from the main loop. */
void lora_e22_task(void);

/**
 * Queue a buffer for transmission. Returns false if the radio is still busy
 * with the previous packet, in which case the caller should skip this cycle
 * rather than block - for periodic telemetry, fresher data is worth more than
 * a guaranteed-delivered stale packet.
 *
 * @p len must not exceed 240 bytes, the module's maximum sub-packet size.
 */
bool lora_e22_send(const uint8_t *data, size_t len);

/** True while a transmission is in flight. */
bool lora_e22_busy(void);

/** Switch operating mode via M0/M1 and wait for AUX to settle. */
void lora_e22_set_mode(e22_mode_t mode);

/**
 * Write the persistent configuration registers.
 *
 * Enters config mode, sends a C0 command, restores normal mode. Blocking,
 * takes roughly 200 ms. Call once at startup if you want to force settings,
 * or leave it out entirely and configure the modules with Ebyte's Windows
 * tool - which is easier to verify, since it reads the settings back.
 *
 * Verify the air-rate bit encoding against the manual revision that came with
 * your modules before trusting this in the field. Ebyte has shipped more than
 * one register layout under the E22 name, and a mismatched air rate produces
 * a link that looks alive (AUX toggles, no errors reported) but never
 * delivers a packet.
 *
 * @param address  16-bit module address, 0xFFFF for broadcast
 * @param netid    network id, must match on both ends
 * @param channel  frequency_MHz - 850.125, so 0x45 = 919.125 MHz
 */
bool lora_e22_configure(uint16_t address,
                        uint8_t  netid,
                        uint8_t  channel,
                        e22_air_rate_t air_rate,
                        e22_power_t    power);

#ifdef __cplusplus
}
#endif

#endif /* LORA_E22_H */
