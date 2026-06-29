#pragma once
#include <stdint.h>

/* Register frame pushed by the ISR stubs (see kernel/isr.asm). Field order
 * mirrors the push order exactly. */
struct registers {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rax, rbx, rcx, rdx, rbp, rsi, rdi;
    uint64_t int_no, err_code;
    uint64_t rip, cs, rflags, rsp, ss;
};

typedef void (*irq_handler_t)(struct registers *);

void idt_init(void);
void idt_load(void);                    /* load the built IDT on an AP */
void irq_install(int irq, irq_handler_t h);

/* Register a handler for a Message-Signalled Interrupt vector. MSI vectors live
 * in 0x70..0x7F, outside the legacy IRQ window (32..47), so device MSIs never
 * collide with the timer/keyboard/etc. isr_handler EOIs the local APIC. */
#define MSI_VECTOR_BASE 0x70
void msi_install(uint8_t vector, irq_handler_t h);

/* Tick hook: called on every IRQ0 (timer) and may return a different register
 * frame to continue with, which is how the preemptive scheduler switches
 * tasks. isr_common reloads RSP from isr_handler's return value. */
typedef struct registers *(*tick_hook_t)(struct registers *);
void irq_set_tick_hook(tick_hook_t h);

struct registers *isr_handler(struct registers *r);   /* called from asm; returns frame to resume */
