#include <stdint.h>
#include <stddef.h>
#include "fs.h"
#include "kheap.h"
#include "string.h"
#include "pit.h"
#include "blk.h"
#include "kprintf.h"

static fs_node *root_;
static fs_node *cwd_;

/* persistence state (see bottom of file) */
static blkdev_t *g_disk;       /* backing disk, or 0 for RAM-only */
static uint64_t g_base;        /* first LBA of the BoltFS partition (0 = whole disk) */
static uint64_t g_cap;         /* usable sectors from g_base (partition length) */
static int      g_autosave;    /* flush on every mutation once mounted */
static void     fs_autosave(void);

/* --------------------------------------------------------------------------
 *  node allocation
 * --------------------------------------------------------------------------*/
static fs_node *node_new(const char *name, int is_dir, fs_node *parent) {
    fs_node *n = (fs_node *)kmalloc(sizeof(fs_node));
    if (!n) return 0;
    memset(n, 0, sizeof(*n));
    strncpy(n->name, name, FS_NAME_MAX);
    n->is_dir = is_dir;
    n->parent = parent;
    n->mtime  = pit_ticks();
    if (parent) {                               /* append to sibling list */
        if (!parent->child) parent->child = n;
        else {
            fs_node *c = parent->child;
            while (c->next) c = c->next;
            c->next = n;
        }
    }
    return n;
}

void fs_init(void) {
    root_ = (fs_node *)kmalloc(sizeof(fs_node));
    memset(root_, 0, sizeof(*root_));
    strncpy(root_->name, "/", FS_NAME_MAX);
    root_->is_dir = 1;
    cwd_ = root_;

    /* seed a small tree so the file commands have something to show */
    fs_create("/etc", 1);
    fs_create("/home", 1);
    fs_create("/tmp", 1);
    fs_node *f;
    f = fs_create("/etc/motd", 0);
    if (f) fs_write(f, "Welcome to BoltOS.\nType 'help' for commands.\n", 45);
    f = fs_create("/etc/version", 0);
    if (f) fs_write(f, "BoltOS 0.2 (64-bit long mode)\n", 30);
    f = fs_create("/home/readme.txt", 0);
    if (f) fs_write(f,
        "BoltOS in-RAM filesystem.\n"
        "Files live in the kernel heap and reset on reboot.\n"
        "Create one with:  write notes.txt hello world\n", 122);

    /* A JavaScript demo page: open it in the Browser (address: /home/jsdemo.html)
     * to see the BoltJS engine read and rewrite the DOM. */
    f = fs_create("/home/jsdemo.html", 0);
    if (f) {
        const char *js =
            "<html><head><title>JS Demo</title></head><body>"
            "<h1>BoltJS DOM Demo</h1>"
            "<p id=\"out\">placeholder text (should be replaced)</p>"
            "<p id=\"list\">loop result here</p>"
            "<div id=\"calc\">calc result here</div>"
            "<script>"
            "document.getElementById('out').innerHTML = 'This text was set by JavaScript!';"
            "var s=''; for(var i=1;i<=5;i++){ s += i*i + ' '; }"
            "document.getElementById('list').textContent = 'Squares 1..5: ' + s;"
            "function fact(n){ return n<=1 ? 1 : n*fact(n-1); }"
            "document.getElementById('calc').innerHTML = '7! = ' + fact(7) + ', and 10! = ' + fact(10);"
            "document.title = 'BoltJS ran on this page';"
            "document.write('<p>This paragraph was generated at runtime by document.write().</p>');"
            "console.log('script finished; fact(6)=' + fact(6));"
            "</script>"
            "</body></html>";
        fs_write(f, js, (uint32_t)strlen(js));
    }

    /* BoltJS async demo: Promises + JSON.parse + Promise.all + new Promise.
     * Run it from the shell with:  js /home/promise.js
     * (timers/fetch need the browser's pump; this file is pump-free on purpose
     * so it verifies the microtask queue deterministically.) */
    f = fs_create("/home/promise.js", 0);
    if (f) {
        const char *js =
            "console.log('sync-start');\n"
            "Promise.resolve(10).then(function(v){ console.log('then1='+v); return v+5; })"
            "                   .then(function(v){ console.log('then2='+v); });\n"
            "Promise.reject('boom').catch(function(e){ console.log('caught='+e); });\n"
            "var d=JSON.parse('{\"n\":42,\"s\":\"hi\",\"a\":[1,2,3],\"b\":true}');\n"
            "console.log('json='+d.n+','+d.s+','+d.a[1]+','+d.b);\n"
            "Promise.all([Promise.resolve(1),Promise.resolve(2),3])"
            "       .then(function(a){ console.log('all='+a[0]+a[1]+a[2]); });\n"
            "new Promise(function(res,rej){ res('made'); }).then(function(v){ console.log('ctor='+v); });\n"
            "console.log('sync-end');\n";
        fs_write(f, js, (uint32_t)strlen(js));
    }

    /* fetch() over the real network:  js /home/fetch.js  */
    f = fs_create("/home/fetch.js", 0);
    if (f) {
        const char *js =
            "console.log('fetch-start');\n"
            "fetch('http://example.com/')"
            "  .then(function(r){ console.log('status='+r.status+' ok='+r.ok); return r.text(); })"
            "  .then(function(t){ console.log('bodylen='+t.length); });\n"
            "console.log('fetch-issued');\n";
        fs_write(f, js, (uint32_t)strlen(js));
    }
}

