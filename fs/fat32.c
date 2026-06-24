#include <stdint.h>
#include <stddef.h>
#include "fat32.h"
#include "blk.h"
#include "string.h"
#include "kprintf.h"

/* ===========================================================================
 *  FAT32 implementation. Synchronous, single-volume, one open operation at a
 *  time. Cluster I/O goes through the generic block layer (blk_read/write).
 * ===========================================================================*/

#define SECSZ 512u
#define MAX_CLUSTER_BYTES (64 * 1024)
#define EOC 0x0FFFFFF8u

static struct {
    int       mounted;
    blkdev_t *dev;
    uint32_t  bytes_per_sec;
    uint32_t  sec_per_clus;
    uint32_t  reserved;
    uint32_t  num_fats;
    uint32_t  fat_size;       /* sectors per FAT */
    uint32_t  root_clus;
    uint32_t  fat_start;      /* LBA of FAT #0 */
    uint32_t  data_start;     /* LBA of cluster 2 */
    uint32_t  total_clus;
    uint32_t  clus_bytes;
    char      label[12];
} fs;

static uint8_t  secbuf[SECSZ];
static uint8_t  clusbuf[MAX_CLUSTER_BYTES];

int fat32_mounted(void) { return fs.mounted; }
const char *fat32_label(void) { return fs.label; }

/* ---- low level ---------------------------------------------------------- */
static int rd_sec(uint32_t lba, void *b) { return blk_read(fs.dev, lba, 1, b); }
static int wr_sec(uint32_t lba, const void *b) { return blk_write(fs.dev, lba, 1, b); }

static uint32_t clus_lba(uint32_t c) { return fs.data_start + (c - 2) * fs.sec_per_clus; }

static uint32_t fat_get(uint32_t clus) {
    uint32_t off = clus * 4;
    uint32_t sec = fs.fat_start + off / SECSZ;
    uint32_t idx = off % SECSZ;
    if (rd_sec(sec, secbuf) != 0) return EOC;
    return (*(uint32_t *)(secbuf + idx)) & 0x0FFFFFFFu;
}
static int fat_set(uint32_t clus, uint32_t val) {
    uint32_t off = clus * 4;
    uint32_t idx = off % SECSZ;
    for (uint32_t f = 0; f < fs.num_fats; f++) {
        uint32_t sec = fs.fat_start + f * fs.fat_size + off / SECSZ;
        if (rd_sec(sec, secbuf) != 0) return -1;
        uint32_t *e = (uint32_t *)(secbuf + idx);
        *e = (*e & 0xF0000000u) | (val & 0x0FFFFFFFu);
        if (wr_sec(sec, secbuf) != 0) return -1;
    }
    return 0;
}

/* allocate one free cluster, mark EOC. returns cluster# or 0. */
static uint32_t fat_alloc(void) {
    for (uint32_t c = 2; c < fs.total_clus + 2; c++) {
        if (fat_get(c) == 0) { if (fat_set(c, EOC) != 0) return 0; return c; }
    }
    return 0;
}

static int read_cluster(uint32_t c, void *buf) {
    return blk_read(fs.dev, clus_lba(c), fs.sec_per_clus, buf);
}
static int write_cluster(uint32_t c, const void *buf) {
    return blk_write(fs.dev, clus_lba(c), fs.sec_per_clus, buf);
}

/* ---- name helpers ------------------------------------------------------- */
static void to_short(const char *name, char out[11]) {
    memset(out, ' ', 11);
    int i = 0, o = 0;
    /* base name */
    const char *dot = 0;
    for (const char *p = name; *p; p++) if (*p == '.') dot = p;
    while (name[i] && (dot ? &name[i] != dot : 1) && o < 8) {
        char c = name[i++];
        if (c == '.') break;
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if (c == ' ') continue;
        out[o++] = c;
    }
    if (dot) {
        const char *e = dot + 1;
        o = 8;
        for (int k = 0; e[k] && o < 11; k++) {
            char c = e[k];
            if (c >= 'a' && c <= 'z') c = (char)(c - 32);
            out[o++] = c;
        }
    }
}

