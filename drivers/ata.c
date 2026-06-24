#include <stdint.h>
#include <stddef.h>
#include "ata.h"
#include "blk.h"
#include "io.h"
#include "string.h"
#include "kprintf.h"

/* ===========================================================================
 *  ATA / IDE PIO block driver.  See include/ata.h for the model.
 * ===========================================================================*/

/* command-block register offsets from io_base */
#define R_DATA     0
#define R_ERR      1     /* read: error           */
#define R_FEAT     1     /* write: features       */
#define R_SECCNT   2
#define R_LBA0     3
#define R_LBA1     4
#define R_LBA2     5
#define R_DRIVE    6
#define R_STATUS   7     /* read: status          */
#define R_CMD      7     /* write: command        */

/* status register bits */
#define ST_ERR   0x01
#define ST_DRQ   0x08
#define ST_DF    0x20
#define ST_DRDY  0x40
#define ST_BSY   0x80

/* commands */
#define CMD_READ_PIO      0x20
#define CMD_READ_PIO_EXT  0x24
#define CMD_WRITE_PIO     0x30
#define CMD_WRITE_PIO_EXT 0x34
#define CMD_CACHE_FLUSH   0xE7
#define CMD_IDENTIFY      0xEC

#define ATA_MAX 4
static ata_dev g_devs[ATA_MAX];     /* probe-order table of present disks */
static int     g_count;
static int     g_inited;

/* Channel layout: (io_base, ctrl_base) per ATA channel. */
struct chan { uint16_t io, ctrl; };
static const struct chan CHANNELS[2] = {
    { 0x1F0, 0x3F6 },   /* primary   */
    { 0x170, 0x376 },   /* secondary */
};

/* 400 ns settle: four alternate-status reads. */
static void ata_delay(uint16_t ctrl) {
    inb(ctrl); inb(ctrl); inb(ctrl); inb(ctrl);
}

static void ata_select(uint16_t io, uint16_t ctrl, uint8_t slave, uint8_t lba_high4) {
    outb(io + R_DRIVE, (uint8_t)(0xE0 | (slave << 4) | (lba_high4 & 0x0F)));
    ata_delay(ctrl);
}

static int wait_clear_bsy(uint16_t io) {
    for (uint32_t t = 0; t < 4000000u; t++) {
        uint8_t s = inb(io + R_STATUS);
        if (!(s & ST_BSY)) return (s & (ST_ERR | ST_DF)) ? -1 : 0;
    }
    return -1;
}

static int wait_drq(uint16_t io) {
    for (uint32_t t = 0; t < 4000000u; t++) {
        uint8_t s = inb(io + R_STATUS);
        if (s & (ST_ERR | ST_DF)) return -1;
        if (!(s & ST_BSY) && (s & ST_DRQ)) return 0;
    }
    return -1;
}

/* ---------------------------------------------------------------------------
 *  IDENTIFY-based probe of one slot.
 * ---------------------------------------------------------------------------*/
static int probe_one(uint16_t io, uint16_t ctrl, uint8_t slave, ata_dev *out) {
    /* disable device IRQs (nIEN) - we poll */
    outb(ctrl, 0x02);

    ata_select(io, ctrl, slave, 0);
    outb(io + R_SECCNT, 0);
    outb(io + R_LBA0,   0);
    outb(io + R_LBA1,   0);
    outb(io + R_LBA2,   0);
    outb(io + R_CMD, CMD_IDENTIFY);

    uint8_t status = inb(io + R_STATUS);
    if (status == 0) return 0;                 /* nothing on this slot */

    if (wait_clear_bsy(io) != 0) return 0;

    /* ATAPI / SATA devices answer IDENTIFY with a signature in LBA1/LBA2
     * (CD-ROM: 0x14/0xEB). Those are not plain ATA fixed disks - skip them. */
    if (inb(io + R_LBA1) != 0 || inb(io + R_LBA2) != 0) return 0;

    if (wait_drq(io) != 0) return 0;

    uint16_t id[256];
    for (int i = 0; i < 256; i++) id[i] = inw(io + R_DATA);

    memset(out, 0, sizeof(*out));
    out->present   = 1;
    out->io_base   = io;
    out->ctrl_base = ctrl;
    out->slave     = slave;
    out->lba48     = (id[83] & (1 << 10)) ? 1 : 0;

    uint64_t s28 = (uint64_t)id[60] | ((uint64_t)id[61] << 16);
    uint64_t s48 = (uint64_t)id[100] | ((uint64_t)id[101] << 16) |
                   ((uint64_t)id[102] << 32) | ((uint64_t)id[103] << 48);
    out->sectors = (out->lba48 && s48) ? s48 : s28;

    /* word 217: nominal media rotation rate. 1 == non-rotating (SSD). */
    out->is_ssd = (id[217] == 1) ? 1 : 0;

    /* model: words 27..46, ASCII, byte-swapped within each word */
    for (int i = 0; i < 20; i++) {
        out->model[i * 2]     = (char)(id[27 + i] >> 8);
        out->model[i * 2 + 1] = (char)(id[27 + i] & 0xFF);
    }
    out->model[40] = 0;
    for (int i = 39; i >= 0 && (out->model[i] == ' ' || out->model[i] == 0); i--)
        out->model[i] = 0;                     /* trim trailing pad */

    return 1;
}

