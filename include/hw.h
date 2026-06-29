#pragma once
#include <stdint.h>

/* Low-level hardware introspection: CPUID, CMOS real-time clock, PCI config
 * space. All real reads of real silicon (works under QEMU). */

/* --- CPUID ---------------------------------------------------------------- */
static inline void cpuidx(uint32_t leaf, uint32_t sub,
                          uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile("cpuid"
                     : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                     : "a"(leaf), "c"(sub));
}
void hw_cpu_vendor(char out[13]);   /* leaf 0 vendor string */
void hw_cpu_brand(char out[49]);    /* leaves 0x80000002..4 brand string */
uint32_t hw_cpu_max_leaf(void);

/* --- CMOS real-time clock ------------------------------------------------- */
struct rtc_time { uint16_t year; uint8_t mon, day, hour, min, sec; };
void rtc_now(struct rtc_time *t);

/* --- PCI ------------------------------------------------------------------ */
struct pci_dev {
    uint8_t  bus, slot, func;
    uint16_t vendor, device;
    uint8_t  class, subclass, prog_if, revision;
};

/* A decoded base address register. is_mmio=1 -> memory-mapped (use .virt after
 * pci_map_bar); is_mmio=0 -> I/O port space (use .port with in/out). size is the
 * region length probed from the BAR. */
struct pci_bar {
    int      is_mmio;
    int      is_64bit;
    int      prefetch;
    uint64_t phys;          /* MMIO physical base, or I/O base in low bits  */
    uint64_t size;          /* region size in bytes                          */
    uint16_t port;          /* I/O port base (when !is_mmio)                 */
    volatile void *virt;    /* kernel pointer after pci_map_bar (MMIO only)  */
};

/* Enumerate PCI devices into buf (up to max); returns count found. */
int         pci_scan(struct pci_dev *buf, int max);
const char *pci_class_name(uint8_t cls);

/* Config-space access (mechanism #1). off is byte offset; 16/32 must be aligned. */
uint32_t    pci_cfg_read32 (uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);
uint16_t    pci_cfg_read16 (uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);
uint8_t     pci_cfg_read8  (uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);
void        pci_cfg_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint32_t v);
void        pci_cfg_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint16_t v);
void        pci_cfg_write8 (uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint8_t  v);

/* Per-device convenience wrappers (forward the bus/slot/func triple). */
uint32_t    pci_read32 (const struct pci_dev *d, uint8_t off);
uint16_t    pci_read16 (const struct pci_dev *d, uint8_t off);
void        pci_write32(const struct pci_dev *d, uint8_t off, uint32_t v);
void        pci_write16(const struct pci_dev *d, uint8_t off, uint16_t v);

/* Find first device matching vendor:device, or class/subclass (0xFF = wildcard
 * subclass). Returns 1 and fills *out on match, 0 otherwise. */
int  pci_find_by_id   (uint16_t vendor, uint16_t device, struct pci_dev *out);
int  pci_find_by_class(uint8_t cls, uint8_t subclass, struct pci_dev *out);

/* Decode BAR index 0..5 into *out (handles 64-bit BARs and size probing).
 * Returns 0 on success, -1 if the BAR is unused/invalid. */
int  pci_bar(const struct pci_dev *d, int idx, struct pci_bar *out);

/* Map an MMIO BAR into kernel-virtual space and set bar->virt. Returns the
 * pointer (NULL for I/O-space BARs). Uses the shared direct map so the mapping
 * is valid under any CR3 (IRQ-safe). */
volatile void *pci_map_bar(struct pci_bar *bar);

/* Command register helpers. */
void pci_enable_bus_master(const struct pci_dev *d); /* set BM + MEM + IO bits */
uint8_t pci_interrupt_line(const struct pci_dev *d); /* legacy IRQ from cfg 0x3C */

/* Enable MSI/MSI-X on the device, delivering to the given IDT vector on the
 * boot CPU. Prefers MSI-X (programs vector-table entry 0), falls back to MSI.
 * Returns 0 on success, -1 if the device exposes neither capability. Pair with
 * msi_install(vector, handler) to register the IRQ callback. */
int pci_msi_enable(const struct pci_dev *d, uint8_t vector);