fs_node *fs_root(void) { return root_; }
fs_node *fs_cwd(void)  { return cwd_; }
void     fs_set_cwd(fs_node *d) { if (d && d->is_dir) cwd_ = d; }

int fs_chdir(const char *path) {
    fs_node *n = fs_lookup(path && *path ? path : "/");
    if (!n || !n->is_dir) return -1;
    cwd_ = n;
    return 0;
}

/* --------------------------------------------------------------------------
 *  lookup / path resolution
 * --------------------------------------------------------------------------*/
fs_node *fs_child(fs_node *dir, const char *name) {
    if (!dir || !dir->is_dir) return 0;
    for (fs_node *c = dir->child; c; c = c->next)
        if (strcmp(c->name, name) == 0) return c;
    return 0;
}

fs_node *fs_lookup(const char *path) {
    if (!path || !*path) return cwd_;
    char buf[FS_PATH_MAX];
    strncpy(buf, path, sizeof(buf));

    fs_node *cur = (buf[0] == '/') ? root_ : cwd_;
    char *p = buf;
    while (*p == '/') p++;

    while (*p) {
        char *seg = p;
        while (*p && *p != '/') p++;
        if (*p) *p++ = 0;
        while (*p == '/') p++;

        if (seg[0] == 0 || strcmp(seg, ".") == 0) continue;
        if (strcmp(seg, "..") == 0) { if (cur->parent) cur = cur->parent; continue; }
        if (!cur->is_dir) return 0;
        cur = fs_child(cur, seg);
        if (!cur) return 0;
    }
    return cur;
}

/* Resolve everything up to the last component; return the parent dir and copy
 * the final name into leaf_out. */
fs_node *fs_parent_of(const char *path, char *leaf_out, uint32_t leaf_cap) {
    char buf[FS_PATH_MAX];
    strncpy(buf, path, sizeof(buf));
    /* strip a single trailing slash */
    uint32_t L = (uint32_t)strlen(buf);
    if (L > 1 && buf[L - 1] == '/') buf[L - 1] = 0;

    char *slash = strrchr(buf, '/');
    if (!slash) {                               /* bare name -> cwd */
        strncpy(leaf_out, buf, leaf_cap);
        return cwd_;
    }
    strncpy(leaf_out, slash + 1, leaf_cap);
    if (slash == buf) return root_;             /* "/leaf" */
    *slash = 0;
    return fs_lookup(buf);
}

fs_node *fs_create(const char *path, int is_dir) {
    char leaf[FS_NAME_MAX];
    fs_node *parent = fs_parent_of(path, leaf, sizeof(leaf));
    if (!parent || !parent->is_dir) return 0;
    if (leaf[0] == 0) return 0;
    if (fs_child(parent, leaf)) return 0;       /* already exists */
    fs_node *n = node_new(leaf, is_dir, parent);
    if (n) fs_autosave();
    return n;
}

/* --------------------------------------------------------------------------
 *  remove
 * --------------------------------------------------------------------------*/
static void node_free_tree(fs_node *n) {
    fs_node *c = n->child;
    while (c) { fs_node *nx = c->next; node_free_tree(c); c = nx; }
    if (n->data) kfree(n->data);
    kfree(n);
}

int fs_remove_node(fs_node *n) {
    if (!n || n == root_) return -1;
    fs_node *parent = n->parent;
    if (parent) {                               /* unlink from sibling list */
        if (parent->child == n) parent->child = n->next;
        else {
            fs_node *c = parent->child;
            while (c && c->next != n) c = c->next;
            if (c) c->next = n->next;
        }
    }
    if (cwd_ == n) cwd_ = parent ? parent : root_;
    node_free_tree(n);
    fs_autosave();
    return 0;
}

