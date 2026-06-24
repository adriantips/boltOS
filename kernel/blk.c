#include <stdint.h>
#include <stddef.h>
#include "blk.h"
#include "string.h"

/* ===========================================================================
 *  Generic block layer: a small registry plus transport-agnostic MBR helpers.
 * ===========================================================================*/

#define BLK_MAX 8
static blkdev_t g_dev[BLK_MAX];
static int      g_n;

int blk_register(const blkdev_t *dev) {
    if (g_n >= BLK_MAX || !dev || !dev->read) return -1;
    g_dev[g_n] = *dev;
    if (g_dev[g_n].sector_size == 0) g_dev[g_n].sector_size = BLK_SECTOR;
    return g_n++;
}
int       blk_count(void)      { return g_n; }
blkdev_t *blk_get(int i)       { return (i >= 0 && i < g_n) ? &g_dev[i] : 0; }

const char *blk_media(const blkdev_t *d) {
    if (!d) return "";
    if (d->name[0] == 'n') return "NVMe";        /* nvme* */
    return d->is_ssd ? "SSD" : "HDD";
}

blkdev_t *blk_fs_disk(void) {
    /* Prefer a non-boot NVMe namespace, then any non-boot SSD, then any
     * non-boot disk. Lets the FS land on the fastest persistent medium. */
    blkdev_t *ssd = 0, *any = 0;
    for (int i = 0; i < g_n; i++) {
        blkdev_t *d = &g_dev[i];
        if (d->is_boot) continue;
        if (d->name[0] == 'n') return d;         /* NVMe wins outright */
        if (d->is_ssd && !ssd) ssd = d;
        if (!any) any = d;
    }
    return ssd ? ssd : any;
}

int blk_read(blkdev_t *d, uint64_t lba, uint32_t count, void *buf) {
    if (!d || !d->read) return -1;
    if (lba + count > d->sectors) return -1;
    return d->read(d, lba, count, buf);
}
int blk_write(blkdev_t *d, uint64_t lba, uint32_t count, const void *buf) {
    if (!d || !d->write) return -1;
    if (lba + count > d->sectors) return -1;
    return d->write(d, lba, count, buf);
}

/* ---------------------------------------------------------------- MBR ------*/
static uint32_t rd_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

int blk_read_mbr(blkdev_t *d, blk_part out[4]) {
    if (!d || !out) return -1;
    uint8_t sec[BLK_SECTOR];
    if (blk_read(d, 0, 1, sec) != 0) return -1;
    if (sec[BLK_MBR_SIG_OFF] != 0x55 || sec[BLK_MBR_SIG_OFF + 1] != 0xAA) return -1;

    int n = 0;
    for (int i = 0; i < 4; i++) {
        const uint8_t *e = sec + BLK_MBR_PART_OFF + i * 16;
        blk_part *p = &out[i];
        memset(p, 0, sizeof *p);
        p->boot      = e[0];
        p->type      = e[4];
        p->lba_start = rd_le32(e + 8);
        p->sectors   = rd_le32(e + 12);
        if (p->type != 0 && p->sectors != 0) { p->present = 1; n++; }
    }
    return n;
}

int blk_write_mbr_single(blkdev_t *d, uint8_t type, uint64_t start) {
    if (!d || start == 0 || start >= d->sectors) return -1;
    uint8_t sec[BLK_SECTOR];
    memset(sec, 0, sizeof sec);

    uint64_t count = d->sectors - start;
    if (count > 0xFFFFFFFFull) count = 0xFFFFFFFFull;

    uint8_t *e = sec + BLK_MBR_PART_OFF;
    e[0] = 0x00;
    e[1] = 0xFE; e[2] = 0xFF; e[3] = 0xFF;
    e[4] = type;
    e[5] = 0xFE; e[6] = 0xFF; e[7] = 0xFF;
    wr_le32(e + 8,  (uint32_t)start);
    wr_le32(e + 12, (uint32_t)count);
    sec[BLK_MBR_SIG_OFF]     = 0x55;
    sec[BLK_MBR_SIG_OFF + 1] = 0xAA;

    return blk_write(d, 0, 1, sec);
}
