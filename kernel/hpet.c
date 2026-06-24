#include <stdint.h>
#include "hpet.h"
#include "acpi.h"
#include "mm.h"
#include "mmio.h"
#include "kprintf.h"

/* HPET register block (memory mapped). */
#define HPET_CAP      0x000     /* general capabilities + ID (period in bits 32..63) */
#define HPET_CONFIG   0x010     /* general configuration (bit0 = enable) */
#define HPET_COUNTER  0x0F0     /* 64-bit main counter */

static volatile uint8_t *g_hpet;
static uint64_t g_period_fs;     /* counter period in femtoseconds (10^-15 s) */
static uint64_t g_base;          /* counter value captured at init */

static uint64_t hpet_read64(uint32_t off) {
    /* main counter is 64-bit; read directly via a 64-bit access. */
    return *(volatile uint64_t *)(g_hpet + off);
}
static void hpet_write64(uint32_t off, uint64_t v) {
    *(volatile uint64_t *)(g_hpet + off) = v;
}

int hpet_present(void) { return g_hpet != 0; }

int hpet_init(void) {
    const struct acpi_info *ai = acpi_get();
    if (!ai->present || !ai->hpet_addr) return -1;

    g_hpet = (volatile uint8_t *)P2V(ai->hpet_addr);

    uint64_t cap = hpet_read64(HPET_CAP);
    g_period_fs = cap >> 32;                 /* fs per tick */
    if (g_period_fs == 0 || g_period_fs > 100000000ull) { g_hpet = 0; return -1; }

    /* enable the main counter */
    uint64_t cfg = hpet_read64(HPET_CONFIG);
    hpet_write64(HPET_CONFIG, cfg | 1);
    g_base = hpet_read64(HPET_COUNTER);

    uint64_t hz = 1000000000000000ull / g_period_fs;
    kprintf("[ok] HPET @0x%lx, %lu Hz (period %lu fs)\n", ai->hpet_addr, hz, g_period_fs);
    return 0;
}

uint64_t hpet_ns(void) {
    if (!g_hpet) return 0;
    uint64_t ticks = hpet_read64(HPET_COUNTER) - g_base;
    /* ns = ticks * period_fs / 1e6 ; split to limit overflow */
    return (ticks / 1000000ull) * g_period_fs + ((ticks % 1000000ull) * g_period_fs) / 1000000ull;
}

uint64_t hpet_us(void) { return hpet_ns() / 1000ull; }

void hpet_delay_us(uint64_t us) {
    if (!g_hpet) { for (volatile uint64_t i = 0; i < us * 200; i++) __asm__ volatile("pause"); return; }
    uint64_t target = hpet_us() + us;
    while (hpet_us() < target) __asm__ volatile("pause");
}