static void ata_blk_register(ata_dev *d, int idx);

void ata_init(void) {
    if (g_inited) return;
    g_inited = 1;
    g_count  = 0;

    for (int c = 0; c < 2; c++)
        for (uint8_t s = 0; s < 2 && g_count < ATA_MAX; s++) {
            ata_dev d;
            if (probe_one(CHANNELS[c].io, CHANNELS[c].ctrl, s, &d))
                g_devs[g_count++] = d;
        }

    kprintf("[ok] ATA: %d disk%s\n", g_count, g_count == 1 ? "" : "s");
    for (int i = 0; i < g_count; i++) {
        ata_dev *d = &g_devs[i];
        uint64_t mb = (d->sectors * ATA_SECTOR) / (1024 * 1024);
        kprintf("     [%d] %s  %luMiB  %s%s\n", i,
                ata_media(d), (unsigned long)mb, d->lba48 ? "lba48 " : "lba28 ",
                d->model[0] ? d->model : "(disk)");
        ata_blk_register(d, i);
    }
}

/* --- block-layer integration --------------------------------------------- */
static int ata_blk_read(blkdev_t *b, uint64_t lba, uint32_t count, void *buf) {
    return ata_read((ata_dev *)b->drv, lba, count, buf);
}
static int ata_blk_write(blkdev_t *b, uint64_t lba, uint32_t count, const void *buf) {
    return ata_write((ata_dev *)b->drv, lba, count, buf);
}
static void ata_blk_register(ata_dev *d, int idx) {
    blkdev_t b;
    memset(&b, 0, sizeof b);
    b.sectors     = d->sectors;
    b.sector_size = ATA_SECTOR;
    b.is_ssd      = d->is_ssd;
    /* primary master (0x1F0, master) is the boot image disk */
    b.is_boot     = (d->io_base == 0x1F0 && d->slave == 0);
    b.drv         = d;
    b.read        = ata_blk_read;
    b.write       = ata_blk_write;
    b.name[0] = 'a'; b.name[1] = 't'; b.name[2] = 'a';
    b.name[3] = (char)('0' + idx); b.name[4] = 0;
    strncpy(b.model, d->model[0] ? d->model : "ATA disk", sizeof b.model - 1);
    blk_register(&b);
}

int      ata_count(void)        { return g_count; }
ata_dev *ata_get(int i)         { return (i >= 0 && i < g_count) ? &g_devs[i] : 0; }
const char *ata_media(const ata_dev *d) { return d && d->is_ssd ? "SSD" : "HDD"; }

