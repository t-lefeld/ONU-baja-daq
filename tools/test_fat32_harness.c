/**
 * test_fat32_harness.c - Exercises fat32.c against a disk image file.
 *
 * Built and driven by tools/test_fat32.py, which formats a real FAT32 image
 * with mkfs.vfat, runs this, then validates the result with fsck.fat and by
 * parsing the filesystem independently in Python.
 *
 * Testing a filesystem writer against the real thing rather than against a
 * mock is the whole point: the failure modes that matter (a FAT mirror that
 * disagrees, a directory entry whose size field lags the allocated chain, a
 * missing end-of-directory marker) are invisible to a mock and immediately
 * obvious to fsck.
 *
 *   usage: harness <image> <n_files> <bytes_per_file> <chunk> <sync_every>
 */

#include "fat32.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE *g_img;
static unsigned long g_reads, g_writes;

static int img_read(uint32_t lba, uint8_t *buf)
{
    if (fseek(g_img, (long)lba * FAT32_SECTOR_SIZE, SEEK_SET) != 0) return 1;
    if (fread(buf, 1, FAT32_SECTOR_SIZE, g_img) != FAT32_SECTOR_SIZE) return 1;
    g_reads++;
    return 0;
}

static int img_write(uint32_t lba, const uint8_t *buf)
{
    if (fseek(g_img, (long)lba * FAT32_SECTOR_SIZE, SEEK_SET) != 0) return 1;
    if (fwrite(buf, 1, FAT32_SECTOR_SIZE, g_img) != FAT32_SECTOR_SIZE) return 1;
    g_writes++;
    return 0;
}

static const fat32_bdev_t bdev = { img_read, img_write };

/* Deterministic filler so the Python side can verify content byte for byte. */
static uint8_t pattern(uint32_t file_idx, uint32_t off)
{
    return (uint8_t)((off * 31u + file_idx * 101u + (off >> 8) * 7u) & 0xFFu);
}

int main(int argc, char **argv)
{
    if (argc != 6)
    {
        fprintf(stderr, "usage: %s <image> <n_files> <bytes> <chunk> <sync_every>\n", argv[0]);
        return 2;
    }

    const char *path      = argv[1];
    uint32_t n_files      = (uint32_t)strtoul(argv[2], NULL, 10);
    uint32_t bytes        = (uint32_t)strtoul(argv[3], NULL, 10);
    uint32_t chunk        = (uint32_t)strtoul(argv[4], NULL, 10);
    uint32_t sync_every   = (uint32_t)strtoul(argv[5], NULL, 10);

    g_img = fopen(path, "r+b");
    if (!g_img)
    {
        perror("open image");
        return 2;
    }

    static fat32_fs_t fs;
    fat32_err_t rc = fat32_mount(&fs, &bdev);

    if (rc != FAT32_OK)
    {
        printf("MOUNT_FAIL %d\n", rc);
        return 1;
    }

    printf("MOUNT ok spc=%u nfats=%u fatlba=%u fatsec=%u datalba=%u root=%u clus=%u\n",
           fs.sec_per_clus, fs.num_fats, fs.fat_lba, fs.fat_sectors,
           fs.data_lba, fs.root_clus, fs.total_clus);

    uint8_t *chunkbuf = malloc(chunk);
    if (!chunkbuf) return 2;

    for (uint32_t i = 0; i < n_files; i++)
    {
        static fat32_file_t f;
        char name[16];

        rc = fat32_create_numbered(&fs, &f, "LOG", "TLM", name);
        if (rc != FAT32_OK)
        {
            printf("CREATE_FAIL %d\n", rc);
            return 1;
        }

        printf("CREATE %s\n", name);

        uint32_t written = 0, since_sync = 0;

        while (written < bytes)
        {
            uint32_t n = (bytes - written < chunk) ? (bytes - written) : chunk;

            for (uint32_t k = 0; k < n; k++)
            {
                chunkbuf[k] = pattern(i, written + k);
            }

            rc = fat32_write(&f, chunkbuf, n);
            if (rc != FAT32_OK)
            {
                printf("WRITE_FAIL %d at %u\n", rc, written);
                return 1;
            }

            written += n;

            /* Sync mid-file, mirroring what sd_log.c does. This is where a
               partial-sector flush bug would show up as a corrupt file. */
            if (sync_every && ++since_sync >= sync_every)
            {
                since_sync = 0;
                rc = fat32_sync(&f);
                if (rc != FAT32_OK)
                {
                    printf("SYNC_FAIL %d\n", rc);
                    return 1;
                }
            }
        }

        rc = fat32_close(&f);
        if (rc != FAT32_OK)
        {
            printf("CLOSE_FAIL %d\n", rc);
            return 1;
        }

        printf("CLOSED %s %u\n", name, f.size);
    }

    /* Re-mounting must see the files we just wrote. */
    static fat32_fs_t fs2;
    if (fat32_mount(&fs2, &bdev) != FAT32_OK)
    {
        printf("REMOUNT_FAIL\n");
        return 1;
    }

    printf("EXISTS_LOG0001 %d\n", fat32_exists(&fs2, "LOG0001 TLM") ? 1 : 0);
    printf("EXISTS_LOG9999 %d\n", fat32_exists(&fs2, "LOG9999 TLM") ? 1 : 0);
    printf("IO reads=%lu writes=%lu\n", g_reads, g_writes);

    free(chunkbuf);
    fclose(g_img);
    return 0;
}