/* assemble LFN fragment chars (UTF-16 low byte) into dst at position */
static void lfn_chars(const uint8_t *e, char *dst, int *n) {
    const int idx[13] = {1,3,5,7,9,14,16,18,20,22,24,28,30};
    for (int i = 0; i < 13; i++) {
        uint16_t c = e[idx[i]] | (e[idx[i] + 1] << 8);
        if (c == 0 || c == 0xFFFF) return;
        if (*n < 255) dst[(*n)++] = (char)(c & 0xFF);
    }
}

static void short_to_name(const uint8_t *e, char *out) {
    int o = 0;
    for (int i = 0; i < 8 && e[i] != ' '; i++) out[o++] = (char)e[i];
    int has_ext = e[8] != ' ';
    if (has_ext) {
        out[o++] = '.';
        for (int i = 8; i < 11 && e[i] != ' '; i++) out[o++] = (char)e[i];
    }
    out[o] = 0;
}

/* ---- directory iteration ------------------------------------------------ */
/* Walk every entry of the directory whose first cluster is dir_clus, invoking
 * cb for each live file/subdir. Returns via callback. */
typedef int (*dir_cb)(const uint8_t *ent, const char *name, void *ctx);

static int walk_dir(uint32_t dir_clus, dir_cb cb, void *ctx) {
    char lfn[256]; int lfn_len = 0; int have_lfn = 0;
    uint32_t c = dir_clus;
    while (c >= 2 && c < EOC) {
        if (read_cluster(c, clusbuf) != 0) return -1;
        uint32_t entries = fs.clus_bytes / 32;
        for (uint32_t i = 0; i < entries; i++) {
            uint8_t *e = clusbuf + i * 32;
            if (e[0] == 0x00) return 0;              /* end of directory */
            if (e[0] == 0xE5) { have_lfn = 0; continue; }  /* deleted */
            if (e[11] == 0x0F) {                      /* LFN entry */
                if (e[0] & 0x40) { lfn_len = 0; }     /* last logical -> reset */
                /* LFN pieces come last-first; prepend by rebuilding */
                char frag[16]; int fn = 0;
                lfn_chars(e, frag, &fn);
                /* shift existing right and place frag at front */
                char tmp[256];
                int seq = (e[0] & 0x3F);
                int pos = (seq - 1) * 13;
                for (int k = 0; k < fn && pos + k < 255; k++) lfn[pos + k] = frag[k];
                if (pos + fn > lfn_len) lfn_len = pos + fn;
                (void)tmp;
                have_lfn = 1;
                continue;
            }
            if (e[11] & FAT_ATTR_VOLID) { have_lfn = 0; continue; }
            char name[256];
            if (have_lfn) { for (int k = 0; k < lfn_len; k++) name[k] = lfn[k]; name[lfn_len] = 0; }
            else short_to_name(e, name);
            have_lfn = 0; lfn_len = 0;
            int r = cb(e, name, ctx);
            if (r) return r;                          /* cb requested stop */
        }
        c = fat_get(c);
    }
    return 0;
}

/* ---- path resolution ---------------------------------------------------- */
struct find_ctx { const char *want; uint32_t clus; uint32_t size; uint8_t attr; int found; };
static int find_cb(const uint8_t *e, const char *name, void *vctx) {
    struct find_ctx *fc = vctx;
    /* case-insensitive compare */
    const char *a = name, *b = fc->want;
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 32);
        if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 32);
        if (ca != cb) return 0;
        a++; b++;
    }
    if (*a || *b) return 0;
    fc->clus = ((uint32_t)(e[20] | (e[21] << 8)) << 16) | (e[26] | (e[27] << 8));
    fc->size = *(uint32_t *)(e + 28);
    fc->attr = e[11];
    fc->found = 1;
    return 1;
}

/* split path; resolve to (cluster,size,attr). Returns 0 if found. Also returns
 * the parent directory cluster in *parent (for create operations). */