ata_dev *ata_fs_disk(void) {
    /* first present disk that is not the boot disk (primary master) */
    for (int i = 0; i < g_count; i++) {
        ata_dev *d = &g_devs[i];
        if (d->io_base == 0x1F0 && d->slave == 0) continue;
        return d;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 *  Single-sector PIO transfer. One command per sector keeps the state machine
 *  trivial and bulletproof for the small images this FS uses.
 * ---------------------------------------------------------------------------*/
static int rw_sector(ata_dev *d, uint64_t lba, void *buf, int write) {
    uint16_t io = d->io_base, ctrl = d->ctrl_base;

    if (wait_clear_bsy(io) != 0) return -1;

    int use48 = d->lba48 && (lba >= (1ull << 28));

    if (use48) {
        outb(io + R_DRIVE, (uint8_t)(0x40 | (d->slave << 4)));
        ata_delay(ctrl);
        outb(io + R_SECCNT, 0);                          /* count high = 0 */
        outb(io + R_LBA0, (uint8_t)(lba >> 24));
        outb(io + R_LBA1, (uint8_t)(lba >> 32));
        outb(io + R_LBA2, (uint8_t)(lba >> 40));
        outb(io + R_SECCNT, 1);                          /* count low = 1  */
        outb(io + R_LBA0, (uint8_t)(lba));
        outb(io + R_LBA1, (uint8_t)(lba >> 8));
        outb(io + R_LBA2, (uint8_t)(lba >> 16));
        outb(io + R_CMD, write ? CMD_WRITE_PIO_EXT : CMD_READ_PIO_EXT);
    } else {
        ata_select(io, ctrl, d->slave, (uint8_t)(lba >> 24));
        outb(io + R_FEAT,   0);
        outb(io + R_SECCNT, 1);
        outb(io + R_LBA0, (uint8_t)(lba));
        outb(io + R_LBA1, (uint8_t)(lba >> 8));
        outb(io + R_LBA2, (uint8_t)(lba >> 16));
        outb(io + R_CMD, write ? CMD_WRITE_PIO : CMD_READ_PIO);
    }

    if (wait_drq(io) != 0) return -1;

    uint16_t *p = (uint16_t *)buf;
    if (write) {
        for (int i = 0; i < 256; i++) outw(io + R_DATA, p[i]);
        outb(io + R_CMD, CMD_CACHE_FLUSH);
        if (wait_clear_bsy(io) != 0) return -1;
    } else {
        for (int i = 0; i < 256; i++) p[i] = inw(io + R_DATA);
        ata_delay(ctrl);
    }
    return 0;
}

static int rw(ata_dev *d, uint64_t lba, uint32_t count, uint8_t *buf, int write) {
    if (!d || !d->present) return -1;
    if (lba + count > d->sectors) return -1;
    for (uint32_t i = 0; i < count; i++)
        if (rw_sector(d, lba + i, buf + i * ATA_SECTOR, write) != 0) return -1;
    return 0;
}

int ata_read (ata_dev *d, uint64_t lba, uint32_t count, void *buf) {
    return rw(d, lba, count, (uint8_t *)buf, 0);
}
int ata_write(ata_dev *d, uint64_t lba, uint32_t count, const void *buf) {
    return rw(d, lba, count, (uint8_t *)(uintptr_t)buf, 1);
}

/* ---------------------------------------------------------------------------
 *  MBR partition table.  Entries are little-endian; lba_start/sectors are the
 *  32-bit LBA fields at offsets 8 and 12 of each 16-byte entry.
 * ---------------------------------------------------------------------------*/
static uint32_t rd_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int ata_read_mbr(ata_dev *d, ata_part out[4]) {
    if (!d || !d->present || !out) return -1;

    uint8_t sec[ATA_SECTOR];
    if (ata_read(d, 0, 1, sec) != 0) return -1;
    if (sec[MBR_SIG_OFF] != 0x55 || sec[MBR_SIG_OFF + 1] != 0xAA) return -1;

    int n = 0;
    for (int i = 0; i < 4; i++) {
        const uint8_t *e = sec + MBR_PART_OFF + i * 16;
        ata_part *p = &out[i];
        memset(p, 0, sizeof *p);
        p->boot      = e[0];
        p->type      = e[4];
        p->lba_start = rd_le32(e + 8);
        p->sectors   = rd_le32(e + 12);
        if (p->type != 0 && p->sectors != 0) { p->present = 1; n++; }
    }
    return n;
}

static void wr_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

int ata_write_mbr_single(ata_dev *d, uint8_t type, uint64_t start) {
    if (!d || !d->present || start == 0 || start >= d->sectors) return -1;

    uint8_t sec[ATA_SECTOR];
    memset(sec, 0, sizeof sec);

    uint64_t count = d->sectors - start;
    if (count > 0xFFFFFFFFull) count = 0xFFFFFFFFull;   /* 32-bit LBA field cap */

    uint8_t *e = sec + MBR_PART_OFF;        /* first partition entry */
    e[0] = 0x00;                            /* not bootable          */
    e[1] = 0xFE; e[2] = 0xFF; e[3] = 0xFF;  /* CHS start: unused/max */
    e[4] = type;
    e[5] = 0xFE; e[6] = 0xFF; e[7] = 0xFF;  /* CHS end:   unused/max */
    wr_le32(e + 8,  (uint32_t)start);
    wr_le32(e + 12, (uint32_t)count);

    sec[MBR_SIG_OFF]     = 0x55;
    sec[MBR_SIG_OFF + 1] = 0xAA;

    return ata_write(d, 0, 1, sec);
}
