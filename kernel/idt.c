#include <stdint.h>
#include "interrupts.h"

struct idt_entry {
    uint16_t off_lo;
    uint16_t sel;
    uint8_t  ist;
    uint8_t  flags;
    uint16_t off_mid;
    uint32_t off_hi;
    uint32_t zero;
} __attribute__((packed));

struct idt_ptr { uint16_t limit; uint64_t base; } __attribute__((packed));

static struct idt_entry idt[256];
static struct idt_ptr   idtp;

extern uint64_t isr_stub_table[256];   /* from isr.asm */

static void set_gate(int n, uint64_t handler) {
    idt[n].off_lo  = handler & 0xFFFF;
    idt[n].sel     = 0x08;              /* kernel code selector */
    idt[n].ist     = 0;
    idt[n].flags   = 0x8E;             /* present, DPL0, 64-bit interrupt gate */
    idt[n].off_mid = (handler >> 16) & 0xFFFF;
    idt[n].off_hi  = (handler >> 32) & 0xFFFFFFFF;
    idt[n].zero    = 0;
}

void idt_init(void) {
    for (int i = 0; i < 256; i++) set_gate(i, isr_stub_table[i]);
    idtp.limit = sizeof(idt) - 1;
    idtp.base  = (uint64_t)idt;
    __asm__ volatile("lidt %0" :: "m"(idtp) : "memory");
}

/* Load the already-built IDT on the calling CPU (application processors). */
void idt_load(void) {
    __asm__ volatile("lidt %0" :: "m"(idtp) : "memory");
}
