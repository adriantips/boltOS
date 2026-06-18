#include <stdint.h>
#include <stddef.h>
#include "vfs.h"
#include "fs.h"
#include "kheap.h"
#include "string.h"
#include "serial.h"
#include "pmm.h"
#include "kheap.h"

/* ===========================================================================
 *  Backend ops tables are defined below; vfs_open() routes a path to one of
 *  them. A file* is heap-allocated and freed on close.
 * ===========================================================================*/

static const file_ops ramfs_ops;
static const file_ops dev_ops;
static const file_ops proc_ops;

void vfs_init(void) { /* backends are stateless today; nothing to set up */ }

static file *file_new(const file_ops *ops, void *priv, int flags) {
    file *f = (file *)kmalloc(sizeof(file));
    if (!f) return 0;
    f->ops = ops; f->priv = priv; f->pos = 0; f->flags = flags;
    return f;
}

/* ----------------------------------------------------------------- ramfs ---*/
static int64_t ramfs_read(file *f, void *buf, uint64_t len) {
    fs_node *n = (fs_node *)f->priv;
    if (!n || n->is_dir) return -1;
    if (f->pos >= n->size) return 0;
    uint64_t avail = n->size - f->pos;
    if (len > avail) len = avail;
    memcpy(buf, n->data + f->pos, len);
    f->pos += len;
    return (int64_t)len;
}
/* Writes append at end of file (sequential write model). */
static int64_t ramfs_write(file *f, const void *buf, uint64_t len) {
    fs_node *n = (fs_node *)f->priv;
    if (!n || n->is_dir) return -1;
    if (fs_append(n, buf, (uint32_t)len) != 0) return -1;
    f->pos = n->size;
    return (int64_t)len;
}
static int64_t ramfs_lseek(file *f, int64_t off, int whence) {
    fs_node *n = (fs_node *)f->priv;
    int64_t base = whence == SEEK_CUR ? (int64_t)f->pos
                 : whence == SEEK_END ? (int64_t)n->size : 0;
    int64_t np = base + off;
    if (np < 0) return -1;
    f->pos = (uint64_t)np;
    return np;
}
static void ramfs_close(file *f) { (void)f; }
static const file_ops ramfs_ops = { ramfs_read, ramfs_write, ramfs_lseek, ramfs_close };

/* ------------------------------------------------------------------- /dev --*/
enum { DEV_NULL = 1, DEV_ZERO, DEV_CONSOLE };

static int64_t dev_read(file *f, void *buf, uint64_t len) {
    switch ((int)(uintptr_t)f->priv) {
    case DEV_ZERO:    memset(buf, 0, len); return (int64_t)len;
    case DEV_NULL:    return 0;            /* EOF */
    case DEV_CONSOLE: return 0;            /* no keyboard plumbing yet */
    }
    return -1;
}
static int64_t dev_write(file *f, const void *buf, uint64_t len) {
    switch ((int)(uintptr_t)f->priv) {
    case DEV_CONSOLE: {
        const char *p = (const char *)buf;
        for (uint64_t i = 0; i < len; i++) serial_putc(p[i]);
        return (int64_t)len;
    }
    case DEV_NULL:
    case DEV_ZERO:    return (int64_t)len; /* discard */
    }
    return -1;
}
static int64_t dev_lseek(file *f, int64_t off, int whence) { (void)f;(void)off;(void)whence; return 0; }
static void    dev_close(file *f) { (void)f; }
static const file_ops dev_ops = { dev_read, dev_write, dev_lseek, dev_close };

static file *dev_open(const char *name, int flags) {
    int id = 0;
    if      (!strcmp(name, "null"))    id = DEV_NULL;
    else if (!strcmp(name, "zero"))    id = DEV_ZERO;
    else if (!strcmp(name, "console")) id = DEV_CONSOLE;
    else if (!strcmp(name, "tty"))     id = DEV_CONSOLE;
    else return 0;
    return file_new(&dev_ops, (void *)(uintptr_t)id, flags);
}

/* ------------------------------------------------------------------ /proc --*/
typedef struct { uint8_t *data; uint64_t len; } procbuf;

static char *put_str(char *o, const char *s) { while (*s) *o++ = *s++; return o; }
static char *put_u64(char *o, uint64_t v) {
    char tmp[24]; int i = 0;
    if (v == 0) tmp[i++] = '0';
    while (v) { tmp[i++] = (char)('0' + v % 10); v /= 10; }
    while (i) *o++ = tmp[--i];
    return o;
}

