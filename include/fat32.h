#pragma once
#include <stdint.h>
#include "blk.h"

/* ===========================================================================
 *  FAT32 filesystem: mount/format a block device and read/write files.
 *
 *  Standard on-disk format, so volumes are interchangeable with Windows/Linux
 *  and USB sticks. Long File Names are parsed on read; writes emit 8.3 short
 *  names (uppercased/truncated). One volume mounted at a time.
 * ===========================================================================*/

#define FAT_ATTR_DIR    0x10
#define FAT_ATTR_VOLID  0x08

typedef struct {
    char     name[256];      /* long name if present, else 8.3 */
    uint32_t size;
    uint8_t  attr;
    uint32_t cluster;
} fat_dirent;

int  fat32_mount(blkdev_t *dev);          /* 0 on success */
int  fat32_mounted(void);
int  fat32_format(blkdev_t *dev, const char *label);

/* List directory at absolute path ("/", "/sub"). Returns count, fills up to max. */
int  fat32_list(const char *path, fat_dirent *out, int max);

/* Read whole file into buf (up to max). Returns bytes read, <0 on error. */
int  fat32_read(const char *path, void *buf, uint32_t max);

/* Create/overwrite a file with the given bytes. Returns 0 on success. */
int  fat32_write(const char *path, const void *data, uint32_t len);

/* Create a directory. Returns 0 on success. */
int  fat32_mkdir(const char *path);

const char *fat32_label(void);
uint64_t    fat32_free_bytes(void);
uint64_t    fat32_total_bytes(void);
