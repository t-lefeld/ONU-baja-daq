/**
 * fat32.c - Minimal append-only FAT32 writer. See fat32.h for scope.
 *
 * Everything is read and written byte-wise: FAT32 on-disk structures are
 * little-endian and unaligned (FstClusLO sits at offset 26 of a 32-byte
 * entry), so casting a struct over the sector buffer would be both an
 * alignment hazard on Cortex-M and wrong on a big-endian host test build.
 */

#include "fat32.h"
#include <string.h>

/* ------------------------------------------------------------------ */
/* Little-endian accessors                                             */
/* ------------------------------------------------------------------ */

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#define DIR_ENTRY_SIZE   32u
#define ATTR_READ_ONLY   0x01u
#define ATTR_VOLUME_ID   0x08u
#define ATTR_DIRECTORY   0x10u
#define ATTR_ARCHIVE     0x20u
#define ATTR_LONG_NAME   0x0Fu

#define FAT_EOC          0x0FFFFFF8u   /* >= this means end of chain */
#define FAT_EOC_WRITE    0x0FFFFFFFu
#define FAT_MASK         0x0FFFFFFFu

#define DIRENT_FREE      0xE5u
#define DIRENT_END       0x00u

/* ------------------------------------------------------------------ */
/* Sector helpers                                                      */
/* ------------------------------------------------------------------ */

static int dev_read(fat32_fs_t *fs, uint32_t lba, uint8_t *buf)
{
    return fs->dev->read(lba, buf);
}

static int dev_write(fat32_fs_t *fs, uint32_t lba, const uint8_t *buf)
{
    return fs->dev->write(lba, buf);
}

static uint32_t clus_to_lba(const fat32_fs_t *fs, uint32_t clus)
{
    return fs->data_lba + (clus - 2u) * fs->sec_per_clus;
}

/* ------------------------------------------------------------------ */
/* FAT access                                                          */
/* ------------------------------------------------------------------ */

static fat32_err_t fat_get(fat32_fs_t *fs, uint32_t clus, uint32_t *out)
{
    if (clus >= fs->total_clus)
    {
        return FAT32_ERR_PARAM;
    }

    uint32_t off = clus * 4u;
    uint32_t lba = fs->fat_lba + off / FAT32_SECTOR_SIZE;

    if (dev_read(fs, lba, fs->scratch) != 0)
    {
        return FAT32_ERR_IO;
    }

    *out = rd32(&fs->scratch[off % FAT32_SECTOR_SIZE]) & FAT_MASK;
    return FAT32_OK;
}

/**
 * Write one FAT entry, mirrored into every FAT copy.
 *
 * Mirroring matters: Windows validates FAT #1 against FAT #0 and will flag the
 * volume dirty if they disagree. The cost is one extra sector write per
 * allocation, which at one cluster per several minutes of logging is nothing.
 */
static fat32_err_t fat_set(fat32_fs_t *fs, uint32_t clus, uint32_t value)
{
    if (clus >= fs->total_clus)
    {
        return FAT32_ERR_PARAM;
    }

    uint32_t off = clus * 4u;
    uint32_t sec = off / FAT32_SECTOR_SIZE;
    uint32_t pos = off % FAT32_SECTOR_SIZE;

    for (uint8_t i = 0; i < fs->num_fats; i++)
    {
        uint32_t lba = fs->fat_lba + (uint32_t)i * fs->fat_sectors + sec;

        if (dev_read(fs, lba, fs->scratch) != 0)
        {
            return FAT32_ERR_IO;
        }

        /* Preserve the top 4 bits: they are reserved on FAT32 and some
           formatters use them. */
        uint32_t old = rd32(&fs->scratch[pos]);
        wr32(&fs->scratch[pos], (old & 0xF0000000u) | (value & FAT_MASK));

        if (dev_write(fs, lba, fs->scratch) != 0)
        {
            return FAT32_ERR_IO;
        }
    }

    return FAT32_OK;
}

