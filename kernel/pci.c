#include <stdint.h>
#include "hw.h"
#include "io.h"
#include "mm.h"
#include "apic.h"
#include "kprintf.h"

/* ===========================================================================
 *  PCI configuration space (mechanism #1) + BAR decode / MMIO mapping.
 *
 *  Config access is the classic 0xCF8 (address) / 0xCFC (data) port pair.
 *  Reads/writes narrower than 32 bits operate on the containing aligned dword.
 * ===========================================================================*/
#define PCI_ADDR 0xCF8
#define PCI_DATA 0xCFC

static uint32_t cfg_addr(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    return (uint32_t)((1u << 31) | ((uint32_t)bus << 16) |
                      ((uint32_t)slot << 11) | ((uint32_t)func << 8) |
                      (off & 0xFC));
}

uint32_t pci_cfg_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    outl(PCI_ADDR, cfg_addr(bus, slot, func, off));
    return inl(PCI_DATA);
}
void pci_cfg_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint32_t v) {
    outl(PCI_ADDR, cfg_addr(bus, slot, func, off));
    outl(PCI_DATA, v);
}
uint16_t pci_cfg_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    uint32_t d = pci_cfg_read32(bus, slot, func, off);
    return (uint16_t)(d >> ((off & 2) * 8));
}
uint8_t pci_cfg_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    uint32_t d = pci_cfg_read32(bus, slot, func, off);
    return (uint8_t)(d >> ((off & 3) * 8));
}
void pci_cfg_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint16_t v) {
    uint32_t d = pci_cfg_read32(bus, slot, func, off);
    int sh = (off & 2) * 8;
    d = (d & ~(0xFFFFu << sh)) | ((uint32_t)v << sh);
    pci_cfg_write32(bus, slot, func, off, d);
}
void pci_cfg_write8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint8_t v) {
    uint32_t d = pci_cfg_read32(bus, slot, func, off);
    int sh = (off & 3) * 8;
    d = (d & ~(0xFFu << sh)) | ((uint32_t)v << sh);
    pci_cfg_write32(bus, slot, func, off, d);
}

uint32_t pci_read32(const struct pci_dev *d, uint8_t off) {
    return pci_cfg_read32(d->bus, d->slot, d->func, off);
}
uint16_t pci_read16(const struct pci_dev *d, uint8_t off) {
    return pci_cfg_read16(d->bus, d->slot, d->func, off);
}
void pci_write32(const struct pci_dev *d, uint8_t off, uint32_t v) {
    pci_cfg_write32(d->bus, d->slot, d->func, off, v);
}
void pci_write16(const struct pci_dev *d, uint8_t off, uint16_t v) {
    pci_cfg_write16(d->bus, d->slot, d->func, off, v);
}

/* ===========================================================================
 *  Enumeration
 * ===========================================================================*/
int pci_scan(struct pci_dev *buf, int max) {
    int n = 0;
    for (uint16_t bus = 0; bus < 8 && n < max; bus++) {
        for (uint8_t slot = 0; slot < 32 && n < max; slot++) {
            for (uint8_t func = 0; func < 8 && n < max; func++) {
                uint32_t id = pci_cfg_read32((uint8_t)bus, slot, func, 0x00);
                uint16_t vendor = (uint16_t)(id & 0xFFFF);
                if (vendor == 0xFFFF) {
                    if (func == 0) break;     /* no device in this slot */
                    continue;
                }
                uint32_t classreg = pci_cfg_read32((uint8_t)bus, slot, func, 0x08);
                struct pci_dev *p = &buf[n++];
                p->bus = (uint8_t)bus; p->slot = slot; p->func = func;
                p->vendor   = vendor;
                p->device   = (uint16_t)(id >> 16);
                p->revision = (uint8_t)(classreg & 0xFF);
                p->prog_if  = (uint8_t)((classreg >> 8) & 0xFF);
                p->subclass = (uint8_t)((classreg >> 16) & 0xFF);
                p->class    = (uint8_t)((classreg >> 24) & 0xFF);

                /* only probe extra functions on multi-function devices */
                if (func == 0) {
                    uint32_t hdr = pci_cfg_read32((uint8_t)bus, slot, 0, 0x0C);
                    if (!((hdr >> 16) & 0x80)) break;
                }
            }
        }
    }
    return n;
}

const char *pci_class_name(uint8_t cls) {
    switch (cls) {
    case 0x00: return "unclassified";
    case 0x01: return "mass storage";
    case 0x02: return "network";
    case 0x03: return "display";
    case 0x04: return "multimedia";
    case 0x05: return "memory";
    case 0x06: return "bridge";
    case 0x07: return "comm";
    case 0x08: return "system";
    case 0x09: return "input";
    case 0x0C: return "serial bus";
    case 0x0D: return "wireless";
    default:   return "device";
    }
}