static procbuf *proc_gen(const char *path) {
    char *buf = (char *)kmalloc(512);
    if (!buf) return 0;
    char *o = buf;
    if (!strcmp(path, "/proc/meminfo")) {
        uint64_t total = pmm_total_count() * 4096, freeb = pmm_free_count() * 4096;
        uint64_t hu = 0, ht = 0; kheap_usage(&hu, &ht);
        o = put_str(o, "PhysTotal: "); o = put_u64(o, total >> 10); o = put_str(o, " kB\n");
        o = put_str(o, "PhysFree:  "); o = put_u64(o, freeb >> 10); o = put_str(o, " kB\n");
        o = put_str(o, "KHeapUsed: "); o = put_u64(o, hu >> 10);    o = put_str(o, " kB\n");
        o = put_str(o, "KHeapTotal:"); o = put_u64(o, ht >> 10);    o = put_str(o, " kB\n");
    } else if (!strcmp(path, "/proc/version")) {
        o = put_str(o, "BoltOS 0.3 (x86-64 long mode)\n");
    } else {
        kfree(buf); return 0;
    }
    procbuf *pb = (procbuf *)kmalloc(sizeof(procbuf));
    if (!pb) { kfree(buf); return 0; }
    pb->data = (uint8_t *)buf; pb->len = (uint64_t)(o - buf);
    return pb;
}

static int64_t proc_read(file *f, void *buf, uint64_t len) {
    procbuf *pb = (procbuf *)f->priv;
    if (f->pos >= pb->len) return 0;
    uint64_t avail = pb->len - f->pos;
    if (len > avail) len = avail;
    memcpy(buf, pb->data + f->pos, len);
    f->pos += len;
    return (int64_t)len;
}
static int64_t proc_write(file *f, const void *buf, uint64_t len) { (void)f;(void)buf;(void)len; return -1; }
static int64_t proc_lseek(file *f, int64_t off, int whence) {
    procbuf *pb = (procbuf *)f->priv;
    int64_t base = whence == SEEK_CUR ? (int64_t)f->pos
                 : whence == SEEK_END ? (int64_t)pb->len : 0;
    int64_t np = base + off;
    if (np < 0) return -1;
    f->pos = (uint64_t)np;
    return np;
}
static void proc_close(file *f) {
    procbuf *pb = (procbuf *)f->priv;
    if (pb) { kfree(pb->data); kfree(pb); }
}
static const file_ops proc_ops = { proc_read, proc_write, proc_lseek, proc_close };

/* ------------------------------------------------------------- open/router -*/
file *vfs_open(const char *path, int flags) {
    if (!path || path[0] != '/') return 0;

    if (!strncmp(path, "/dev/", 5))
        return dev_open(path + 5, flags);

    if (!strncmp(path, "/proc/", 6) || !strcmp(path, "/proc")) {
        procbuf *pb = proc_gen(path);
        if (!pb) return 0;
        return file_new(&proc_ops, pb, flags);
    }

    /* default backend: ramfs */
    fs_node *n = fs_lookup(path);
    if (!n && (flags & O_CREAT)) n = fs_create(path, 0);
    if (!n || n->is_dir) return 0;
    if (flags & O_TRUNC) fs_write(n, "", 0);
    file *f = file_new(&ramfs_ops, n, flags);
    if (f && (flags & O_APPEND)) f->pos = n->size;
    return f;
}

int64_t vfs_read (file *f, void *buf, uint64_t len) { return f && f->ops->read  ? f->ops->read(f, buf, len)  : -1; }
int64_t vfs_write(file *f, const void *buf, uint64_t len){ return f && f->ops->write ? f->ops->write(f, buf, len) : -1; }
int64_t vfs_lseek(file *f, int64_t off, int whence) { return f && f->ops->lseek ? f->ops->lseek(f, off, whence) : -1; }

void vfs_close(file *f) {
    if (!f) return;
    if (f->ops->close) f->ops->close(f);
    kfree(f);
}

int vfs_fstat(file *f, vstat *st) {
    if (!f || !st) return -1;
    if (f->ops == &ramfs_ops) { fs_node *n = (fs_node *)f->priv; st->size = n->size; st->is_dir = n->is_dir; return 0; }
    if (f->ops == &proc_ops)  { procbuf *pb = (procbuf *)f->priv; st->size = pb->len; st->is_dir = 0; return 0; }
    st->size = 0; st->is_dir = 0; return 0;   /* /dev: streams, size 0 */
}
