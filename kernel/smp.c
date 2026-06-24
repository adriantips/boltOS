#include <stdint.h>
#include "smp.h"
#include "acpi.h"
#include "apic.h"
#include "gdt.h"
#include "interrupts.h"
#include "vmm.h"
#include "pmm.h"
#include "mm.h"
#include "kheap.h"
#include "hpet.h"
#include "io.h"
#include "kprintf.h"
#include "string.h"

/* Trampoline blob embedded by objcopy (kernel/ap_boot.asm -> build/ap_boot.bin). */
extern const uint8_t _binary_ap_boot_bin_start[];
extern const uint8_t _binary_ap_boot_bin_end[];

#define TRAMPOLINE_PHYS 0x8000
#define AP_PARAM_PHYS   0x8F00          /* must match ap_boot.asm */
#define AP_STACK_SIZE   (32 * 1024)

static struct percpu  g_cpus[SMP_MAX_CPUS];
static int            g_cpu_count = 1;  /* BSP counts immediately */
static volatile int   g_online    = 1;
static volatile int   g_ap_ack    = 0;  /* handshake for the AP currently booting */

static void set_gs_base(void *p) {
    uint64_t v = (uint64_t)p;
    /* IA32_GS_BASE = 0xC0000101 */
    __asm__ volatile("wrmsr" :: "a"((uint32_t)v), "d"((uint32_t)(v >> 32)), "c"(0xC0000101u));
}

struct percpu *this_cpu(void) {
    struct percpu *p;
    __asm__ volatile("mov %%gs:0, %0" : "=r"(p));   /* percpu.self at offset 0 */
    return p;
}
uint32_t smp_cpu_index(void) { return this_cpu()->cpu_index; }
int smp_cpu_count(void) { return g_cpu_count; }

/* 64-bit C entry the trampoline jumps to. RDI = cpu index. Never returns. */
void ap_entry64(uint64_t idx) {
    struct percpu *pc = &g_cpus[idx];
    gdt_load_ap();
    idt_load();
    set_gs_base(pc);
    lapic_enable_ap();

    pc->online = 1;
    pc->lapic_id = lapic_id();
    __atomic_fetch_add(&g_online, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_ap_ack, 1, __ATOMIC_SEQ_CST);

    /* Interrupt-driven idle. The APIC timer ticks here too; a future scheduler
     * can pull runnable threads from the global queue under a lock. */
    sti();
    for (;;) {
        pc->idle_ticks++;
        __asm__ volatile("hlt");
    }
}

static void udelay(uint64_t us) {
    if (hpet_present()) hpet_delay_us(us);
    else for (volatile uint64_t i = 0; i < us * 100; i++) __asm__ volatile("pause");
}

void smp_init(void) {
    const struct acpi_info *ai = acpi_get();
    if (!ai->present || !apic_active() || ai->cpu_count <= 1) {
        kprintf("[smp] single processor (BSP only)\n");
        /* still set up the BSP per-CPU block */
        g_cpus[0] = (struct percpu){0};
        g_cpus[0].self = (uint64_t)&g_cpus[0];
        g_cpus[0].is_bsp = 1;
        g_cpus[0].online = 1;
        g_cpus[0].lapic_id = lapic_id();
        set_gs_base(&g_cpus[0]);
        return;
    }

    uint8_t bsp = lapic_id();

    /* BSP per-CPU block. */
    g_cpus[0] = (struct percpu){0};
    g_cpus[0].self = (uint64_t)&g_cpus[0];
    g_cpus[0].is_bsp = 1;
    g_cpus[0].online = 1;
    g_cpus[0].lapic_id = bsp;
    set_gs_base(&g_cpus[0]);

    /* copy trampoline to low memory (identity-mapped, < 1 MiB). */
    uint64_t tlen = (uint64_t)(_binary_ap_boot_bin_end - _binary_ap_boot_bin_start);
    memcpy(P2V(TRAMPOLINE_PHYS), _binary_ap_boot_bin_start, tlen);

    uint64_t *param = (uint64_t *)P2V(AP_PARAM_PHYS);
    uint64_t cr3 = vmm_kernel_pml4();

    int idx = 1;
    for (int i = 0; i < ai->cpu_count && idx < SMP_MAX_CPUS; i++) {
        uint8_t id = ai->cpu_lapic_id[i];
        if (id == bsp) continue;

        void *stack = kmalloc(AP_STACK_SIZE);
        if (!stack) break;
        struct percpu *pc = &g_cpus[idx];
        *pc = (struct percpu){0};
        pc->self = (uint64_t)pc;
        pc->cpu_index = (uint32_t)idx;
        pc->lapic_id = id;
        pc->kernel_stack = (uint64_t)stack + AP_STACK_SIZE;

        /* fill the trampoline parameter block */
        param[0] = cr3;
        param[1] = pc->kernel_stack;
        param[2] = (uint64_t)&ap_entry64;
        param[3] = (uint64_t)idx;

        g_ap_ack = 0;

        /* INIT-SIPI-SIPI */
        lapic_send_init(id);
        udelay(10000);                       /* 10 ms */
        lapic_send_sipi(id, TRAMPOLINE_PHYS >> 12);
        udelay(200);
        if (!__atomic_load_n(&g_ap_ack, __ATOMIC_SEQ_CST)) {
            lapic_send_sipi(id, TRAMPOLINE_PHYS >> 12);
        }

        /* wait up to ~100 ms for the AP to check in */
        for (int t = 0; t < 1000 && !__atomic_load_n(&g_ap_ack, __ATOMIC_SEQ_CST); t++)
            udelay(100);

        if (__atomic_load_n(&g_ap_ack, __ATOMIC_SEQ_CST)) {
            g_cpu_count++;
            idx++;
        } else {
            kprintf("[smp] CPU lapic %u did not start\n", id);
        }
    }

    kprintf("[ok] SMP: %d/%d CPUs online\n", g_cpu_count, ai->cpu_count);
}
