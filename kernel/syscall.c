#include <stdint.h>
#include "syscall.h"
#include "serial.h"
#include "mm.h"
#include "vmm.h"
#include "pmm.h"
#include "vfs.h"
#include "proc.h"
#include "kprintf.h"
#include "framebuffer.h"
#include "keyboard.h"
#include "http.h"

#define IA32_EFER  0xC0000080
#define IA32_STAR  0xC0000081
#define IA32_LSTAR 0xC0000082
#define IA32_FMASK 0xC0000084

#define PG_UP(x) (((x) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))
#define PG_DN(x) ((x) & ~(PAGE_SIZE - 1))

static inline void wrmsr(uint32_t msr, uint64_t v) {
    __asm__ volatile("wrmsr" :: "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}
static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

/* Fallback kernel stack for the SYSCALL prologue. The scheduler repoints
 * syscall_kstack_top at the running process's own kernel stack on every
 * switch-in; this default only covers the (unused) pre-first-switch window. */
static uint8_t   syscall_stack[16384] __attribute__((aligned(16)));
uint64_t         syscall_kstack_top;     /* read by kernel/syscall.asm */
uint64_t         syscall_user_rsp;       /* scratch save of user RSP    */

void syscall_init(void) {
    syscall_kstack_top = (uint64_t)(syscall_stack + sizeof syscall_stack);

    /* STAR[63:48]=0x10 -> SYSRET SS=0x18|3, CS=0x20|3 (user data/code).
     * STAR[47:32]=0x08 -> SYSCALL CS=0x08, SS=0x10 (kernel code/data).      */
    wrmsr(IA32_STAR, ((uint64_t)0x10 << 48) | ((uint64_t)0x08 << 32));
    wrmsr(IA32_LSTAR, (uint64_t)syscall_entry);
    wrmsr(IA32_FMASK, 0x200);                       /* clear IF on entry */
    wrmsr(IA32_EFER, rdmsr(IA32_EFER) | 1);         /* EFER.SCE */
}

/* a user pointer must be non-null and below the kernel direct map */
static inline int uok(uint64_t p) { return p != 0 && p < PHYS_BASE; }

static uint64_t do_brk(proc_t *p, uint64_t target) {
    if (target == 0) return p->brk_cur;
    uint64_t old = p->brk_cur;
    if (target > old) {
        for (uint64_t va = PG_UP(old); va < PG_UP(target); va += PAGE_SIZE) {
            if (vmm_get_phys(p->cr3, va)) continue;
            uint64_t fr = pmm_alloc_frame();
            if (!fr) return old;                       /* OOM: leave break put */
            uint8_t *kv = (uint8_t *)P2V(fr);
            for (int i = 0; i < (int)PAGE_SIZE; i++) kv[i] = 0;
            if (vmm_map(p->cr3, va, fr, PTE_PRESENT | PTE_WRITE | PTE_USER) != 0) {
                pmm_free_frame(fr); return old;
            }
        }
    } else if (target < old) {
        for (uint64_t va = PG_UP(target); va < PG_UP(old); va += PAGE_SIZE) {
            uint64_t ph = vmm_get_phys(p->cr3, va);
            if (ph) { vmm_unmap(p->cr3, va); pmm_free_frame(PG_DN(ph)); }
        }
    }
    p->brk_cur = target;
    return target;
}

static uint64_t do_mmap(proc_t *p, uint64_t len, uint64_t prot) {
    if (len == 0) return (uint64_t)-1;
    uint64_t pages = (len + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t base  = p->mmap_cur;
    uint64_t flags = PTE_PRESENT | PTE_USER | ((prot & PROT_WRITE) ? PTE_WRITE : 0);
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t fr = pmm_alloc_frame();
        if (!fr) return (uint64_t)-1;
        uint8_t *kv = (uint8_t *)P2V(fr);
        for (int j = 0; j < (int)PAGE_SIZE; j++) kv[j] = 0;
        if (vmm_map(p->cr3, base + i * PAGE_SIZE, fr, flags) != 0) {
            pmm_free_frame(fr); return (uint64_t)-1;
        }
    }
    p->mmap_cur += pages * PAGE_SIZE;
    return base;
}

/* Claim the panel for a ring-3 renderer and report its geometry. No framebuffer
 * is mapped into the process: the renderer draws into its own RAM backbuffer and
 * hands it to SYS_FBPRESENT, so the kernel performs every VRAM write (the proven
 * fb_blit path) and is the sole arbiter of the panel -- there is no shared-VRAM
 * race with the compositor, which the g_fb_exclusive flag parks. */
static uint64_t do_fbinfo(proc_t *p, uint64_t uptr) {
    (void)p;
    if (!uok(uptr)) return (uint64_t)-1;
    if (!fb_present()) return (uint64_t)-1;             /* no panel */
    struct { uint64_t ptr; uint32_t w, h, pitch_px; } *u = (void *)uptr;
    u->ptr = 0; u->w = fb_width(); u->h = fb_height(); u->pitch_px = fb_width();
    g_fb_exclusive = 1;                                  /* compositor steps back */
    return 0;
}

/* Copy a ring-3 renderer's packed w*h xRGB backbuffer onto the panel via the
 * kernel's framebuffer path. The buffer must be fully inside the user half. */
static uint64_t do_fbpresent(uint64_t ubuf) {
    if (!g_fb_exclusive || !fb_present()) return (uint64_t)-1;
    uint64_t bytes = (uint64_t)fb_width() * fb_height() * 4;
    if (!uok(ubuf) || !uok(ubuf + bytes - 1)) return (uint64_t)-1;
    fb_blit((const uint32_t *)ubuf);
    return 0;
}

uint64_t syscall_dispatch(uint64_t num, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a5;
    proc_t *p = proc_current();

    switch (num) {
    case SYS_READ: {
        if (!p || !uok(a2)) return (uint64_t)-1;
        file *f = proc_fd_get(p, (int)a1);
        if (!f) return (uint64_t)-1;
        return (uint64_t)vfs_read(f, (void *)a2, a3);
    }
    case SYS_WRITE: {
        if (!p || !uok(a2)) return (uint64_t)-1;
        file *f = proc_fd_get(p, (int)a1);
        if (!f) return (uint64_t)-1;
        return (uint64_t)vfs_write(f, (const void *)a2, a3);
    }
    case SYS_OPEN: {
        if (!p || !uok(a1)) return (uint64_t)-1;
        file *f = vfs_open((const char *)a1, (int)a2);
        if (!f) return (uint64_t)-1;
        int fd = proc_fd_alloc(p, f);
        if (fd < 0) { vfs_close(f); return (uint64_t)-1; }
        return (uint64_t)fd;
    }
    case SYS_CLOSE:
        if (!p) return (uint64_t)-1;
        return (uint64_t)proc_fd_close(p, (int)a1);
    case SYS_FSTAT: {
        if (!p || !uok(a2)) return (uint64_t)-1;
        file *f = proc_fd_get(p, (int)a1);
        if (!f) return (uint64_t)-1;
        vstat st;
        if (vfs_fstat(f, &st) != 0) return (uint64_t)-1;
        uint64_t *u = (uint64_t *)a2;
        u[0] = st.size; u[1] = (uint64_t)st.is_dir;   /* {size, is_dir} */
        return 0;
    }
    case SYS_LSEEK: {
        if (!p) return (uint64_t)-1;
        file *f = proc_fd_get(p, (int)a1);
        if (!f) return (uint64_t)-1;
        return (uint64_t)vfs_lseek(f, (int64_t)a2, (int)a3);
    }
    case SYS_MMAP:
        if (!p) return (uint64_t)-1;
        return do_mmap(p, a2, a3);
    case SYS_BRK:
        if (!p) return (uint64_t)-1;
        return do_brk(p, a1);
    case SYS_YIELD:  return 0;
    case SYS_GETPID: return p ? (uint64_t)p->pid : 0;
    case SYS_EXIT:   proc_exit((int)a1);   /* no return */

    /* ---- device syscalls for the ring-3 browser -------------------------- */
    case SYS_FBINFO:
        if (!p) return (uint64_t)-1;
        return do_fbinfo(p, a1);
    case SYS_GETKEY:                          /* non-blocking; -1 if ring empty */
        return (uint64_t)(int64_t)kbd_trygetc();
    case SYS_HTTPGET: {                        /* (url, buf, cap, status*)      */
        if (!p || !uok(a1) || !uok(a2) || a3 == 0) return (uint64_t)-1;
        int status = 0;
        int n = http_get((const char *)a1, (char *)a2, (uint32_t)a3, &status, 0, 0);
        if (a4 && uok(a4)) *(int *)a4 = status;
        return (uint64_t)(int64_t)n;
    }
    case SYS_FBPRESENT:                        /* blit a ring-3 backbuffer       */
        return do_fbpresent(a1);
    case SYS_FBEND:                            /* hand the panel back to the GUI */
        g_fb_exclusive = 0;
        return 0;
    default:         return (uint64_t)-1;
    }
}
