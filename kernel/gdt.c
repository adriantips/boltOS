#include <stdint.h>
#include "gdt.h"

/* 7 slots: null, kcode, kdata, ucode, udata, then a 16-byte TSS descriptor. */
static uint64_t gdt[7];

struct gdt_ptr { uint16_t limit; uint64_t base; } __attribute__((packed));
static struct gdt_ptr gp;

typedef struct {
    uint32_t reserved0;
    uint64_t rsp0, rsp1, rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed)) tss_t;

static tss_t tss;
static uint8_t tss_stack[16384] __attribute__((aligned(16)));

static void set_tss(int idx, uint64_t base, uint32_t limit) {
    uint64_t lo = 0;
    lo |= (limit & 0xFFFFull);
    lo |= (base & 0xFFFFFFull) << 16;
    lo |= (uint64_t)0x89 << 40;                 /* present, type=available TSS */
    lo |= (uint64_t)((limit >> 16) & 0xF) << 48;
    lo |= ((base >> 24) & 0xFFull) << 56;
    gdt[idx]     = lo;
    gdt[idx + 1] = (base >> 32) & 0xFFFFFFFFull;
}

void gdt_init(void) {
    gdt[0] = 0;
    gdt[1] = 0x00AF9A000000FFFFull;   /* 0x08 kernel code */
    gdt[2] = 0x00CF92000000FFFFull;   /* 0x10 kernel data */
    /* SYSCALL/SYSRET ordering: SYSRET derives user SS = STAR[63:48]+8 and
     * user CS = STAR[63:48]+16, so user DATA must precede user CODE here. */
    gdt[3] = 0x00CFF2000000FFFFull;   /* 0x18 user data (DPL3) -> SS 0x1B */
    gdt[4] = 0x00AFFA000000FFFFull;   /* 0x20 user code (DPL3) -> CS 0x23 */

    uint8_t *p = (uint8_t *)&tss;
    for (unsigned i = 0; i < sizeof(tss); i++) p[i] = 0;
    tss.rsp0 = (uint64_t)(tss_stack + sizeof(tss_stack));
    tss.iomap_base = sizeof(tss);
    set_tss(5, (uint64_t)&tss, sizeof(tss) - 1);   /* selector 0x28 */

    gp.limit = sizeof(gdt) - 1;
    gp.base  = (uint64_t)gdt;
    __asm__ volatile("lgdt %0" :: "m"(gp) : "memory");

    __asm__ volatile(
        "mov $0x10, %%ax\n"
        "mov %%ax, %%ds\n mov %%ax, %%es\n mov %%ax, %%ss\n"
        "mov %%ax, %%fs\n mov %%ax, %%gs\n"
        "push $0x08\n"
        "lea 1f(%%rip), %%rax\n"
        "push %%rax\n"
        "lretq\n"
        "1:\n"
        ::: "rax", "memory");

    __asm__ volatile("ltr %%ax" :: "a"((uint16_t)0x28));
}

void tss_set_rsp0(uint64_t top) { tss.rsp0 = top; }

/* Load the shared GDT on an application processor and reload its segment
 * registers + CS. We deliberately skip LTR: a TSS may only be busy on one CPU,
 * and idle APs never take a privilege-level change that needs RSP0. */
void gdt_load_ap(void) {
    __asm__ volatile("lgdt %0" :: "m"(gp) : "memory");
    __asm__ volatile(
        "mov $0x10, %%ax\n"
        "mov %%ax, %%ds\n mov %%ax, %%es\n mov %%ax, %%ss\n"
        "mov %%ax, %%fs\n mov %%ax, %%gs\n"
        "push $0x08\n"
        "lea 1f(%%rip), %%rax\n"
        "push %%rax\n"
        "lretq\n"
        "1:\n"
        ::: "rax", "memory");
}