int fs_remove(const char *path) {
    return fs_remove_node(fs_lookup(path));
}

/* --------------------------------------------------------------------------
 *  read/write
 * --------------------------------------------------------------------------*/
int fs_write(fs_node *n, const void *data, uint32_t len) {
    if (!n || n->is_dir) return -1;
    if (n->cap < len || !n->data) {
        uint8_t *nd = (uint8_t *)kmalloc(len ? len : 1);
        if (!nd) return -1;
        if (n->data) kfree(n->data);
        n->data = nd;
        n->cap  = len ? len : 1;
    }
    if (len) memcpy(n->data, data, len);
    n->size  = len;
    n->mtime = pit_ticks();
    fs_autosave();
    return 0;
}

int fs_append(fs_node *n, const void *data, uint32_t len) {
    if (!n || n->is_dir) return -1;
    uint32_t need = n->size + len;
    if (n->cap < need) {
        uint8_t *nd = (uint8_t *)kmalloc(need ? need : 1);
        if (!nd) return -1;
        if (n->size && n->data) memcpy(nd, n->data, n->size);
        if (n->data) kfree(n->data);
        n->data = nd;
        n->cap  = need;
    }
    if (len) memcpy(n->data + n->size, data, len);
    n->size  = need;
    n->mtime = pit_ticks();
    fs_autosave();
    return 0;
}

/* --------------------------------------------------------------------------
 *  move / rename
 * --------------------------------------------------------------------------*/
static int is_ancestor(fs_node *anc, fs_node *n) {
    for (fs_node *p = n; p; p = p->parent) if (p == anc) return 1;
    return 0;
}

int fs_move(fs_node *n, fs_node *newparent, const char *newname) {
    if (!n || n == root_ || !newparent || !newparent->is_dir) return -1;
    if (is_ancestor(n, newparent)) return -1;   /* no moving into own subtree */
    if (fs_child(newparent, newname)) return -1;

    fs_node *op = n->parent;                     /* unlink from old parent */
    if (op) {
        if (op->child == n) op->child = n->next;
        else {
            fs_node *c = op->child;
            while (c && c->next != n) c = c->next;
            if (c) c->next = n->next;
        }
    }
    n->next = 0;
    n->parent = newparent;
    strncpy(n->name, newname, FS_NAME_MAX);
    if (!newparent->child) newparent->child = n;
    else {
        fs_node *c = newparent->child;
        while (c->next) c = c->next;
        c->next = n;
    }
    fs_autosave();
    return 0;
}

/* --------------------------------------------------------------------------
 *  helpers
 * --------------------------------------------------------------------------*/
void fs_abspath(fs_node *n, char *out, uint32_t cap) {
    if (!n) { strncpy(out, "?", cap); return; }
    if (n == root_) { strncpy(out, "/", cap); return; }

    fs_node *stack[FS_MAX_DEPTH];
    int depth = 0;
    for (fs_node *p = n; p && p != root_ && depth < FS_MAX_DEPTH; p = p->parent)
        stack[depth++] = p;

    uint32_t pos = 0;
    for (int i = depth - 1; i >= 0; i--) {
        if (pos + 1 < cap) out[pos++] = '/';
        const char *s = stack[i]->name;
        while (*s && pos + 1 < cap) out[pos++] = *s++;
    }
    out[pos < cap ? pos : cap - 1] = 0;
}

static uint64_t bytes_rec(fs_node *n) {
    uint64_t t = n->is_dir ? 0 : n->size;
    for (fs_node *c = n->child; c; c = c->next) t += bytes_rec(c);
    return t;
}
uint64_t fs_total_bytes(void) { return root_ ? bytes_rec(root_) : 0; }

static int count_rec(fs_node *n) {
    int t = 1;
    for (fs_node *c = n->child; c; c = c->next) t += count_rec(c);
    return t;
}
int fs_count_nodes(void) { return root_ ? count_rec(root_) - 1 : 0; }

/* ==========================================================================
 *  Persistence  -  BoltFS on-disk image (serialised tree on a real ATA disk).
 *
 *  Layout:  sector 0 = superblock, sectors 1.. = a flat pre-order list of
 *  node records (parent given by array index), file data inlined after each
 *  file record. Whole image is rewritten on every mutation (autosave) - the
 *  trees this OS holds are tiny, so simplicity beats journalling here.
 * ==========================================================================*/
