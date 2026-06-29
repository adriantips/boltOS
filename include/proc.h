#pragma once
#include <stdint.h>
#include "vfs.h"

/* A process: an address space, a file-descriptor table, and a user heap
 * (brk + mmap region). Scheduled as one ring-3 thread by the existing
 * round-robin scheduler. */

#define PROC_MAX_FD   16

typedef struct proc {
    int       pid;
    uint64_t  cr3;                  /* PML4 physical for this address space */
    file     *fds[PROC_MAX_FD];     /* fd table (0/1/2 = stdin/out/err)     */
    uint64_t  brk_start, brk_cur;   /* program break (grows up)             */
    uint64_t  mmap_cur;             /* next anonymous mmap address          */
    int       exit_code;
    int       alive;
    char      name[16];
} proc_t;

/* User-space virtual layout (low half, ring 3) */
#define USER_STACK_TOP  0x0000700000000000ull
#define USER_STACK_PGS  64        /* 256 KiB: the ring-3 browser recurses through
                                   * DOM parse + layout + JS eval + paint */
#define USER_MMAP_BASE  0x0000400000000000ull

void    proc_init(void);
int     proc_exec(const char *path);       /* load ELF + run; pid or -1 */
int     proc_exec_argv(const char *path, const char *arg1); /* with argv[1] */
proc_t *proc_current(void);                /* running process, or 0     */

int     proc_fd_alloc(proc_t *p, file *f); /* lowest free fd, or -1     */
file   *proc_fd_get(proc_t *p, int fd);
int     proc_fd_close(proc_t *p, int fd);

void    proc_exit(int code) __attribute__((noreturn));
