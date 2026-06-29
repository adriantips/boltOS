#include <stdint.h>
#include "interrupts.h"
#include "kprintf.h"
#include "pic.h"
#include "apic.h"
#include "io.h"

static irq_handler_t handlers[16];
static irq_handler_t msi_handlers[16];      /* vectors 0x70..0x7F */
static tick_hook_t   tick_hook;

void irq_install(int irq, irq_handler_t h) {
    if (irq >= 0 && irq < 16) handlers[irq] = h;
}

void msi_install(uint8_t vector, irq_handler_t h) {
    if (vector >= MSI_VECTOR_BASE && vector < MSI_VECTOR_BASE + 16)
        msi_handlers[vector - MSI_VECTOR_BASE] = h;
}

void irq_set_tick_hook(tick_hook_t h) { tick_hook = h; }

static const char *exc[32] = {
    "Divide-by-zero", "Debug", "NMI", "Breakpoint", "Overflow",
    "BOUND range", "Invalid opcode", "Device not available", "Double fault",
    "Coproc segment", "Invalid TSS", "Segment not present", "Stack-segment",
    "General protection", "Page fault", "Reserved", "x87 FP",
    "Alignment check", "Machine check", "SIMD FP", "Virtualization",
    "Control protection", "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved", "Hypervisor", "VMM comm", "Security", "Reserved"
};

static inline uint64_t read_cr3(void) { uint64_t v; __asm__ volatile("mov %%cr3,%0":"=r"(v)); return v; }

/* Walk the saved RBP frame chain and print return addresses. The kernel keeps
 * frame pointers for non-leaf functions; bogus links are filtered by requiring
 * a forward-moving, 8-aligned RBP within a stack-sized window, so a corrupt
 * stack can't send us reading wild memory and triple-faulting the dump. */
/* A frame pointer is only safe to dereference if it lands in mapped memory:
 * either identity-mapped low RAM or the higher-half kernel window (where this
 * kernel's stacks live). Anything else ends the walk rather than risking a
 * fault inside the dump that would triple-fault the machine. */
static int rbp_ok(uint64_t p) {
    if (p & 7) return 0;
    if (p >= 0x1000 && p < 0x100000000ull)   return 1;   /* low identity map */
    if (p >= 0xffffffff80000000ull)           return 1;   /* higher-half kernel */
    return 0;
}

static void backtrace(uint64_t rbp, uint64_t rip) {
    kprintf("    backtrace:\n      #0 0x%lx\n", rip);
    for (int i = 1; i <= 16 && rbp_ok(rbp); i++) {
        uint64_t next = ((uint64_t *)rbp)[0];
        uint64_t ret  = ((uint64_t *)rbp)[1];
        if (ret < 0x1000) break;
        kprintf("      #%d 0x%lx\n", i, ret);
        if (next <= rbp || next - rbp > 0x40000) break;   /* not a sane frame */
        rbp = next;
    }
}

/* Full crash dump: cause, every GPR, control/segment state, page-fault decode,
 * and a stack backtrace. Mirrors to screen and COM1, then halts. */
static void panic_dump(struct registers *r) {
    kprintf("\n\n================ KERNEL PANIC ================\n");
    kprintf("  CPU EXCEPTION #%lu", r->int_no);
    if (r->int_no < 32) kprintf(" (%s)", exc[r->int_no]);
    kprintf("   err=0x%lx\n", r->err_code);
    kprintf("  RIP=0x%016lx  CS=0x%lx  RFLAGS=0x%lx\n", r->rip, r->cs, r->rflags);
    kprintf("  RSP=0x%016lx  SS=0x%lx\n", r->rsp, r->ss);
    kprintf("  RAX=0x%016lx  RBX=0x%016lx\n", r->rax, r->rbx);
    kprintf("  RCX=0x%016lx  RDX=0x%016lx\n", r->rcx, r->rdx);
    kprintf("  RSI=0x%016lx  RDI=0x%016lx\n", r->rsi, r->rdi);
    kprintf("  RBP=0x%016lx  R8 =0x%016lx\n", r->rbp, r->r8);
    kprintf("  R9 =0x%016lx  R10=0x%016lx\n", r->r9,  r->r10);
    kprintf("  R11=0x%016lx  R12=0x%016lx\n", r->r11, r->r12);
    kprintf("  R13=0x%016lx  R14=0x%016lx  R15=0x%016lx\n", r->r13, r->r14, r->r15);
    kprintf("  CR2=0x%016lx  CR3=0x%016lx\n", read_cr2(), read_cr3());
    if (r->int_no == 14) {                                  /* page fault: decode err bits */
        uint64_t e = r->err_code;
        kprintf("  page fault: %s while %s in %s mode%s%s\n",
                (e & 1) ? "protection-violation" : "non-present page",
                (e & 2) ? "write" : "read",
                (e & 4) ? "user" : "kernel",
                (e & 8)  ? ", reserved-bit set" : "",
                (e & 16) ? ", instruction fetch" : "");
    }
    backtrace(r->rbp, r->rip);
    kprintf("  System halted.\n");
    kprintf("=============================================\n");
}

struct registers *isr_handler(struct registers *r) {
    if (r->int_no < 32) {
        panic_dump(r);
        for (;;) __asm__ volatile("cli; hlt");
    }

    if (r->int_no >= 32 && r->int_no < 48) {
        int irq = (int)r->int_no - 32;
        if (handlers[irq]) handlers[irq](r);
        if (apic_active()) lapic_eoi(); else pic_eoi(irq);
        /* preemptive reschedule on the timer tick; may return a new frame */
        if (irq == 0 && tick_hook) r = tick_hook(r);
    }

    /* Message-signalled interrupts (NVMe completions, xHCI events). Delivered
     * straight to the local APIC, so only it needs the EOI. */
    if (r->int_no >= MSI_VECTOR_BASE && r->int_no < MSI_VECTOR_BASE + 16) {
        irq_handler_t h = msi_handlers[r->int_no - MSI_VECTOR_BASE];
        if (h) h(r);
        if (apic_active()) lapic_eoi();
    }
    return r;
}
