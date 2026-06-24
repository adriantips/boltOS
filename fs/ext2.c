/* ===========================================================================
 *  BoltOS  -  fs/ext2.c   (read-only ext2 driver)
 *  See include/ext2.h. Parses the superblock, block-group descriptors, inode
 *  table and directory entries; resolves direct/single/double-indirect block
 *  pointers. All disk I/O goes through the generic block layer (blk_read).
 * ===========================================================================*/
#include <stdint.h>
#include "ext2.h"
#include "blk.h"
#include "kprintf.h"
#include "string.h"

#define EXT2_MAGIC 0xEF53
#define ROOT_INO   2
#define EXT2_S_IFDIR 0x4000

/* on-disk superblock (only the fields we use) */
struct sb {
    uint32_t inodes_count, blocks_count, r_blocks_count, free_blocks, free_inodes;
    uint32_t first_data_block, log_block_size, log_frag_size;
    uint32_t blocks_per_group, frags_per_group, inodes_per_group;
    uint32_t mtime, wtime;
    uint16_t mnt_count, max_mnt_count, magic, state, errors, minor_rev;
    uint32_t lastcheck, checkinterval, creator_os, rev_level;
    uint16_t def_resuid, def_resgid;
    uint32_t first_ino;
    uint16_t inode_size;
    /* ... rest unused ... */
} __attribute__((packed));

struct gdesc {
    uint32_t block_bitmap, inode_bitmap, inode_table;
    uint16_t free_blocks, free_inodes, used_dirs, pad;
    uint8_t  reserved[12];
} __attribute__((packed));

struct dinode {
    uint16_t mode, uid;
    uint32_t size;
    uint32_t atime, ctime, mtime, dtime;
    uint16_t gid, links;
    uint32_t blocks, flags, osd1;
    uint32_t block[15];
    uint32_t generation, file_acl, dir_acl, faddr;
    uint8_t  osd2[12];
} __attribute__((packed));

static struct {
    blkdev_t *dev;
    int       mounted;
    uint32_t  block_size;
    uint32_t  inodes_per_group;
    uint32_t  blocks_per_group;
    uint32_t  first_data_block;
    uint32_t  inode_size;
    uint32_t  gdt_block;          /* block holding the group descriptor table */
    uint32_t  blocks_count;
    uint32_t  free_blocks;
} fs;

/* read one filesystem block into buf (must hold block_size bytes) */
static int read_block(uint32_t blk, void *buf) {
    uint32_t spb = fs.block_size / BLK_SECTOR;
    return blk_read(fs.dev, (uint64_t)blk * spb, spb, buf);
}

static int read_gdesc(uint32_t group, struct gdesc *gd) {
    static uint8_t b[4096];
    uint32_t per_block = fs.block_size / sizeof(struct gdesc);
    uint32_t blk = fs.gdt_block + group / per_block;
    if (read_block(blk, b) != 0) return -1;
    *gd = ((struct gdesc *)b)[group % per_block];
    return 0;
}

static int read_inode(uint32_t ino, struct dinode *out) {
    if (ino == 0) return -1;
    uint32_t group = (ino - 1) / fs.inodes_per_group;
    uint32_t index = (ino - 1) % fs.inodes_per_group;
    struct gdesc gd;
    if (read_gdesc(group, &gd) != 0) return -1;
    uint64_t byte = (uint64_t)gd.inode_table * fs.block_size + (uint64_t)index * fs.inode_size;
    uint64_t lba  = byte / BLK_SECTOR;
    uint32_t off  = (uint32_t)(byte % BLK_SECTOR);
    static uint8_t sec[BLK_SECTOR * 2];
    if (blk_read(fs.dev, lba, 2, sec) != 0) return -1;   /* inode may straddle 2 sectors */
    memcpy(out, sec + off, sizeof(struct dinode));
    return 0;
}

/* map a file's logical block index to its physical block, following direct,
 * single- and double-indirect pointers. Returns 0 (a hole) if unmapped. */
static uint32_t bmap(const struct dinode *in, uint32_t lbn) {
    uint32_t ppb = fs.block_size / 4;                 /* pointers per block */
    if (lbn < 12) return in->block[lbn];
    lbn -= 12;
    static uint8_t ib[4096];
    if (lbn < ppb) {                                  /* single indirect */
        if (!in->block[12]) return 0;
        if (read_block(in->block[12], ib) != 0) return 0;
        return ((uint32_t *)ib)[lbn];
    }
    lbn -= ppb;
    if (lbn < ppb * ppb) {                            /* double indirect */
        if (!in->block[13]) return 0;
        if (read_block(in->block[13], ib) != 0) return 0;
        uint32_t mid = ((uint32_t *)ib)[lbn / ppb];
        if (!mid) return 0;
        static uint8_t ib2[4096];
        if (read_block(mid, ib2) != 0) return 0;
        return ((uint32_t *)ib2)[lbn % ppb];
    }
    return 0;                                          /* triple indirect: unsupported */
}

/* copy up to cap bytes of inode `in` into buf; returns bytes copied */
static uint32_t read_file_data(const struct dinode *in, uint8_t *buf, uint32_t cap) {
    uint32_t size = in->size;
    if (size > cap) size = cap;
    static uint8_t blk[4096];
    uint32_t done = 0;
    for (uint32_t lbn = 0; done < size; lbn++) {
        uint32_t phys = bmap(in, lbn);
        uint32_t take = fs.block_size;
        if (done + take > size) take = size - done;
        if (phys == 0) { memset(buf + done, 0, take); }   /* sparse hole */
        else { if (read_block(phys, blk) != 0) break; memcpy(buf + done, blk, take); }
        done += take;
    }
    return done;
}

