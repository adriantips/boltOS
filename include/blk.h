#pragma once
#include <stdint.h>

/* ===========================================================================
 *  Generic block-device layer.
 *
 *  Storage drivers (ATA PIO, NVMe, USB mass-storage, ...) register a blkdev
 *  with read/write callbacks; the filesystem and tools talk only to this
 *  interface, so the backing transport is interchangeable. Sectors are the
 *  device's logical block size (512 for everything BoltOS currently mounts).
 * ===========================================================================*/

#define BLK_SECTOR 512u

typedef struct blkdev {
    char     name[16];      /* "ata0", "nvme0n1", ...                        */
    char     model[48];     /* human model string                            */
    uint64_t sectors;       /* total addressable logical blocks              */
    uint32_t sector_size;   /* bytes per logical block (512)                 */
    int      is_ssd;        /* 1 = solid-state media                         */
    int      is_boot;       /* 1 = boot medium; FS avoids persisting here    */
    void    *drv;           /* driver-private handle (ata_dev*, nvme_ns*)    */
    int (*read )(struct blkdev *, uint64_t lba, uint32_t count, void *);
    int (*write)(struct blkdev *, uint64_t lba, uint32_t count, const void *);
} blkdev_t;

/* Register a device (the struct is copied into the table). Returns its index
 * or -1 if the table is full. */
int       blk_register(const blkdev_t *dev);
int       blk_count(void);
blkdev_t *blk_get(int i);

/* The disk the filesystem should persist to: the first registered non-boot
 * disk, preferring NVMe/SSD over spinning media. NULL -> FS stays RAM-only. */
blkdev_t *blk_fs_disk(void);

const char *blk_media(const blkdev_t *d);   /* "NVMe" / "SSD" / "HDD" */

int  blk_read (blkdev_t *d, uint64_t lba, uint32_t count, void *buf);
int  blk_write(blkdev_t *d, uint64_t lba, uint32_t count, const void *buf);

/* MBR partition table (LBA 0), transport-agnostic (drives blk_read/write). */
#define BLK_MBR_SIG_OFF   510
#define BLK_MBR_PART_OFF  446
#define BLK_PART_BOLTFS   0xBF

typedef struct blk_part {
    int      present;
    uint8_t  boot;
    uint8_t  type;
    uint64_t lba_start;
    uint64_t sectors;
} blk_part;

int blk_read_mbr(blkdev_t *d, blk_part out[4]);
int blk_write_mbr_single(blkdev_t *d, uint8_t type, uint64_t start);
