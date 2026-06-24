#include <stdint.h>
#include "apic.h"
#include "acpi.h"
#include "mm.h"
#include "io.h"
#include "mmio.h"
#include "kprintf.h"

/* ===========================================================================
 *  Local APIC register offsets (MMIO, one 32-bit value per 16-byte slot).
 * ===========================================================================*/
#define LAPIC_ID        0x020
#define LAPIC_VER       0x030
#define LAPIC_TPR       0x080
#define LAPIC_EOI       0x0B0
#define LAPIC_LDR       0x0D0
#define LAPIC_DFR       0x0E0
#define LAPIC_SVR       0x0F0
#define LAPIC_ESR       0x280
#define LAPIC_ICRLO     0x300
#define LAPIC_ICRHI     0x310
#define LAPIC_LVT_TIMER 0x320
#define LAPIC_LVT_LINT0 0x350
#define LAPIC_LVT_LINT1 0x360
#define LAPIC_LVT_ERR   0x370
#define LAPIC_TIMER_ICR 0x380
#define LAPIC_TIMER_CCR 0x390
#define LAPIC_TIMER_DIV 0x3E0

#define SVR_ENABLE      0x100
#define LVT_MASKED      0x10000
#define TIMER_PERIODIC  0x20000

#define ICR_INIT        0x00000500
#define ICR_STARTUP     0x00000600
#define ICR_LEVEL       0x00008000
#define ICR_ASSERT      0x00004000
#define ICR_DEASSERT    0x00000000
#define ICR_DELIV_PEND  0x00001000

static volatile uint8_t *g_lapic;        /* mapped local APIC base */
static volatile uint8_t *g_ioapic;       /* mapped IO-APIC #0 base */
static uint32_t g_ioapic_gsi_base;
static int g_active;
static uint64_t g_timer_hz;

static inline uint32_t lr(uint32_t off)            { return mmio_read32(g_lapic, off); }
static inline void     lw(uint32_t off, uint32_t v){ mmio_write32(g_lapic, off, v); }

int  apic_active(void) { return g_active; }
uint8_t lapic_id(void) { return g_lapic ? (uint8_t)(lr(LAPIC_ID) >> 24) : 0; }
uint64_t lapic_timer_hz(void) { return g_timer_hz; }

void lapic_eoi(void) { if (g_lapic) lw(LAPIC_EOI, 0); }

/* Per-CPU local APIC enable, called on each application processor. The LAPIC
 * MMIO window is shared but the registers are CPU-local. */
void lapic_enable_ap(void) {
    if (!g_lapic) return;
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0x1Bu));
    lo |= (1 << 11);
    __asm__ volatile("wrmsr" :: "a"(lo), "d"(hi), "c"(0x1Bu));
    lw(LAPIC_TPR, 0);
    lw(LAPIC_SVR, SVR_ENABLE | 0xFF);
}

/* ---- IO-APIC index/data ------------------------------------------------- */
static uint32_t ioapic_read(uint32_t reg) {
    mmio_write32(g_ioapic, 0x00, reg);
    return mmio_read32(g_ioapic, 0x10);
}
static void ioapic_write(uint32_t reg, uint32_t val) {
    mmio_write32(g_ioapic, 0x00, reg);
    mmio_write32(g_ioapic, 0x10, val);
}

void ioapic_route_irq(uint8_t irq, uint8_t vector) {
    if (!g_ioapic) return;
    uint16_t flags = 0;
    uint32_t gsi = acpi_irq_to_gsi(irq, &flags);
    uint32_t idx = gsi - g_ioapic_gsi_base;
    uint32_t lo = vector;                 /* fixed delivery, phys, to dest below */
    /* polarity: flags bits 0-1 (1=high,3=low); trigger: bits 2-3 (1=edge,3=level) */
    if ((flags & 0x3) == 0x3) lo |= (1 << 13);    /* active low */
    if ((flags & 0xC) == 0xC) lo |= (1 << 15);    /* level triggered */
    uint32_t hi = (uint32_t)lapic_id() << 24;
    ioapic_write(0x10 + idx * 2 + 1, hi);
    ioapic_write(0x10 + idx * 2,     lo);         /* unmasked (bit16 clear) */
}

void ioapic_mask_irq(uint8_t irq, int masked) {
    if (!g_ioapic) return;
    uint32_t gsi = acpi_irq_to_gsi(irq, 0);
    uint32_t idx = gsi - g_ioapic_gsi_base;
    uint32_t lo = ioapic_read(0x10 + idx * 2);
    if (masked) lo |= (1 << 16); else lo &= ~(1u << 16);
    ioapic_write(0x10 + idx * 2, lo);
}

/* ---- IPI ---------------------------------------------------------------- */
static void icr_wait(void) { while (lr(LAPIC_ICRLO) & ICR_DELIV_PEND) __asm__ volatile("pause"); }

void lapic_send_init(uint8_t apic_id) {
    lw(LAPIC_ICRHI, (uint32_t)apic_id << 24);
    lw(LAPIC_ICRLO, ICR_INIT | ICR_LEVEL | ICR_ASSERT);
    icr_wait();
}
void lapic_send_sipi(uint8_t apic_id, uint8_t vector) {
    lw(LAPIC_ICRHI, (uint32_t)apic_id << 24);
    lw(LAPIC_ICRLO, ICR_STARTUP | vector);
    icr_wait();
}
void lapic_send_ipi(uint8_t apic_id, uint8_t vector) {
    lw(LAPIC_ICRHI, (uint32_t)apic_id << 24);
    lw(LAPIC_ICRLO, (uint32_t)vector | ICR_ASSERT);
    icr_wait();
}
void lapic_broadcast_ipi(uint8_t vector) {
    lw(LAPIC_ICRHI, 0);
    lw(LAPIC_ICRLO, (uint32_t)vector | ICR_ASSERT | (0x3 << 18)); /* all excl self */
    icr_wait();
}