static int resolve(const char *path, uint32_t *clus, uint32_t *size, uint8_t *attr,
                   uint32_t *parent, char *leaf) {
    uint32_t cur = fs.root_clus;
    uint32_t par = fs.root_clus;
    char comp[256];
    const char *p = path;
    if (*p == '/') p++;
    int is_dir = 1;
    if (leaf) leaf[0] = 0;
    uint32_t csize = 0; uint8_t cattr = FAT_ATTR_DIR;
    while (*p) {
        int n = 0;
        while (*p && *p != '/') { if (n < 255) comp[n++] = *p; p++; }
        comp[n] = 0;
        if (*p == '/') p++;
        if (n == 0) break;
        if (leaf) strcpy(leaf, comp);
        struct find_ctx fc = { comp, 0, 0, 0, 0 };
        par = cur;
        if (walk_dir(cur, find_cb, &fc) <= 0 || !fc.found) {
            if (parent) *parent = cur;
            return -1;                                /* not found */
        }
        cur = fc.clus ? fc.clus : fs.root_clus;
        csize = fc.size; cattr = fc.attr;
        is_dir = (fc.attr & FAT_ATTR_DIR) != 0;
    }
    (void)is_dir;
    if (clus) *clus = cur;
    if (size) *size = csize;
    if (attr) *attr = cattr;
    if (parent) *parent = par;
    return 0;
}

/* ---- public: list ------------------------------------------------------- */
struct list_ctx { fat_dirent *out; int max; int n; };
static int list_cb(const uint8_t *e, const char *name, void *vctx) {
    struct list_ctx *lc = vctx;
    if (lc->n >= lc->max) return 1;
    if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0))) return 0;
    fat_dirent *d = &lc->out[lc->n++];
    strncpy(d->name, name, sizeof d->name - 1);
    d->name[sizeof d->name - 1] = 0;
    d->size = *(uint32_t *)(e + 28);
    d->attr = e[11];
    d->cluster = ((uint32_t)(e[20] | (e[21] << 8)) << 16) | (e[26] | (e[27] << 8));
    return 0;
}
int fat32_list(const char *path, fat_dirent *out, int max) {
    if (!fs.mounted) return -1;
    uint32_t clus; uint8_t attr;
    if (path[0] == 0 || (path[0] == '/' && path[1] == 0)) clus = fs.root_clus;
    else if (resolve(path, &clus, 0, &attr, 0, 0) != 0) return -1;
    struct list_ctx lc = { out, max, 0 };
    walk_dir(clus, list_cb, &lc);
    return lc.n;
}

/* ---- public: read ------------------------------------------------------- */
int fat32_read(const char *path, void *buf, uint32_t max) {
    if (!fs.mounted) return -1;
    uint32_t clus, size; uint8_t attr;
    if (resolve(path, &clus, &size, &attr, 0, 0) != 0) return -1;
    if (attr & FAT_ATTR_DIR) return -1;
    uint8_t *out = buf;
    uint32_t got = 0;
    uint32_t c = clus;
    while (c >= 2 && c < EOC && got < size && got < max) {
        if (read_cluster(c, clusbuf) != 0) return -1;
        uint32_t chunk = fs.clus_bytes;
        if (got + chunk > size) chunk = size - got;
        if (got + chunk > max)  chunk = max - got;
        memcpy(out + got, clusbuf, chunk);
        got += chunk;
        c = fat_get(c);
    }
    return (int)got;
}

/* ---- public: write (create/overwrite) ----------------------------------- */
/* find a free 32-byte slot in dir cluster chain; returns sector+offset, may
 * extend the directory by a cluster. */