/** Scan for a free cluster, starting from the allocation hint and wrapping. */
static fat32_err_t fat_alloc(fat32_fs_t *fs, uint32_t *out)
{
    uint32_t start = fs->next_free;

    if (start < 2u || start >= fs->total_clus)
    {
        start = 2u;
    }

    uint32_t clus = start;

    do {
        uint32_t val;
        fat32_err_t rc = fat_get(fs, clus, &val);

        if (rc != FAT32_OK)
        {
            return rc;
        }

        if (val == 0u)
        {
            rc = fat_set(fs, clus, FAT_EOC_WRITE);
            if (rc != FAT32_OK)
            {
                return rc;
            }

            fs->next_free = (clus + 1u < fs->total_clus) ? clus + 1u : 2u;
            *out = clus;
            return FAT32_OK;
        }

        clus++;
        if (clus >= fs->total_clus)
        {
            clus = 2u;
        }
    } while (clus != start);

    return FAT32_ERR_FULL;
}

static fat32_err_t fsinfo_update(fat32_fs_t *fs)
{
    if (fs->fsinfo_lba == 0u)
    {
        return FAT32_OK;
    }

    if (dev_read(fs, fs->fsinfo_lba, fs->scratch) != 0)
    {
        return FAT32_ERR_IO;
    }

    if (rd32(&fs->scratch[0]) != 0x41615252u || rd32(&fs->scratch[484]) != 0x61417272u)
    {
        return FAT32_OK;   /* not a valid FSInfo; leave it alone */
    }

    /*
     * Free count is set to "unknown" rather than tracked. Reporting a stale
     * count is worse than reporting none: the OS trusts it and then disagrees
     * with what it finds. 0xFFFFFFFF is the documented value meaning
     * "recompute me", and every OS handles it correctly.
     */
    wr32(&fs->scratch[488], 0xFFFFFFFFu);
    wr32(&fs->scratch[492], fs->next_free);

    return dev_write(fs, fs->fsinfo_lba, fs->scratch) == 0 ? FAT32_OK : FAT32_ERR_IO;
}

/* ------------------------------------------------------------------ */
/* Mount                                                               */
/* ------------------------------------------------------------------ */

static bool looks_like_bpb(const uint8_t *s)
{
    uint16_t bps = rd16(&s[11]);
    uint8_t  spc = s[13];

    if (bps != FAT32_SECTOR_SIZE)
    {
        return false;
    }

    /* sectors per cluster must be a power of two, 1..128 */
    if (spc == 0u || (spc & (uint8_t)(spc - 1u)) != 0u)
    {
        return false;
    }

    /* FAT32 has zero root entries and zero 16-bit total sectors */
    return rd16(&s[17]) == 0u && rd16(&s[22]) == 0u;
}