int pci_find_by_id(uint16_t vendor, uint16_t device, struct pci_dev *out) {
    struct pci_dev buf[64];
    int n = pci_scan(buf, 64);
    for (int i = 0; i < n; i++)
        if (buf[i].vendor == vendor && buf[i].device == device) { *out = buf[i]; return 1; }
    return 0;
}

int pci_find_by_class(uint8_t cls, uint8_t subclass, struct pci_dev *out) {
    struct pci_dev buf[64];
    int n = pci_scan(buf, 64);
    for (int i = 0; i < n; i++)
        if (buf[i].class == cls && (subclass == 0xFF || buf[i].subclass == subclass)) {
            *out = buf[i]; return 1;
        }
    return 0;
}

/* ===========================================================================
 *  BAR decode + MMIO map
 *
 *  Command register (offset 0x04): bit0 I/O space, bit1 memory space, bit2 bus
 *  master. We clear I/O+memory decode while probing the BAR size (write all-ones,
 *  read back the writable mask) so a transient address never aliases live MMIO,
 *  then restore the original command word.
 * ===========================================================================*/
#define PCI_CMD        0x04
#define PCI_CMD_IO     0x0001
#define PCI_CMD_MEM    0x0002
#define PCI_CMD_MASTER 0x0004
#define PCI_BAR0       0x10
#define PCI_INTLINE    0x3C

int pci_bar(const struct pci_dev *d, int idx, struct pci_bar *out) {
    if (idx < 0 || idx > 5) return -1;
    uint8_t off = (uint8_t)(PCI_BAR0 + idx * 4);
    uint32_t lo = pci_read32(d, off);
    if (lo == 0) return -1;

    uint16_t cmd = pci_read16(d, PCI_CMD);
    pci_cfg_write16(d->bus, d->slot, d->func, PCI_CMD,
                    (uint16_t)(cmd & ~(PCI_CMD_IO | PCI_CMD_MEM)));

    for (int i = 0; i < (int)sizeof(*out); i++) ((uint8_t *)out)[i] = 0;

    if (lo & 0x1) {
        /* I/O space BAR */
        out->is_mmio = 0;
        out->phys = lo & ~0x3u;
        out->port = (uint16_t)out->phys;
        pci_write32((struct pci_dev *)d, off, 0xFFFFFFFFu);
        uint32_t mask = pci_read32(d, off) & ~0x3u;
        out->size = mask ? ((uint64_t)(~mask) + 1) & 0xFFFF : 0;
        pci_write32((struct pci_dev *)d, off, lo);
    } else {
        /* memory space BAR */
        out->is_mmio  = 1;
        out->prefetch = (lo >> 3) & 1;
        int type = (lo >> 1) & 0x3;       /* 0=32-bit, 2=64-bit */
        uint32_t hi = 0;
        out->is_64bit = (type == 2);
        out->phys = lo & ~0xFu;
        if (out->is_64bit) {
            hi = pci_read32(d, (uint8_t)(off + 4));
            out->phys |= (uint64_t)hi << 32;
        }
        /* size probe */
        pci_write32((struct pci_dev *)d, off, 0xFFFFFFFFu);
        uint32_t szlo = pci_read32(d, off) & ~0xFu;
        uint64_t szmask = szlo;
        if (out->is_64bit) {
            pci_write32((struct pci_dev *)d, (uint8_t)(off + 4), 0xFFFFFFFFu);
            uint32_t szhi = pci_read32(d, (uint8_t)(off + 4));
            szmask |= (uint64_t)szhi << 32;
            pci_write32((struct pci_dev *)d, (uint8_t)(off + 4), hi);
        } else {
            szmask |= 0xFFFFFFFF00000000ull;   /* upper bits not writable on 32-bit BAR */
        }
        out->size = ~szmask + 1;
        pci_write32((struct pci_dev *)d, off, lo);
    }

    pci_cfg_write16(d->bus, d->slot, d->func, PCI_CMD, cmd);   /* restore decode */
    return 0;
}

volatile void *pci_map_bar(struct pci_bar *bar) {
    if (!bar->is_mmio) return 0;
    /* BoltOS direct-maps phys 0..4 GiB (shared PML4 slot -> valid under any CR3).
     * QEMU does not model caching, so the cached direct map is fine for MMIO.
     * TODO(real-HW/P5): remap these pages uncached (PCD) in a dedicated window. */
    if (bar->phys >= 0x100000000ull) return 0;   /* above the direct map */
    bar->virt = (volatile void *)P2V(bar->phys);
    return bar->virt;
}