static int dir_put_entry(uint32_t dir_clus, const uint8_t ent[32]) {
    uint32_t c = dir_clus, last = dir_clus;
    while (c >= 2 && c < EOC) {
        if (read_cluster(c, clusbuf) != 0) return -1;
        uint32_t entries = fs.clus_bytes / 32;
        for (uint32_t i = 0; i < entries; i++) {
            uint8_t *e = clusbuf + i * 32;
            if (e[0] == 0x00 || e[0] == 0xE5) {
                memcpy(e, ent, 32);
                return write_cluster(c, clusbuf);
            }
        }
        last = c;
        c = fat_get(c);
    }
    /* extend directory */
    uint32_t nc = fat_alloc();
    if (!nc) return -1;
    fat_set(last, nc);
    memset(clusbuf, 0, fs.clus_bytes);
    memcpy(clusbuf, ent, 32);
    return write_cluster(nc, clusbuf);
}

/* update size + first cluster of an existing short entry matching name */
struct upd_ctx { const char *want; uint32_t newclus; uint32_t newsize; int done; };
static int free_chain(uint32_t c) {
    while (c >= 2 && c < EOC) { uint32_t nx = fat_get(c); fat_set(c, 0); c = nx; }
    return 0;
}

int fat32_write(const char *path, const void *data, uint32_t len) {
    if (!fs.mounted) return -1;
    uint32_t parent; char leaf[256];
    uint32_t old_clus, old_size; uint8_t old_attr;
    int exists = (resolve(path, &old_clus, &old_size, &old_attr, &parent, leaf) == 0);
    if (exists && (old_attr & FAT_ATTR_DIR)) return -1;
    if (leaf[0] == 0) return -1;

    /* free old data + remove old directory entry */
    if (exists) {
        if (old_clus) free_chain(old_clus);
        /* mark old dir entry deleted */
        char sn[11]; to_short(leaf, sn);
        uint32_t c = parent;
        while (c >= 2 && c < EOC) {
            if (read_cluster(c, clusbuf) != 0) break;
            uint32_t entries = fs.clus_bytes / 32;
            int hit = 0;
            for (uint32_t i = 0; i < entries; i++) {
                uint8_t *e = clusbuf + i * 32;
                if (e[11] != 0x0F && e[0] != 0xE5 && e[0] != 0 && memcmp(e, sn, 11) == 0) {
                    e[0] = 0xE5; hit = 1;
                }
            }
            if (hit) write_cluster(c, clusbuf);
            c = fat_get(c);
        }
    }

    /* allocate + write data clusters */
    uint32_t first = 0, prev = 0, written = 0;
    const uint8_t *src = data;
    if (len == 0) first = 0;
    while (written < len) {
        uint32_t nc = fat_alloc();
        if (!nc) return -1;
        if (!first) first = nc;
        if (prev) fat_set(prev, nc);
        memset(clusbuf, 0, fs.clus_bytes);
        uint32_t chunk = len - written;
        if (chunk > fs.clus_bytes) chunk = fs.clus_bytes;
        memcpy(clusbuf, src + written, chunk);
        if (write_cluster(nc, clusbuf) != 0) return -1;
        written += chunk;
        prev = nc;
    }

    /* build + insert short directory entry */
    uint8_t ent[32]; memset(ent, 0, 32);
    to_short(leaf, (char *)ent);
    ent[11] = 0x20;                                   /* archive */
    ent[20] = (uint8_t)(first >> 16); ent[21] = (uint8_t)(first >> 24);
    ent[26] = (uint8_t)(first);       ent[27] = (uint8_t)(first >> 8);
    *(uint32_t *)(ent + 28) = len;
    return dir_put_entry(parent, ent);
}