fat32_err_t fat32_mount(fat32_fs_t *fs, const fat32_bdev_t *dev)
{
    memset(fs, 0, sizeof(*fs));
    fs->dev = dev;

    if (dev_read(fs, 0, fs->scratch) != 0)
    {
        return FAT32_ERR_IO;
    }

    if (rd16(&fs->scratch[510]) != 0xAA55u)
    {
        return FAT32_ERR_NOT_FAT32;
    }

    uint32_t part_lba = 0u;

    if (!looks_like_bpb(fs->scratch))
    {
        /*
         * Not a bare BPB, so treat sector 0 as an MBR. Cards from a camera or
         * formatted by Windows are partitioned; cards formatted by some Linux
         * tools are "superfloppy" with the BPB at sector 0. Both appear in the
         * wild, so handle both rather than guessing.
         */
        bool found = false;

        for (int i = 0; i < 4 && !found; i++)
        {
            const uint8_t *e = &fs->scratch[446 + i * 16];
            uint8_t type = e[4];

            if (type == 0x0Bu || type == 0x0Cu ||   /* FAT32 / FAT32 LBA */
                type == 0x07u)                       /* sometimes exFAT/NTFS id */
            {
                part_lba = rd32(&e[8]);
                found = (part_lba != 0u);
            }
        }

        if (!found)
        {
            return FAT32_ERR_NOT_FAT32;
        }

        if (dev_read(fs, part_lba, fs->scratch) != 0)
        {
            return FAT32_ERR_IO;
        }

        if (!looks_like_bpb(fs->scratch))
        {
            return FAT32_ERR_NOT_FAT32;
        }
    }

    const uint8_t *b = fs->scratch;

    uint16_t bytes_per_sec  = rd16(&b[11]);
    uint8_t  sec_per_clus   = b[13];
    uint16_t reserved_sec   = rd16(&b[14]);
    uint8_t  num_fats       = b[16];
    uint32_t total_sec32    = rd32(&b[32]);
    uint32_t fat_size32     = rd32(&b[36]);
    uint32_t root_clus      = rd32(&b[44]);
    uint16_t fsinfo_sec     = rd16(&b[48]);

    if (bytes_per_sec != FAT32_SECTOR_SIZE)
    {
        return FAT32_ERR_BAD_SECTOR_SIZE;
    }

    if (fat_size32 == 0u || num_fats == 0u || sec_per_clus == 0u || root_clus < 2u)
    {
        return FAT32_ERR_NOT_FAT32;
    }

    /*
     * FAT32 is identified by the BPB shape, not by cluster count. The spec's
     * 65525-cluster threshold describes what a formatter should choose, not
     * what a driver must reject: mkfs.vfat -F 32 will happily produce a valid
     * FAT32 volume below it, and refusing to mount one means refusing a card
     * the OS reads perfectly well.
     *
     * A FAT32 BPB always has zero 16-bit FAT size and zero root entries; those
     * fields are where FAT12/16 keep real values, so this is unambiguous.
     */
    if (rd16(&b[22]) != 0u || rd16(&b[17]) != 0u)
    {
        return FAT32_ERR_NOT_FAT32;
    }

    fs->part_lba     = part_lba;
    fs->sec_per_clus = sec_per_clus;
    fs->num_fats     = num_fats;
    fs->fat_lba      = part_lba + reserved_sec;
    fs->fat_sectors  = fat_size32;
    fs->data_lba     = fs->fat_lba + (uint32_t)num_fats * fat_size32;
    fs->root_clus    = root_clus;
    fs->fsinfo_lba   = (fsinfo_sec != 0u && fsinfo_sec != 0xFFFFu)
                     ? part_lba + fsinfo_sec : 0u;

    uint32_t meta_sectors = reserved_sec + (uint32_t)num_fats * fat_size32;

    if (total_sec32 <= meta_sectors)
    {
        return FAT32_ERR_NOT_FAT32;
    }

    uint32_t data_sectors = total_sec32 - meta_sectors;
    fs->total_clus = data_sectors / sec_per_clus + 2u;

    /*
     * Do not trust the FAT to be larger than the cluster count implies. A
     * malformed BPB could otherwise let fat_get() index past the FAT area and
     * read directory or file data as chain pointers.
     */
    uint32_t max_clus_in_fat = (fat_size32 * FAT32_SECTOR_SIZE) / 4u;
    if (fs->total_clus > max_clus_in_fat)
    {
        fs->total_clus = max_clus_in_fat;
    }

    if (fs->total_clus <= 2u)
    {
        return FAT32_ERR_NOT_FAT32;
    }

    fs->next_free = 2u;

    if (fs->fsinfo_lba != 0u)
    {
        if (dev_read(fs, fs->fsinfo_lba, fs->scratch) == 0 &&
            rd32(&fs->scratch[0]) == 0x41615252u &&
            rd32(&fs->scratch[484]) == 0x61417272u)
        {
            uint32_t hint = rd32(&fs->scratch[492]);
            if (hint >= 2u && hint < fs->total_clus)
            {
                fs->next_free = hint;
            }
        }
    }

    fs->mounted = true;
    return FAT32_OK;
}

/* ------------------------------------------------------------------ */
/* Directory                                                           */
/* ------------------------------------------------------------------ */

/**
 * Walk the root directory looking for @p name83 and/or a free slot.
 *
 * One pass finds both, because we have to scan the whole directory to prove a
 * name is absent anyway. @p free_lba is only written if a free slot is seen.
 */
