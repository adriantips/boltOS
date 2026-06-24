#pragma once
#include <stdint.h>
#include "blk.h"
/* ===========================================================================
 *  Read-only ext2 filesystem driver. Enough to mount an ext2 volume from any
 *  block device and read files/directories — so BoltOS can pull data off Linux-
 *  formatted USB sticks and disks, not just its own BoltFS/FAT32. Direct,
 *  single- and double-indirect block pointers are followed (covers any sane
 *  file on a small volume); writing is not supported.
 * ===========================================================================*/

#define EXT2_NAME_MAX 255

typedef struct {
    char     name[256];
    uint32_t inode;
    uint32_t size;
    int      is_dir;
} ext2_dirent;

int  ext2_mount(blkdev_t *dev);                 /* 0 on success, -1 if not ext2 */
int  ext2_mounted(void);
const char *ext2_volume_name(void);

/* List a directory; returns entry count (<0 on error), up to `max`. */
int  ext2_list(const char *path, ext2_dirent *out, int max);
/* Read a file's bytes into buf (up to cap); returns bytes read, or -1. */
int  ext2_read(const char *path, void *buf, uint32_t cap);
uint64_t ext2_total_bytes(void);
uint64_t ext2_free_bytes(void);
