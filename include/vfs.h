#pragma once
#include <stdint.h>
#include <stddef.h>

/* ===========================================================================
 *  BoltOS Virtual File System
 *
 *  A thin uniform layer so disks (ramfs), device hardware (/dev) and process
 *  information (/proc) are all reached through the same `struct file` + ops
 *  table, and therefore the same file descriptors. vfs_open() routes a path to
 *  a backend; the returned file* is what a process stores in its fd table.
 * ===========================================================================*/

/* open() flags (subset, values mirror Linux for libc convenience) */
#define O_RDONLY 0x000
#define O_WRONLY 0x001
#define O_RDWR   0x002
#define O_CREAT  0x040
#define O_TRUNC  0x200
#define O_APPEND 0x400

/* lseek whence */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

struct file;

typedef struct file_ops {
    int64_t (*read )(struct file *f, void *buf, uint64_t len);
    int64_t (*write)(struct file *f, const void *buf, uint64_t len);
    int64_t (*lseek)(struct file *f, int64_t off, int whence);
    void    (*close)(struct file *f);
} file_ops;

typedef struct file {
    const file_ops *ops;
    void           *priv;     /* backend handle: fs_node*, dev id, procbuf*   */
    uint64_t        pos;      /* current offset                               */
    int             flags;    /* O_* flags from open                          */
} file;

typedef struct { uint64_t size; int is_dir; } vstat;

void    vfs_init(void);
file   *vfs_open (const char *path, int flags);   /* 0 on failure              */
int64_t vfs_read (file *f, void *buf, uint64_t len);
int64_t vfs_write(file *f, const void *buf, uint64_t len);
int64_t vfs_lseek(file *f, int64_t off, int whence);
void    vfs_close(file *f);
int     vfs_fstat(file *f, vstat *st);            /* 0 ok, -1 err              */
