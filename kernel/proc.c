#include <stdint.h>
#include "proc.h"
#include "elf.h"
#include "vfs.h"
#include "vmm.h"
#include "pmm.h"
#include "mm.h"
#include "sched.h"
#include "kheap.h"
#include "string.h"
#include "kprintf.h"
#include "framebuffer.h"

static int next_pid = 1;

proc_t *proc_current(void) { return sched_current_proc(); }

void proc_init(void) { /* nothing global to set up yet */ }

/* -------------------------------------------------------------- fd table ---*/
int proc_fd_alloc(proc_t *p, file *f) {
    for (int i = 0; i < PROC_MAX_FD; i++)
        if (!p->fds[i]) { p->fds[i] = f; return i; }
    return -1;
}
file *proc_fd_get(proc_t *p, int fd) {
    if (fd < 0 || fd >= PROC_MAX_FD) return 0;
    return p->fds[fd];
}
int proc_fd_close(proc_t *p, int fd) {
    if (fd < 0 || fd >= PROC_MAX_FD || !p->fds[fd]) return -1;
    vfs_close(p->fds[fd]);
    p->fds[fd] = 0;
    return 0;
}

/* ---------------------------------------------------------- stack builder --*/
static inline void poke64(uint8_t *page_kv, uint64_t frame_off, uint64_t v) {
    *(uint64_t *)(page_kv + frame_off) = v;
}

/* Map the user stack and lay out a minimal SysV entry stack (argc/argv/auxv).
 * arg0 is argv[0]; arg1 (if non-null) becomes argv[1], so a ring-3 tool can be
 * handed an operand (e.g. the script path for /bin/js). Returns the initial
 * user RSP, or 0 on failure. */
static uint64_t setup_user_stack(uint64_t cr3, const char *arg0, const char *arg1) {
    uint64_t base = USER_STACK_TOP - (uint64_t)USER_STACK_PGS * PAGE_SIZE;
    uint64_t top_frame = 0;
    for (int i = 0; i < USER_STACK_PGS; i++) {
        uint64_t fr = pmm_alloc_frame();
        if (!fr) return 0;
        uint8_t *kv = (uint8_t *)P2V(fr);
        for (int j = 0; j < (int)PAGE_SIZE; j++) kv[j] = 0;
        uint64_t va = base + (uint64_t)i * PAGE_SIZE;
        if (vmm_map(cr3, va, fr, PTE_PRESENT | PTE_WRITE | PTE_USER) != 0) return 0;
        if (i == USER_STACK_PGS - 1) top_frame = fr;
    }
    /* everything we write lives in the top page: [TOP-PAGE_SIZE, TOP) */
    uint8_t *tk = (uint8_t *)P2V(top_frame);
    uint64_t page_va = USER_STACK_TOP - PAGE_SIZE;     /* user VA of top page base */
    #define OFF(uva) ((uva) - page_va)

    int argc = arg1 ? 2 : 1;

    /* copy the argv strings down from the very top of the stack */
    uint64_t slen0 = strlen(arg0) + 1;
    uint64_t arg0_uva = (USER_STACK_TOP - slen0) & ~0xFull;
    for (uint64_t i = 0; i < slen0; i++) tk[OFF(arg0_uva) + i] = (uint8_t)arg0[i];

    uint64_t arg1_uva = 0, lowstr = arg0_uva;
    if (arg1) {
        uint64_t slen1 = strlen(arg1) + 1;
        arg1_uva = (arg0_uva - slen1) & ~0xFull;
        for (uint64_t i = 0; i < slen1; i++) tk[OFF(arg1_uva) + i] = (uint8_t)arg1[i];
        lowstr = arg1_uva;
    }

    /* vector: argc, argv[0..argc-1], NULL, envp NULL, AT_NULL(key,val) */
    uint64_t nq = (uint64_t)argc + 5;
    uint64_t sp = (lowstr - nq * 8) & ~0xFull;          /* 16-aligned argc slot */
    uint64_t o  = OFF(sp);
    poke64(tk, o, (uint64_t)argc);                       /* argc            */
    poke64(tk, o + 8,  arg0_uva);                        /* argv[0]         */
    uint64_t k = o + 16;
    if (arg1) { poke64(tk, k, arg1_uva); k += 8; }       /* argv[1]         */
    poke64(tk, k,      0); k += 8;                        /* argv terminator */
    poke64(tk, k,      0); k += 8;                        /* envp terminator */
    poke64(tk, k,      0); k += 8;                        /* auxv AT_NULL k  */
    poke64(tk, k,      0);                                /* auxv AT_NULL v  */
    #undef OFF
    return sp;
}

/* --------------------------------------------------------------- exec ------*/
int proc_exec(const char *path) { return proc_exec_argv(path, 0); }

int proc_exec_argv(const char *path, const char *arg1) {
    file *f = vfs_open(path, O_RDONLY);
    if (!f) { kprintf("[exec] %s: not found\n", path); return -1; }

    vstat st;
    if (vfs_fstat(f, &st) != 0 || st.size == 0) { vfs_close(f); return -1; }
    uint8_t *image = (uint8_t *)kmalloc(st.size);
    if (!image) { vfs_close(f); return -1; }
    int64_t got = vfs_read(f, image, st.size);
    vfs_close(f);
    if (got < 0 || (uint64_t)got != st.size) { kfree(image); return -1; }

    uint64_t cr3 = vmm_new_address_space();
    if (!cr3) { kfree(image); return -1; }

    uint64_t entry = 0, brk_end = 0;
    if (elf_load(cr3, image, st.size, &entry, &brk_end) != 0) {
        kprintf("[exec] %s: bad ELF\n", path);
        kfree(image);
        return -1;
    }
    kfree(image);

    uint64_t ustack = setup_user_stack(cr3, path, arg1);
    if (!ustack) return -1;

    proc_t *p = (proc_t *)kmalloc(sizeof(proc_t));
    if (!p) return -1;
    memset(p, 0, sizeof(*p));
    p->pid       = next_pid++;
    p->cr3       = cr3;
    p->brk_start = p->brk_cur = brk_end;
    p->mmap_cur  = USER_MMAP_BASE;
    p->alive     = 1;
    strncpy(p->name, path, sizeof p->name);

    /* stdin/stdout/stderr -> /dev/console */
    p->fds[0] = vfs_open("/dev/console", O_RDONLY);
    p->fds[1] = vfs_open("/dev/console", O_WRONLY);
    p->fds[2] = vfs_open("/dev/console", O_WRONLY);

    if (sched_add_user(entry, ustack, cr3, p, p->name) < 0) { kfree(p); return -1; }
    kprintf("[exec] %s pid=%d entry=0x%lx brk=0x%lx\n", path, p->pid, entry, brk_end);
    return p->pid;
}

/* --------------------------------------------------------------- exit ------*/
void proc_exit(int code) {
    g_fb_exclusive = 0;          /* if a fb-grabbing program died, free the panel */
    proc_t *p = proc_current();
    if (p) {
        p->exit_code = code;
        p->alive = 0;
        for (int i = 0; i < PROC_MAX_FD; i++)
            if (p->fds[i]) { vfs_close(p->fds[i]); p->fds[i] = 0; }
        kprintf("[exit] pid=%d code=%d\n", p->pid, code);
    }
    sched_exit_current();                 /* scheduler will never resume us */
    __asm__ volatile("sti");
    for (;;) __asm__ volatile("hlt");     /* park until the next tick reaps */
}
