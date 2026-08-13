/**
 * fat32.h - Minimal append-only FAT32 writer.
 *
 * Why this instead of FatFs: your Nucleo project has no FatFs middleware and no
 * SPI HAL driver, and neither can be added by dropping in source files - they
 * come from CubeMX. Rather than make you regenerate the project, this is a
 * self-contained writer that does exactly what the telemetry logger needs and
 * nothing else.
 *
 * Scope, deliberately small:
 *   - FAT32 only, 512-byte sectors only
 *   - short (8.3) filenames in the root directory only
 *   - create and append; no read, no delete, no seek, no subdirectories
 *
 * That covers "open a new log file and stream frames into it" completely, and
 * the resulting card is a normal FAT32 volume that Windows, macOS and Linux
 * mount without complaint.
 *
 * If you later want the full filesystem, add FatFs in CubeMX and swap sd_log.c
 * back to the f_open/f_write API - the layer above does not care.
 *
 * The block device is abstracted behind two function pointers so the whole
 * thing can be exercised on a host against a disk image, which is how the
 * implementation in this repo was tested.
 */

#ifndef FAT32_H
#define FAT32_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FAT32_SECTOR_SIZE 512u

/** Return 0 on success, non-zero on failure. */
typedef struct {
    int (*read)(uint32_t lba, uint8_t *buf);
    int (*write)(uint32_t lba, const uint8_t *buf);
} fat32_bdev_t;

typedef enum {
    FAT32_OK = 0,
    FAT32_ERR_IO = -1,
    FAT32_ERR_NOT_FAT32 = -2,
    FAT32_ERR_BAD_SECTOR_SIZE = -3,
    FAT32_ERR_FULL = -4,        /* no free clusters                     */
    FAT32_ERR_DIR_FULL = -5,    /* no free root directory entry         */
    FAT32_ERR_EXISTS = -6,
    FAT32_ERR_NOT_OPEN = -7,
    FAT32_ERR_PARAM = -8,
} fat32_err_t;

typedef struct {
    const fat32_bdev_t *dev;

    uint32_t part_lba;        /* first sector of the partition          */
    uint8_t  sec_per_clus;
    uint8_t  num_fats;
    uint32_t fat_lba;         /* absolute LBA of FAT #0                 */
    uint32_t fat_sectors;     /* sectors per FAT                        */
    uint32_t data_lba;        /* absolute LBA of cluster 2              */
    uint32_t root_clus;
    uint32_t total_clus;      /* number of data clusters + 2            */
    uint32_t fsinfo_lba;      /* 0 if absent                            */
    uint32_t next_free;       /* allocation hint                        */

    uint8_t  scratch[FAT32_SECTOR_SIZE];
    bool     mounted;
} fat32_fs_t;

typedef struct {
    fat32_fs_t *fs;

    uint32_t first_clus;
    uint32_t clus;            /* cluster being written                  */
    uint32_t sec_in_clus;
    uint32_t size;

    uint32_t dir_lba;         /* sector holding this file's dir entry   */
    uint16_t dir_off;         /* byte offset of the entry in that sector*/

    uint8_t  buf[FAT32_SECTOR_SIZE];
    uint16_t buf_used;
    bool     open;
} fat32_file_t;

/**
 * Read the MBR (or a superfloppy BPB) and validate the volume.
 * Must be called before anything else.
 */
fat32_err_t fat32_mount(fat32_fs_t *fs, const fat32_bdev_t *dev);

/**
 * Create a file in the root directory and open it for appending.
 *
 * @p name83 is exactly 11 characters, space padded, uppercase, no dot:
 * "LOG0001 TLM" is LOG0001.TLM.
 */
fat32_err_t fat32_create(fat32_fs_t *fs, fat32_file_t *f, const char *name83);

/** True if a root-directory entry with this name already exists. */
bool fat32_exists(fat32_fs_t *fs, const char *name83);

/**
 * Find the lowest unused NNNN and create PREFIX + NNNN + EXT.
 * @p prefix is up to 4 chars, @p ext exactly 3. Writes the chosen 8.3 name
 * into @p out_name if it is non-NULL (12 bytes: 8.3 plus terminator).
 */
fat32_err_t fat32_create_numbered(fat32_fs_t *fs, fat32_file_t *f,
                                  const char *prefix, const char *ext,
                                  char *out_name);

/** Append bytes. */
fat32_err_t fat32_write(fat32_file_t *f, const void *data, uint32_t len);

/**
 * Flush the partial sector and update the directory entry, so the file is
 * readable and correctly sized even if power is lost immediately afterwards.
 */
fat32_err_t fat32_sync(fat32_file_t *f);

/** Sync and mark closed. */
fat32_err_t fat32_close(fat32_file_t *f);

#ifdef __cplusplus
}
#endif

#endif /* FAT32_H */