#define BFS_MAGIC   "BOLTFS1"          /* 7 chars + NUL */
#define BFS_NOPAR   0xFFFFFFFFu

struct bfs_super {
    char     magic[8];
    uint32_t version;
    uint32_t node_count;
    uint64_t blob_bytes;
    uint32_t checksum;                 /* sum32 over the blob */
    uint32_t reserved;
};

struct bfs_rec {
    uint32_t parent;                   /* index of parent record, BFS_NOPAR for root */
    uint32_t is_dir;
    uint32_t size;                     /* file data length (0 for dirs) */
    uint32_t pad;
    uint64_t mtime;
    char     name[FS_NAME_MAX];
};

static void measure(fs_node *n, uint32_t *cnt, uint64_t *bytes) {
    (*cnt)++;
    *bytes += sizeof(struct bfs_rec) + (n->is_dir ? 0 : n->size);
    for (fs_node *c = n->child; c; c = c->next) measure(c, cnt, bytes);
}

static uint32_t g_emit_idx;
static void emit(fs_node *n, uint32_t parent_idx, uint8_t **pp) {
    uint32_t myidx = g_emit_idx++;
    struct bfs_rec r;
    memset(&r, 0, sizeof r);
    r.parent = parent_idx;
    r.is_dir = (uint32_t)n->is_dir;
    r.size   = n->is_dir ? 0 : n->size;
    r.mtime  = n->mtime;
    strncpy(r.name, n->name, FS_NAME_MAX);
    memcpy(*pp, &r, sizeof r); *pp += sizeof r;
    if (!n->is_dir && n->size) { memcpy(*pp, n->data, n->size); *pp += n->size; }
    for (fs_node *c = n->child; c; c = c->next) emit(c, myidx, pp);
}

int fs_sync(void) {
    if (!g_disk) return -1;

    uint32_t cnt = 0; uint64_t blob = 0;
    measure(root_, &cnt, &blob);

    uint64_t sectors = 1 + (blob + BLK_SECTOR - 1) / BLK_SECTOR;
    if (sectors > g_cap) {
        kprintf("fs_sync: image (%lu sectors) exceeds partition (%lu)\n",
                (unsigned long)sectors, (unsigned long)g_cap);
        return -1;
    }

    uint64_t bufsz = sectors * BLK_SECTOR;
    uint8_t *buf = (uint8_t *)kmalloc(bufsz);
    if (!buf) return -1;
    memset(buf, 0, bufsz);

    uint8_t *p = buf + BLK_SECTOR;          /* blob starts after the superblock */
    g_emit_idx = 0;
    emit(root_, BFS_NOPAR, &p);

    uint32_t sum = 0;
    for (uint64_t i = 0; i < blob; i++) sum += buf[BLK_SECTOR + i];

    struct bfs_super sb;
    memset(&sb, 0, sizeof sb);
    memcpy(sb.magic, BFS_MAGIC, 8);
    sb.version    = 1;
    sb.node_count = cnt;
    sb.blob_bytes = blob;
    sb.checksum   = sum;
    memcpy(buf, &sb, sizeof sb);

    int rc = blk_write(g_disk, g_base, (uint32_t)sectors, buf);
    kfree(buf);
    return rc;
}

static void fs_autosave(void) { if (g_autosave) fs_sync(); }

/* free every child of root (used before loading a saved image over the seed) */
static void fs_clear(void) {
    fs_node *c = root_->child;
    while (c) { fs_node *nx = c->next; node_free_tree(c); c = nx; }
    root_->child = 0;
    cwd_ = root_;
}

