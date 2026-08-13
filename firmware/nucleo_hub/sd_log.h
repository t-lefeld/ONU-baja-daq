/**
 * sd_log.h - Append-only binary logger for the hub's SD card.
 *
 * Writes the exact bytes that go over the radio, one 42-byte frame after
 * another, into LOGnnnn.TLM in the card root. Because the file format is
 * identical to the radio stream, the PC app can replay a log through the same
 * decoder it uses live:
 *
 *     python run.py --replay LOG0001.TLM
 *
 * and tools/tlm_to_csv.py converts one to a spreadsheet.
 *
 * Built on fat32.c and sd_spi.c, both self-contained. No FatFs, no SPI HAL,
 * nothing that has to come out of CubeMX. See INTEGRATION.md for wiring.
 */

#ifndef SD_LOG_H
#define SD_LOG_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Mount the card and open the next free log file. Safe to call again later to
 * retry after a card was inserted; returns false while no card is present.
 */
bool sd_log_init(void);

/** Append bytes. Returns false on any filesystem error. */
bool sd_log_write(const uint8_t *data, size_t len);

/** Flush FatFs buffers to the card. Called automatically; exposed for shutdown. */
bool sd_log_flush(void);

/** Close cleanly. */
void sd_log_close(void);

/** True once a file is open and writable. */
bool sd_log_ready(void);

/** Bytes written to the current file. */
uint32_t sd_log_bytes(void);

/** Name of the current file, or an empty string. */
const char *sd_log_filename(void);

#ifdef __cplusplus
}
#endif

#endif /* SD_LOG_H */