void pci_enable_bus_master(const struct pci_dev *d) {
    uint16_t cmd = pci_read16(d, PCI_CMD);
    cmd |= PCI_CMD_IO | PCI_CMD_MEM | PCI_CMD_MASTER;
    pci_cfg_write16(d->bus, d->slot, d->func, PCI_CMD, cmd);
}

uint8_t pci_interrupt_line(const struct pci_dev *d) {
    return pci_cfg_read8(d->bus, d->slot, d->func, PCI_INTLINE);
}

/* ===========================================================================
 *  Message-Signalled Interrupts (MSI / MSI-X)
 *
 *  Lets a PCI device deliver an interrupt by *writing* a memory message to the
 *  local APIC, instead of asserting a shared INTx pin (which would need ACPI
 *  _PRT routing BoltOS doesn't parse). The composed (address,data) targets the
 *  boot CPU + the given IDT vector, edge/fixed (apic_msi_compose).
 *
 *  MSI-X is preferred (QEMU's nvme + qemu-xhci expose it): its vector table
 *  lives in an MMIO BAR; we program table entry 0 and unmask it. MSI is the
 *  fallback: the address/data go straight into config space. One vector only,
 *  which matches BoltOS's single-interrupter-per-controller use.
 *
 *  Returns 0 on success, -1 if the device has neither capability.
 * ===========================================================================*/
#define PCI_STATUS    0x06
#define PCI_CAP_PTR   0x34
#define CAP_ID_MSI    0x05
#define CAP_ID_MSIX   0x11

int pci_msi_enable(const struct pci_dev *d, uint8_t vector) {
    uint16_t status = pci_read16(d, PCI_STATUS);
    if (!(status & (1u << 4))) return -1;          /* no capability list */

    uint32_t addr = 0, data = 0;
    apic_msi_compose(vector, &addr, &data);

    /* walk the capability list, remembering MSI + MSI-X offsets */
    uint8_t cap = pci_cfg_read8(d->bus, d->slot, d->func, PCI_CAP_PTR) & 0xFC;
    uint8_t msi_cap = 0, msix_cap = 0;
    for (int guard = 0; cap && guard < 48; guard++) {
        uint8_t id = pci_cfg_read8(d->bus, d->slot, d->func, cap);
        if      (id == CAP_ID_MSIX) msix_cap = cap;
        else if (id == CAP_ID_MSI)  msi_cap  = cap;
        cap = pci_cfg_read8(d->bus, d->slot, d->func, (uint8_t)(cap + 1)) & 0xFC;
    }

    if (msix_cap) {
        uint32_t tbl = pci_read32(d, (uint8_t)(msix_cap + 4));   /* table offset|BIR */
        int      bir = (int)(tbl & 0x7);
        uint32_t toff = tbl & ~0x7u;
        struct pci_bar bar;
        if (pci_bar(d, bir, &bar) == 0 && bar.is_mmio && pci_map_bar(&bar)) {
            volatile uint32_t *e = (volatile uint32_t *)((volatile uint8_t *)bar.virt + toff);
            e[0] = addr;        /* message address low  */
            e[1] = 0;           /* message address high */
            e[2] = data;        /* message data         */
            e[3] = 0;           /* vector control: unmask (bit0 = 0) */
            __asm__ volatile("" ::: "memory");
            uint16_t mc = pci_read16(d, (uint8_t)(msix_cap + 2));
            mc |=  (1u << 15);  /* MSI-X Enable      */
            mc &= ~(1u << 14);  /* Function Mask off */
            pci_write16(d, (uint8_t)(msix_cap + 2), mc);
            return 0;
        }
        kprintf("[pci] MSI-X table BAR%d unmappable; trying MSI\n", bir);
    }

    if (msi_cap) {
        uint16_t mc = pci_read16(d, (uint8_t)(msi_cap + 2));
        int is64 = (mc >> 7) & 1;
        pci_write32(d, (uint8_t)(msi_cap + 4), addr);              /* msg addr lo */
        if (is64) {
            pci_write32(d, (uint8_t)(msi_cap + 8), 0);            /* msg addr hi */
            pci_write16(d, (uint8_t)(msi_cap + 0xC), (uint16_t)data);
        } else {
            pci_write16(d, (uint8_t)(msi_cap + 8), (uint16_t)data);
        }
        mc &= ~(0x7u << 4);   /* MME = 0 -> a single message  */
        mc |=  1u;            /* MSI Enable                   */
        pci_write16(d, (uint8_t)(msi_cap + 2), mc);
        return 0;
    }

    return -1;
}