static fat32_err_t dir_scan(fat32_fs_t *fs, const char *name83,
                            bool *found, uint32_t *free_lba, uint16_t *free_off)
{
    *found = false;

    bool     have_free = false;
    uint32_t clus      = fs->root_clus;
    uint32_t last_clus = clus;

    while (clus >= 2u && clus < FAT_EOC)
    {
        for (uint32_t s = 0; s < fs->sec_per_clus; s++)
        {
            uint32_t lba = clus_to_lba(fs, clus) + s;

            if (dev_read(fs, lba, fs->scratch) != 0)
            {
                return FAT32_ERR_IO;
            }

            for (uint16_t off = 0; off < FAT32_SECTOR_SIZE; off += DIR_ENTRY_SIZE)
            {
                uint8_t first = fs->scratch[off];
                uint8_t attr  = fs->scratch[off + 11];

                if (first == DIRENT_END)
                {
                    /* End of directory. Everything past here is free. */
                    if (!have_free)
                    {
                        *free_lba = lba;
                        *free_off = off;
                        have_free = true;
                    }
                    return FAT32_OK;
                }

                if (first == DIRENT_FREE)
                {
                    if (!have_free)
                    {
                        *free_lba = lba;
                        *free_off = off;
                        have_free = true;
                    }
                    continue;
                }

                /* Skip long-filename entries and the volume label. */
                if ((attr & ATTR_LONG_NAME) == ATTR_LONG_NAME ||
                    (attr & ATTR_VOLUME_ID) != 0u)
                {
                    continue;
                }

                if (name83 != NULL &&
                    memcmp(&fs->scratch[off], name83, 11) == 0)
                {
                    *found = true;
                    return FAT32_OK;
                }
            }
        }

        last_clus = clus;

        uint32_t next;
        fat32_err_t rc = fat_get(fs, clus, &next);
        if (rc != FAT32_OK)
        {
            return rc;
        }
        clus = next;
    }

    if (have_free)
    {
        return FAT32_OK;
    }

    /*
     * Directory is completely full and every cluster is used. Extend it by one
     * cluster and zero the whole thing - an unzeroed cluster would be read as
     * random directory entries.
     */
    uint32_t newc;
    fat32_err_t rc = fat_alloc(fs, &newc);
    if (rc != FAT32_OK)
    {
        return (rc == FAT32_ERR_FULL) ? FAT32_ERR_DIR_FULL : rc;
    }

    uint8_t zero[FAT32_SECTOR_SIZE];
    memset(zero, 0, sizeof(zero));

    for (uint32_t s = 0; s < fs->sec_per_clus; s++)
    {
        if (dev_write(fs, clus_to_lba(fs, newc) + s, zero) != 0)
        {
            return FAT32_ERR_IO;
        }
    }

    rc = fat_set(fs, last_clus, newc);
    if (rc != FAT32_OK)
    {
        return rc;
    }

    *free_lba = clus_to_lba(fs, newc);
    *free_off = 0u;
    return FAT32_OK;
}

bool fat32_exists(fat32_fs_t *fs, const char *name83)
{
    bool     found = false;
    uint32_t lba   = 0u;
    uint16_t off   = 0u;

    if (!fs->mounted)
    {
        return false;
    }

    if (dir_scan(fs, name83, &found, &lba, &off) != FAT32_OK)
    {
        return false;
    }

    return found;
}

/* ------------------------------------------------------------------ */
/* File create / write                                                 */
/* ------------------------------------------------------------------ */

static fat32_err_t dir_write_entry(fat32_file_t *f)
{
    fat32_fs_t *fs = f->fs;

    if (dev_read(fs, f->dir_lba, fs->scratch) != 0)
    {
        return FAT32_ERR_IO;
    }

    uint8_t *e = &fs->scratch[f->dir_off];

    wr16(&e[20], (uint16_t)(f->first_clus >> 16));   /* FstClusHI */
    wr16(&e[26], (uint16_t)(f->first_clus & 0xFFFFu)); /* FstClusLO */
    wr32(&e[28], f->size);                            /* FileSize  */

    return dev_write(fs, f->dir_lba, fs->scratch) == 0 ? FAT32_OK : FAT32_ERR_IO;
}