void apic_msi_compose(uint8_t vector, uint32_t *addr_lo, uint32_t *data) {
    if (addr_lo) *addr_lo = 0xFEE00000u | ((uint32_t)lapic_id() << 12);
    if (data)    *data    = vector;       /* edge, fixed, deassert */
}

/* ---- PIC masking -------------------------------------------------------- */
static void pic_disable(void) {
    outb(0x21, 0xFF);     /* mask every line on master + slave 8259 */
    outb(0xA1, 0xFF);
}

/* ---- APIC timer calibration via PIT channel 2 one-shot ------------------ */
/* Channel 2 gate is port 0x61 bit0; its output is reflected in bit5. We arm a
 * 10 ms one-shot (mode 0) and count how far the APIC timer falls in that span. */
static uint32_t calibrate_timer_ticks_per_10ms(void) {
    lw(LAPIC_TIMER_DIV, 0x3);             /* divide by 16 */

    /* PIT ch2: count for 10 ms. 1193182 Hz -> 11932 ticks ~= 10 ms. */
    uint8_t p61 = inb(0x61);
    outb(0x61, (uint8_t)((p61 & ~0x02) | 0x01));   /* gate on, speaker off */
    outb(0x43, 0xB0);                     /* ch2, lo/hi, mode 0, binary */
    uint16_t count = 11932;
    outb(0x42, (uint8_t)(count & 0xFF));
    outb(0x42, (uint8_t)(count >> 8));

    lw(LAPIC_TIMER_ICR, 0xFFFFFFFFu);     /* start APIC timer counting down */

    /* wait for PIT ch2 output (bit5 of 0x61) to go high */
    while (!(inb(0x61) & 0x20)) __asm__ volatile("pause");

    uint32_t cur = lr(LAPIC_TIMER_CCR);
    lw(LAPIC_LVT_TIMER, LVT_MASKED);      /* stop timer */
    return 0xFFFFFFFFu - cur;             /* APIC ticks elapsed in ~10 ms */
}

int apic_init(void) {
    const struct acpi_info *ai = acpi_get();
    if (!ai->present || !ai->lapic_addr) {
        kprintf("[apic] no ACPI/LAPIC; staying on PIC\n");
        return -1;
    }

    /* CPUID.01h:EDX bit9 = APIC present. */
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a),"=b"(b),"=c"(c),"=d"(d) : "a"(1u));
    if (!(d & (1 << 9))) { kprintf("[apic] CPU lacks APIC\n"); return -1; }

    g_lapic = (volatile uint8_t *)P2V(ai->lapic_addr);

    /* Hardware-enable the local APIC via IA32_APIC_BASE MSR (0x1B) bit 11. */
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0x1Bu));
    lo |= (1 << 11);
    __asm__ volatile("wrmsr" :: "a"(lo), "d"(hi), "c"(0x1Bu));

    pic_disable();

    /* Flat delivery + spurious vector 0xFF, software-enable the APIC. */
    lw(LAPIC_DFR, 0xFFFFFFFFu);
    lw(LAPIC_LDR, (lr(LAPIC_LDR) & 0x00FFFFFF) | (1 << 24));
    lw(LAPIC_TPR, 0);
    lw(LAPIC_LVT_LINT0, LVT_MASKED);
    lw(LAPIC_LVT_LINT1, LVT_MASKED);
    lw(LAPIC_SVR, SVR_ENABLE | 0xFF);

    /* IO-APIC #0. */
    if (ai->ioapic_count) {
        g_ioapic = (volatile uint8_t *)P2V(ai->ioapic[0].address);
        g_ioapic_gsi_base = ai->ioapic[0].gsi_base;
        /* mask every redirection entry first */
        uint32_t ver = ioapic_read(0x01);
        int maxred = ((ver >> 16) & 0xFF) + 1;
        for (int i = 0; i < maxred; i++) {
            ioapic_write(0x10 + i * 2,     (1 << 16));   /* masked */
            ioapic_write(0x10 + i * 2 + 1, 0);
        }
        /* route the lines BoltOS uses: keyboard, mouse, COM1, ATA primary. */
        ioapic_route_irq(1,  33);    /* PS/2 keyboard -> vector 33 */
        ioapic_route_irq(12, 44);    /* PS/2 mouse    -> vector 44 */
        ioapic_route_irq(4,  36);    /* COM1                       */
        ioapic_route_irq(14, 46);    /* ATA primary                */
    }

    /* Calibrate + start the periodic APIC timer at 1000 Hz on vector 32. */
    uint32_t t10 = calibrate_timer_ticks_per_10ms();
    uint32_t per_tick = t10 / 10;            /* ticks per 1 ms (1000 Hz) */
    if (per_tick == 0) per_tick = 1;
    g_timer_hz = (uint64_t)t10 * 100;        /* APIC timer input freq estimate */

    lw(LAPIC_TIMER_DIV, 0x3);                /* divide by 16 */
    lw(LAPIC_LVT_TIMER, 32 | TIMER_PERIODIC);
    lw(LAPIC_TIMER_ICR, per_tick);

    g_active = 1;
    kprintf("[ok] APIC: LAPIC id %u, IO-APIC%s, timer %u ticks/ms\n",
            lapic_id(), g_ioapic ? " routed" : " absent", per_tick);
    return 0;
}