static int fs_load_image(void) {
    if (!g_disk) return -1;

    uint8_t sec0[BLK_SECTOR];
    if (blk_read(g_disk, g_base, 1, sec0) != 0) return -1;
    struct bfs_super *sb = (struct bfs_super *)sec0;
    if (memcmp(sb->magic, BFS_MAGIC, 8) != 0) return -1;
    if (sb->version != 1 || sb->node_count == 0) return -1;

    uint64_t blob = sb->blob_bytes;
    uint32_t cnt  = sb->node_count;
    uint64_t sectors = (blob + BLK_SECTOR - 1) / BLK_SECTOR;
    if (1 + sectors > g_cap || blob == 0) return -1;

    uint8_t *buf = (uint8_t *)kmalloc(sectors * BLK_SECTOR);
    if (!buf) return -1;
    if (blk_read(g_disk, g_base + 1, (uint32_t)sectors, buf) != 0) { kfree(buf); return -1; }

    uint32_t sum = 0;
    for (uint64_t i = 0; i < blob; i++) sum += buf[i];
    if (sum != sb->checksum) { kfree(buf); return -1; }

    fs_node **map = (fs_node **)kmalloc(cnt * sizeof(fs_node *));
    if (!map) { kfree(buf); return -1; }

    fs_clear();

    uint64_t off = 0;
    int ok = 1;
    for (uint32_t i = 0; i < cnt && ok; i++) {
        if (off + sizeof(struct bfs_rec) > blob) { ok = 0; break; }
        struct bfs_rec r;
        memcpy(&r, buf + off, sizeof r);
        off += sizeof r;
        uint32_t dsize = r.is_dir ? 0 : r.size;
        if (off + dsize > blob) { ok = 0; break; }

        fs_node *n;
        if (i == 0) {                               /* record 0 is the root */
            n = root_;
            strncpy(n->name, "/", FS_NAME_MAX);
            n->is_dir = 1;
            n->mtime  = r.mtime;
        } else {
            if (r.parent >= i) { ok = 0; break; }   /* parents precede children */
            n = node_new(r.name, (int)r.is_dir, map[r.parent]);
            if (!n) { ok = 0; break; }
            n->mtime = r.mtime;
            if (!r.is_dir && dsize) {
                n->data = (uint8_t *)kmalloc(dsize);
                if (!n->data) { ok = 0; break; }
                memcpy(n->data, buf + off, dsize);
                n->size = dsize;
                n->cap  = dsize;
            }
        }
        map[i] = n;
        off += dsize;
    }

    kfree(map);
    kfree(buf);
    if (!ok) { fs_clear(); return -1; }
    return 0;
}

/* Pick (or create) the partition BoltFS lives in. Prefers an existing BoltFS
 * partition, else the first present partition; if the disk has no valid MBR it
 * writes one with a single BoltFS partition aligned at 1 MiB. Sets g_base/g_cap. */
static void fs_attach_partition(void) {
    blk_part parts[4];
    int n = blk_read_mbr(g_disk, parts);
    int idx = -1;

    if (n > 0) {
        for (int i = 0; i < 4; i++)                  /* prefer a BoltFS partition */
            if (parts[i].present && parts[i].type == BLK_PART_BOLTFS) { idx = i; break; }
        if (idx < 0)
            for (int i = 0; i < 4; i++)              /* else first present one    */
                if (parts[i].present) { idx = i; break; }
    }

    if (idx < 0) {                                   /* no usable table -> create */
        uint64_t start = 2048;                       /* 1 MiB alignment           */
        if (start >= g_disk->sectors) start = 1;     /* tiny disk fallback        */
        if (blk_write_mbr_single(g_disk, BLK_PART_BOLTFS, start) == 0 &&
            blk_read_mbr(g_disk, parts) > 0) {
            idx = 0;
            kprintf("[ok] fs: wrote MBR, BoltFS partition @ LBA %lu\n",
                    (unsigned long)start);
        }
    }

    if (idx >= 0) {
        g_base = parts[idx].lba_start;
        g_cap  = parts[idx].sectors;
    } else {                                         /* MBR unavailable -> raw disk */
        g_base = 0;
        g_cap  = g_disk->sectors;
        kprintf("[--] fs: no partition table; using raw disk\n");
    }
}

void fs_persist_init(void) {
    g_disk = blk_fs_disk();
    if (!g_disk) {
        kprintf("[--] fs: no data disk found; RAM-only (volatile)\n");
        return;
    }

    fs_attach_partition();

    if (fs_load_image() == 0) {
        kprintf("[ok] fs: loaded image from %s '%s' part@LBA%lu (%d nodes)\n",
                blk_media(g_disk), g_disk->model[0] ? g_disk->model : "disk",
                (unsigned long)g_base, fs_count_nodes());
    } else {
        kprintf("[ok] fs: formatting %s '%s' part@LBA%lu with seed tree\n",
                blk_media(g_disk), g_disk->model[0] ? g_disk->model : "disk",
                (unsigned long)g_base);
    }

    g_autosave = 1;
    fs_sync();                  /* persist current tree (loaded or freshly seeded) */
}

int fs_persist_active(void) { return g_disk != 0; }
const char *fs_persist_media(void) { return g_disk ? blk_media(g_disk) : ""; }
const char *fs_persist_model(void) { return g_disk ? g_disk->model : ""; }