fat32_err_t fat32_create(fat32_fs_t *fs, fat32_file_t *f, const char *name83)
{
    if (!fs->mounted || name83 == NULL)
    {
        return FAT32_ERR_PARAM;
    }

    bool     found = false;
    uint32_t lba   = 0u;
    uint16_t off   = 0u;

    fat32_err_t rc = dir_scan(fs, name83, &found, &lba, &off);
    if (rc != FAT32_OK)
    {
        return rc;
    }

    if (found)
    {
        return FAT32_ERR_EXISTS;
    }

    if (lba == 0u)
    {
        return FAT32_ERR_DIR_FULL;
    }

    uint32_t clus;
    rc = fat_alloc(fs, &clus);
    if (rc != FAT32_OK)
    {
        return rc;
    }

    memset(f, 0, sizeof(*f));
    f->fs         = fs;
    f->first_clus = clus;
    f->clus       = clus;
    f->dir_lba    = lba;
    f->dir_off    = off;
    f->open       = true;

    /* Build the 32-byte directory entry. */
    if (dev_read(fs, lba, fs->scratch) != 0)
    {
        return FAT32_ERR_IO;
    }

    uint8_t *e = &fs->scratch[off];
    memset(e, 0, DIR_ENTRY_SIZE);
    memcpy(e, name83, 11);
    e[11] = ATTR_ARCHIVE;

    /*
     * No RTC on this board, so timestamps are fixed rather than wrong-but-
     * plausible. 1 Jan 2000, 00:00:00. Frame timestamps live inside the data;
     * relying on a directory timestamp that silently reads as 1980 would be
     * worse than an obviously synthetic one.
     */
    wr16(&e[14], 0u);          /* CrtTime  */
    wr16(&e[16], 0x2821u);     /* CrtDate  = 2000-01-01 */
    wr16(&e[18], 0x2821u);     /* LstAccDate */
    wr16(&e[22], 0u);          /* WrtTime  */
    wr16(&e[24], 0x2821u);     /* WrtDate  */
    wr16(&e[20], (uint16_t)(clus >> 16));
    wr16(&e[26], (uint16_t)(clus & 0xFFFFu));
    wr32(&e[28], 0u);

    if (dev_write(fs, lba, fs->scratch) != 0)
    {
        return FAT32_ERR_IO;
    }

    /*
     * If we consumed the end-of-directory marker, the next slot must become the
     * new marker. Skipping this leaves stale bytes that the OS reads as a
     * garbage entry.
     */
    uint16_t next_off = (uint16_t)(off + DIR_ENTRY_SIZE);
    if (next_off < FAT32_SECTOR_SIZE && fs->scratch[next_off] != DIRENT_END &&
        fs->scratch[next_off] != DIRENT_FREE)
    {
        /* Something already there - nothing to do. */
    }
    else if (next_off < FAT32_SECTOR_SIZE)
    {
        fs->scratch[next_off] = DIRENT_END;
        if (dev_write(fs, lba, fs->scratch) != 0)
        {
            return FAT32_ERR_IO;
        }
    }

    return fsinfo_update(fs);
}