int fat32_mkdir(const char *path) {
    if (!fs.mounted) return -1;
    uint32_t parent; char leaf[256];
    uint32_t cc, cs; uint8_t ca;
    if (resolve(path, &cc, &cs, &ca, &parent, leaf) == 0) return -1;  /* exists */
    if (leaf[0] == 0) return -1;

    uint32_t nc = fat_alloc();
    if (!nc) return -1;
    /* init new dir cluster with . and .. */
    memset(clusbuf, 0, fs.clus_bytes);
    uint8_t *dot = clusbuf, *dotdot = clusbuf + 32;
    memset(dot, ' ', 11); dot[0] = '.'; dot[11] = FAT_ATTR_DIR;
    dot[20] = (uint8_t)(nc >> 16); dot[21] = (uint8_t)(nc >> 24);
    dot[26] = (uint8_t)(nc); dot[27] = (uint8_t)(nc >> 8);
    memset(dotdot, ' ', 11); dotdot[0] = '.'; dotdot[1] = '.'; dotdot[11] = FAT_ATTR_DIR;
    uint32_t pc = (parent == fs.root_clus) ? 0 : parent;
    dotdot[20] = (uint8_t)(pc >> 16); dotdot[21] = (uint8_t)(pc >> 24);
    dotdot[26] = (uint8_t)(pc); dotdot[27] = (uint8_t)(pc >> 8);
    if (write_cluster(nc, clusbuf) != 0) return -1;

    uint8_t ent[32]; memset(ent, 0, 32);
    to_short(leaf, (char *)ent);
    ent[11] = FAT_ATTR_DIR;
    ent[20] = (uint8_t)(nc >> 16); ent[21] = (uint8_t)(nc >> 24);
    ent[26] = (uint8_t)(nc); ent[27] = (uint8_t)(nc >> 8);
    return dir_put_entry(parent, ent);
}

uint64_t fat32_total_bytes(void) { return (uint64_t)fs.total_clus * fs.clus_bytes; }
uint64_t fat32_free_bytes(void) {
    if (!fs.mounted) return 0;
    uint64_t freec = 0;
    for (uint32_t c = 2; c < fs.total_clus + 2; c++) if (fat_get(c) == 0) freec++;
    return freec * fs.clus_bytes;
}

/* ---- mount -------------------------------------------------------------- */
int fat32_mount(blkdev_t *dev) {
    if (!dev) return -1;
    if (blk_read(dev, 0, 1, secbuf) != 0) return -1;
    if (secbuf[510] != 0x55 || secbuf[511] != 0xAA) return -1;

    uint32_t bps = secbuf[11] | (secbuf[12] << 8);
    if (bps != SECSZ) return -1;
    uint32_t spc = secbuf[13];
    uint32_t fatsz = *(uint32_t *)(secbuf + 36);
    if (spc == 0 || spc * SECSZ > MAX_CLUSTER_BYTES || fatsz == 0) return -1;

    fs.dev           = dev;
    fs.bytes_per_sec = bps;
    fs.sec_per_clus  = spc;
    fs.reserved      = secbuf[14] | (secbuf[15] << 8);
    fs.num_fats      = secbuf[16];
    fs.fat_size      = fatsz;
    fs.root_clus     = *(uint32_t *)(secbuf + 44);
    fs.fat_start     = fs.reserved;
    fs.data_start    = fs.reserved + fs.num_fats * fs.fat_size;
    fs.clus_bytes    = spc * SECSZ;

    uint32_t total_sec = *(uint32_t *)(secbuf + 32);
    if (total_sec == 0) total_sec = (uint32_t)dev->sectors;
    uint32_t data_sec = total_sec - fs.data_start;
    fs.total_clus = data_sec / spc;

    memcpy(fs.label, secbuf + 71, 11); fs.label[11] = 0;
    for (int i = 10; i >= 0 && fs.label[i] == ' '; i--) fs.label[i] = 0;

    fs.mounted = 1;
    kprintf("[ok] FAT32 mounted on %s: '%s', %u clusters x %u B\n",
            dev->name, fs.label, fs.total_clus, fs.clus_bytes);
    return 0;
}