/* find `name` in directory inode `dir`; returns its inode number or 0 */
static uint32_t dir_find(const struct dinode *dir, const char *name, int namelen) {
    static uint8_t blk[4096];
    uint32_t nblocks = (dir->size + fs.block_size - 1) / fs.block_size;
    for (uint32_t b = 0; b < nblocks; b++) {
        uint32_t phys = bmap(dir, b);
        if (!phys || read_block(phys, blk) != 0) continue;
        uint32_t off = 0;
        while (off < fs.block_size) {
            uint32_t ino   = *(uint32_t *)(blk + off);
            uint16_t rlen  = *(uint16_t *)(blk + off + 4);
            uint8_t  nlen  = blk[off + 6];
            if (rlen < 8) break;
            if (ino && nlen == namelen && memcmp(blk + off + 8, name, namelen) == 0)
                return ino;
            off += rlen;
        }
    }
    return 0;
}

/* resolve an absolute path to an inode number (0 on failure) */
static uint32_t path_to_ino(const char *path, struct dinode *out) {
    struct dinode cur;
    if (read_inode(ROOT_INO, &cur) != 0) return 0;
    uint32_t ino = ROOT_INO;
    const char *p = path;
    while (*p == '/') p++;
    while (*p) {
        char comp[256]; int n = 0;
        while (*p && *p != '/' && n < 255) comp[n++] = *p++;
        comp[n] = 0;
        while (*p == '/') p++;
        if (n == 0) break;
        uint32_t next = dir_find(&cur, comp, n);
        if (!next) return 0;
        if (read_inode(next, &cur) != 0) return 0;
        ino = next;
    }
    if (out) *out = cur;
    return ino;
}

int ext2_mount(blkdev_t *dev) {
    static uint8_t buf[1024];
    /* superblock lives at byte offset 1024 (LBA 2 for 512-byte sectors) */
    if (blk_read(dev, 2, 2, buf) != 0) return -1;
    struct sb *sb = (struct sb *)buf;
    if (sb->magic != EXT2_MAGIC) return -1;

    fs.dev              = dev;
    fs.block_size       = 1024u << sb->log_block_size;
    fs.inodes_per_group = sb->inodes_per_group;
    fs.blocks_per_group = sb->blocks_per_group;
    fs.first_data_block = sb->first_data_block;
    fs.inode_size       = (sb->rev_level >= 1 && sb->inode_size) ? sb->inode_size : 128;
    fs.gdt_block        = sb->first_data_block + 1;
    fs.blocks_count     = sb->blocks_count;
    fs.free_blocks      = sb->free_blocks;
    if (fs.block_size > 4096) { fs.dev = 0; return -1; }   /* our buffers are 4 KiB */
    fs.mounted = 1;
    kprintf("[ok] ext2 mounted on %s: %u blocks x %u B, %u inodes/group\n",
            dev->name, fs.blocks_count, fs.block_size, fs.inodes_per_group);
    return 0;
}

int         ext2_mounted(void)       { return fs.mounted; }
const char *ext2_volume_name(void)   { return fs.mounted ? fs.dev->name : ""; }
uint64_t    ext2_total_bytes(void)   { return (uint64_t)fs.blocks_count * fs.block_size; }
uint64_t    ext2_free_bytes(void)    { return (uint64_t)fs.free_blocks  * fs.block_size; }

int ext2_list(const char *path, ext2_dirent *out, int max) {
    if (!fs.mounted) return -1;
    struct dinode dir;
    if (!path_to_ino(path, &dir)) return -1;
    if (!(dir.mode & EXT2_S_IFDIR)) return -1;
    static uint8_t blk[4096];
    int count = 0;
    uint32_t nblocks = (dir.size + fs.block_size - 1) / fs.block_size;
    for (uint32_t b = 0; b < nblocks && count < max; b++) {
        uint32_t phys = bmap(&dir, b);
        if (!phys || read_block(phys, blk) != 0) continue;
        uint32_t off = 0;
        while (off < fs.block_size && count < max) {
            uint32_t ino  = *(uint32_t *)(blk + off);
            uint16_t rlen = *(uint16_t *)(blk + off + 4);
            uint8_t  nlen = blk[off + 6];
            uint8_t  ftyp = blk[off + 7];
            if (rlen < 8) break;
            if (ino && nlen) {
                int n = nlen < 255 ? nlen : 255;
                memcpy(out[count].name, blk + off + 8, n);
                out[count].name[n] = 0;
                out[count].inode = ino;
                out[count].is_dir = (ftyp == 2);     /* EXT2_FT_DIR */
                struct dinode di;
                out[count].size = (read_inode(ino, &di) == 0) ? di.size : 0;
                count++;
            }
            off += rlen;
        }
    }
    return count;
}

int ext2_read(const char *path, void *buf, uint32_t cap) {
    if (!fs.mounted) return -1;
    struct dinode in;
    if (!path_to_ino(path, &in)) return -1;
    if (in.mode & EXT2_S_IFDIR) return -1;
    return (int)read_file_data(&in, (uint8_t *)buf, cap);
}
