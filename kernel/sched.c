#include <stdint.h>
#include "sched.h"
#include "vmm.h"
#include "kheap.h"
#include "string.h"
#include "gdt.h"

#define MAXT    16
#define KSTACK  16384          /* per-thread kernel stack */

typedef struct {
    uint64_t     rsp;          /* saved register-frame pointer when not running */
    uint64_t     cr3;          /* address space (PML4 physical)                 */
    uint64_t     kstack_top;   /* ring0 stack for syscall/trap entry (user thr) */
    struct proc *proc;         /* owning process, 0 for kernel threads          */
    int          used;
    char         name[16];
    /* FPU/SSE state. Saved/restored by the ISR common path (isr.asm) around
     * every interrupt so a handler's XMM use never corrupts the interrupted
     * thread, and so context switches preserve float state. 16-byte aligned as
     * FXSAVE/FXRSTOR require. */
    _Alignas(16) uint8_t fxarea[512];
} kthread_t;

/* Lay down a valid initial FXSAVE image: x87 control word 0x037F (all
 * exceptions masked, 64-bit precision) and MXCSR 0x1F80 (all SSE exceptions
 * masked). Everything else zero. */
static void fpu_init_area(uint8_t *a) {
    memset(a, 0, 512);
    a[0] = 0x7F; a[1] = 0x03;          /* FCW  @ off 0  */
    a[24] = 0x80; a[25] = 0x1F;        /* MXCSR @ off 24 */
}

static kthread_t th[MAXT];
static int       cur      = 0;
static int       enabled  = 0;
static uint64_t  nswitch  = 0;

/* set by kernel/syscall.c; the SYSCALL prologue reads it to find its stack.
 * The scheduler points it at the running user thread's per-process stack. */
extern uint64_t  syscall_kstack_top;

/* Called from isr_common (kernel/isr.asm) bracketing the C handler. Built with
 * SSE codegen disabled so the functions themselves never touch XMM — they only
 * issue the raw fxsave/fxrstor against the current thread's save area, which
 * isr_handler() may have re-pointed by switching `cur`. */
__attribute__((target("no-sse")))
void fpu_save_current(void) {
    __asm__ volatile("fxsave (%0)" :: "r"(th[cur].fxarea) : "memory");
}
__attribute__((target("no-sse")))
void fpu_restore_current(void) {
    __asm__ volatile("fxrstor (%0)" :: "r"(th[cur].fxarea) : "memory");
}

static int slot_alloc(void) {
    for (int i = 0; i < MAXT; i++) if (!th[i].used) return i;
    return -1;
}

void sched_init(void) {
    th[0].used = 1;
    th[0].cr3  = vmm_kernel_pml4();
    th[0].proc = 0;
    strncpy(th[0].name, "kmain", sizeof th[0].name);   /* current context */
    fpu_init_area(th[0].fxarea);
    cur = 0;
    irq_set_tick_hook(schedule);
    enabled = 1;
}

int sched_add(void (*entry)(void), const char *name) {
    int i = slot_alloc();
    if (i < 0) return -1;
    uint8_t *stack = (uint8_t *)kmalloc(KSTACK);
    if (!stack) return -1;

    /* Hand-craft an initial register frame at the top of the new stack so the
     * ISR epilogue (pop GPRs; iretq) starts the thread at entry() in ring 0. */
    uint64_t top = ((uint64_t)stack + KSTACK) & ~0xFull;
    struct registers *f = (struct registers *)(top - sizeof(struct registers));
    memset(f, 0, sizeof *f);
    f->rip    = (uint64_t)entry;
    f->cs     = 0x08;          /* kernel code */
    f->rflags = 0x202;         /* IF set */
    f->rsp    = top;           /* thread's own stack when running */
    f->ss     = 0x10;          /* kernel data */

    th[i].rsp        = (uint64_t)f;
    th[i].cr3        = vmm_kernel_pml4();
    th[i].kstack_top = top;
    th[i].proc       = 0;
    th[i].used       = 1;
    fpu_init_area(th[i].fxarea);
    strncpy(th[i].name, name, sizeof th[i].name);
    return i;
}

int sched_add_user(uint64_t rip, uint64_t ustack_top, uint64_t cr3,
                   struct proc *p, const char *name) {
    int i = slot_alloc();
    if (i < 0) return -1;
    uint8_t *stack = (uint8_t *)kmalloc(KSTACK);    /* kernel stack for traps/syscalls */
    if (!stack) return -1;

    /* initial frame is already a ring-3 frame: the ISR epilogue's iretq performs
     * the ring0->ring3 transition straight into the user program. */
    uint64_t top = ((uint64_t)stack + KSTACK) & ~0xFull;
    struct registers *f = (struct registers *)(top - sizeof(struct registers));
    memset(f, 0, sizeof *f);
    f->rip    = rip;
    f->cs     = 0x23;          /* user code, RPL 3 */
    f->rflags = 0x202;         /* IF set */
    f->rsp    = ustack_top;
    f->ss     = 0x1B;          /* user data, RPL 3 */

    th[i].rsp        = (uint64_t)f;
    th[i].cr3        = cr3;
    th[i].kstack_top = top;
    th[i].proc       = p;
    th[i].used       = 1;
    fpu_init_area(th[i].fxarea);
    strncpy(th[i].name, name, sizeof th[i].name);
    return i;
}

struct registers *schedule(struct registers *r) {
    if (!enabled) return r;
    if (th[cur].used) th[cur].rsp = (uint64_t)r;   /* save outgoing (if still alive) */

    /* round-robin to the next live slot (slots may have holes from exit) */
    int n = cur;
    for (int k = 0; k < MAXT; k++) {
        n = (n + 1) % MAXT;
        if (th[n].used) break;
    }
    if (n == cur && th[cur].used) return r;        /* sole runnable thread */

    cur = n;
    nswitch++;
    vmm_switch(th[cur].cr3);                        /* activate incoming address space */
    if (th[cur].proc) {                             /* ring-3: arm its kernel stack */
        tss_set_rsp0(th[cur].kstack_top);
        syscall_kstack_top = th[cur].kstack_top;
    }
    return (struct registers *)th[cur].rsp;         /* restore incoming context */
}

void sched_exit_current(void) {
    if (cur != 0) th[cur].used = 0;                /* never retire kmain (slot 0) */
}

struct proc *sched_current_proc(void) { return th[cur].proc; }

int sched_count(void) {
    int n = 0;
    for (int i = 0; i < MAXT; i++) if (th[i].used) n++;
    return n;
}
int         sched_current(void)   { return cur; }
const char *sched_name(int i)     { return (i >= 0 && i < MAXT && th[i].used) ? th[i].name : ""; }
uint64_t    sched_switches(void)  { return nswitch; }