/* ---- format ------------------------------------------------------------- */
int fat32_format(blkdev_t *dev, const char *label) {
    if (!dev) return -1;
    uint32_t total = (uint32_t)dev->sectors;
    if (total < 70000) return -1;                     /* too small for FAT32 */

    uint32_t spc = 8;                                  /* 4 KiB clusters */
    uint32_t reserved = 32;
    uint32_t num_fats = 2;

    /* iteratively size the FAT: clusters = (total - reserved - 2*fatsz)/spc */
    uint32_t fatsz = 0;
    for (int it = 0; it < 8; it++) {
        uint32_t datasec = total - reserved - num_fats * fatsz;
        uint32_t clusters = datasec / spc;
        uint32_t needed = ((clusters + 2) * 4 + SECSZ - 1) / SECSZ;
        if (needed == fatsz) break;
        fatsz = needed;
    }

    uint32_t data_start = reserved + num_fats * fatsz;
    uint32_t root_clus = 2;

    /* boot sector */
    memset(secbuf, 0, SECSZ);
    secbuf[0] = 0xEB; secbuf[1] = 0x58; secbuf[2] = 0x90;
    memcpy(secbuf + 3, "MSWIN4.1", 8);
    secbuf[11] = SECSZ & 0xFF; secbuf[12] = SECSZ >> 8;
    secbuf[13] = (uint8_t)spc;
    secbuf[14] = reserved & 0xFF; secbuf[15] = reserved >> 8;
    secbuf[16] = (uint8_t)num_fats;
    secbuf[21] = 0xF8;                                  /* media */
    secbuf[24] = 63; secbuf[26] = 255;                 /* sectors/track, heads */
    *(uint32_t *)(secbuf + 32) = total;                /* total sectors 32 */
    *(uint32_t *)(secbuf + 36) = fatsz;                /* FAT size 32 */
    *(uint32_t *)(secbuf + 44) = root_clus;
    secbuf[48] = 1;                                    /* FSInfo sector */
    secbuf[50] = 6;                                    /* backup boot sector */
    secbuf[66] = 0x29;                                 /* ext boot sig */
    *(uint32_t *)(secbuf + 67) = 0x12345678;           /* volume id */
    memset(secbuf + 71, ' ', 11);
    for (int i = 0; label && label[i] && i < 11; i++) secbuf[71 + i] = (uint8_t)label[i];
    memcpy(secbuf + 82, "FAT32   ", 8);
    secbuf[510] = 0x55; secbuf[511] = 0xAA;
    if (blk_write(dev, 0, 1, secbuf) != 0) return -1;
    blk_write(dev, 6, 1, secbuf);                      /* backup */

    /* FSInfo */
    memset(secbuf, 0, SECSZ);
    *(uint32_t *)(secbuf + 0)   = 0x41615252;
    *(uint32_t *)(secbuf + 484) = 0x61417272;
    *(uint32_t *)(secbuf + 488) = 0xFFFFFFFF;          /* free count unknown */
    *(uint32_t *)(secbuf + 492) = 0xFFFFFFFF;          /* next free unknown */
    secbuf[510] = 0x55; secbuf[511] = 0xAA;
    blk_write(dev, 1, 1, secbuf);

    /* zero the FATs */
    memset(secbuf, 0, SECSZ);
    for (uint32_t f = 0; f < num_fats; f++)
        for (uint32_t s = 0; s < fatsz; s++)
            blk_write(dev, reserved + f * fatsz + s, 1, secbuf);

    /* FAT[0]=media|EOC, FAT[1]=EOC, FAT[2]=EOC (root) */
    *(uint32_t *)(secbuf + 0) = 0x0FFFFFF8;
    *(uint32_t *)(secbuf + 4) = 0x0FFFFFFF;
    *(uint32_t *)(secbuf + 8) = 0x0FFFFFFF;
    for (uint32_t f = 0; f < num_fats; f++)
        blk_write(dev, reserved + f * fatsz, 1, secbuf);

    /* zero the root directory cluster */
    memset(secbuf, 0, SECSZ);
    for (uint32_t s = 0; s < spc; s++)
        blk_write(dev, data_start + s, 1, secbuf);

    kprintf("[fat32] formatted %s: %u sectors, FAT %u sec, %u clusters\n",
            dev->name, total, fatsz, (total - data_start) / spc);
    return fat32_mount(dev);
}
