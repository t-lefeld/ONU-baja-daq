/**
 * sd_spi.h - SD/SDHC card block driver over SPI1, register level.
 *
 * Written against the SPI registers directly rather than the HAL because your
 * Nucleo project does not include stm32l4xx_hal_spi.c, and that file can only
 * be added by regenerating from CubeMX. Bare-register SPI is about sixty lines
 * and has no downside here: the transfers are byte-at-a-time polled anyway,
 * which is what the HAL would do underneath.
 *
 * Wiring (Nucleo-L476RG):
 *
 *   PA5  SPI1_SCK   -> SD CLK
 *   PA6  SPI1_MISO  -> SD DO
 *   PA7  SPI1_MOSI  -> SD DI
 *   PB6  GPIO out   -> SD CS   (active low)
 *
 * PA5 also drives LD2 on the Nucleo. The LED will flicker with SPI traffic;
 * that is harmless and makes card activity visible.
 *
 * Level shifting: most SD breakout boards for Arduino run at 5V and include a
 * regulator plus level shifters. If yours is a bare 3.3V socket, wire it
 * directly. If it is a 5V-logic board, do not feed 5V into PA6.
 *
 * Pull-ups: SD cards in SPI mode want a pull-up on MISO and CS. Many breakouts
 * include them. sd_spi_init() enables the internal pull-up on PA6 as insurance,
 * which is weak but usually sufficient for short wiring.
 */

#ifndef SD_SPI_H
#define SD_SPI_H

#include "fat32.h"
#include "main.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Pin assignments. Change to match your board. ------------------ */
#define SD_CS_PORT   GPIOB
#define SD_CS_PIN    GPIO_PIN_6

typedef enum {
    SD_TYPE_NONE = 0,
    SD_TYPE_SDSC = 1,   /* standard capacity, byte addressed  */
    SD_TYPE_SDHC = 2,   /* high capacity, block addressed     */
} sd_type_t;

/**
 * Bring up SPI1 and negotiate with the card.
 * Returns false if no card responds; safe to call again later.
 */
bool sd_spi_init(void);

/** Card type detected by the last successful init. */
sd_type_t sd_spi_type(void);

/** Capacity in 512-byte blocks, or 0 if unknown. */
uint32_t sd_spi_block_count(void);

/** Read one 512-byte block. Returns 0 on success. */
int sd_spi_read_block(uint32_t lba, uint8_t *buf);

/** Write one 512-byte block. Returns 0 on success. */
int sd_spi_write_block(uint32_t lba, const uint8_t *buf);

/** Block device wrapper for fat32_mount(). */
extern const fat32_bdev_t sd_spi_bdev;

#ifdef __cplusplus
}
#endif

#endif /* SD_SPI_H */
