/**
 * sd_log.c - Append-only binary logger, built on fat32.c + sd_spi.c.
 */

#include "sd_log.h"
#include "fat32.h"
#include "main.h"
#include "sd_spi.h"
#include <string.h>

/*
 * How many frames between syncs.
 *
 * fat32_sync() rewrites the current partial sector and the directory entry:
 * two card writes. Doing it every frame roughly triples the write load for no
 * real benefit. Never doing it means an unclean power-down leaves a file whose
 * directory entry still says length zero, so the data is on the card but no OS
 * will show it to you.
 *
 * 20 frames at 2 Hz is a 10-second worst-case loss window. Lower it if the hub
 * is likely to lose power abruptly.
 */
#define SD_SYNC_EVERY_N_FRAMES 20u

/* Do not retry a missing card on every pass of the main loop. */
#define SD_RETRY_INTERVAL_MS 5000u

static fat32_fs_t   s_fs;
static fat32_file_t s_file;
static bool         s_open;
static char         s_name[16];
static uint32_t     s_bytes;
static uint32_t     s_writes;
static uint32_t     s_next_retry_ms;

bool sd_log_init(void)
{
    if (s_open)
    {
        return true;
    }

    uint32_t now = HAL_GetTick();

    if (s_next_retry_ms != 0u && (int32_t)(now - s_next_retry_ms) < 0)
    {
        return false;
    }
    s_next_retry_ms = now + SD_RETRY_INTERVAL_MS;

    if (!sd_spi_init())
    {
        return false;
    }

    if (fat32_mount(&s_fs, &sd_spi_bdev) != FAT32_OK)
    {
        return false;
    }

    if (fat32_create_numbered(&s_fs, &s_file, "LOG", "TLM", s_name) != FAT32_OK)
    {
        s_name[0] = '\0';
        return false;
    }

    s_open   = true;
    s_bytes  = 0u;
    s_writes = 0u;

    return true;
}

bool sd_log_write(const uint8_t *data, size_t len)
{
    if (!s_open)
    {
        /* Cheap retry - a card inserted after boot starts logging on its own. */
        return sd_log_init();
    }

    if (fat32_write(&s_file, data, (uint32_t)len) != FAT32_OK)
    {
        /* A write failure usually means the card was pulled. Drop to the
           closed state so the retry path picks it up cleanly. */
        sd_log_close();
        return false;
    }

    s_bytes += (uint32_t)len;

    if (++s_writes >= SD_SYNC_EVERY_N_FRAMES)
    {
        s_writes = 0u;

        if (fat32_sync(&s_file) != FAT32_OK)
        {
            sd_log_close();
            return false;
        }
    }

    return true;
}

bool sd_log_flush(void)
{
    if (!s_open)
    {
        return false;
    }

    return fat32_sync(&s_file) == FAT32_OK;
}

void sd_log_close(void)
{
    if (s_open)
    {
        (void)fat32_close(&s_file);
        s_open = false;
    }

    s_name[0] = '\0';
}

bool sd_log_ready(void)           { return s_open; }
uint32_t sd_log_bytes(void)       { return s_bytes; }
const char *sd_log_filename(void) { return s_name; }