fat32_err_t fat32_create_numbered(fat32_fs_t *fs, fat32_file_t *f,
                                  const char *prefix, const char *ext,
                                  char *out_name)
{
    if (!fs->mounted || prefix == NULL || ext == NULL)
    {
        return FAT32_ERR_PARAM;
    }

    size_t plen = strlen(prefix);
    if (plen == 0u || plen > 4u || strlen(ext) != 3u)
    {
        return FAT32_ERR_PARAM;
    }

    char name[12];

    for (uint32_t i = 1u; i <= 9999u; i++)
    {
        memset(name, ' ', 11);
        name[11] = '\0';
        memcpy(name, prefix, plen);

        /*
         * Always four digits, then space padding out to the 8-character field.
         * Filling the whole field with digits instead would turn "LOG" into
         * LOG00001.TLM, which sorts and reads worse and is not what the
         * documentation elsewhere in this repo promises.
         */
        uint32_t v = i;
        for (uint32_t d = 0; d < 4u; d++)
        {
            name[plen + 3u - d] = (char)('0' + (v % 10u));
            v /= 10u;
        }

        memcpy(&name[8], ext, 3);

        if (!fat32_exists(fs, name))
        {
            fat32_err_t rc = fat32_create(fs, f, name);

            if (rc == FAT32_OK && out_name != NULL)
            {
                /* Present it as 8.3 with a dot, for logging. */
                int o = 0;
                for (int c = 0; c < 8 && name[c] != ' '; c++) out_name[o++] = name[c];
                out_name[o++] = '.';
                for (int c = 8; c < 11 && name[c] != ' '; c++) out_name[o++] = name[c];
                out_name[o] = '\0';
            }

            return rc;
        }
    }

    return FAT32_ERR_DIR_FULL;
}

/** Move to the next sector, allocating and linking a cluster when needed. */
static fat32_err_t advance_sector(fat32_file_t *f)
{
    fat32_fs_t *fs = f->fs;

    f->sec_in_clus++;

    if (f->sec_in_clus < fs->sec_per_clus)
    {
        return FAT32_OK;
    }

    uint32_t next;
    fat32_err_t rc = fat_alloc(fs, &next);
    if (rc != FAT32_OK)
    {
        return rc;
    }

    rc = fat_set(fs, f->clus, next);
    if (rc != FAT32_OK)
    {
        return rc;
    }

    f->clus        = next;
    f->sec_in_clus = 0u;

    return fsinfo_update(fs);
}

fat32_err_t fat32_write(fat32_file_t *f, const void *data, uint32_t len)
{
    if (f == NULL || !f->open)
    {
        return FAT32_ERR_NOT_OPEN;
    }

    const uint8_t *src = (const uint8_t *)data;

    while (len > 0u)
    {
        uint32_t space = FAT32_SECTOR_SIZE - f->buf_used;
        uint32_t n     = (len < space) ? len : space;

        memcpy(&f->buf[f->buf_used], src, n);

        f->buf_used = (uint16_t)(f->buf_used + n);
        f->size    += n;
        src        += n;
        len        -= n;

        if (f->buf_used == FAT32_SECTOR_SIZE)
        {
            uint32_t lba = clus_to_lba(f->fs, f->clus) + f->sec_in_clus;

            if (dev_write(f->fs, lba, f->buf) != 0)
            {
                return FAT32_ERR_IO;
            }

            /* Zeroed so a later partial flush of this buffer writes clean
               padding rather than whatever was here before. */
            memset(f->buf, 0, sizeof(f->buf));
            f->buf_used = 0u;

            fat32_err_t rc = advance_sector(f);
            if (rc != FAT32_OK)
            {
                return rc;
            }
        }
    }

    return FAT32_OK;
}

fat32_err_t fat32_sync(fat32_file_t *f)
{
    if (f == NULL || !f->open)
    {
        return FAT32_ERR_NOT_OPEN;
    }

    if (f->buf_used > 0u)
    {
        /*
         * Write the partial sector without advancing. The in-RAM buffer keeps
         * its contents, so the next write continues filling the same sector and
         * simply rewrites it. Costs one redundant sector write per sync, which
         * is the price of the file being valid at any moment.
         */
        uint32_t lba = clus_to_lba(f->fs, f->clus) + f->sec_in_clus;

        if (dev_write(f->fs, lba, f->buf) != 0)
        {
            return FAT32_ERR_IO;
        }
    }

    return dir_write_entry(f);
}

fat32_err_t fat32_close(fat32_file_t *f)
{
    if (f == NULL || !f->open)
    {
        return FAT32_ERR_NOT_OPEN;
    }

    fat32_err_t rc = fat32_sync(f);
    f->open = false;
    return rc;
}
