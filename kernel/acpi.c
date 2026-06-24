#include <stdint.h>
#include "acpi.h"
#include "mm.h"
#include "io.h"
#include "kprintf.h"
#include "string.h"

/* ===========================================================================
 *  ACPI table discovery + power management.  All physical table addresses are
 *  reached through the direct map (P2V); ACPI tables live well under 4 GiB.
 * ===========================================================================*/

struct rsdp {
    char     signature[8];     /* "RSD PTR " */
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;
    uint32_t rsdt_address;
    /* v2+ */
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t  ext_checksum;
    uint8_t  reserved[3];
} __attribute__((packed));

struct fadt {
    struct acpi_sdt_header h;
    uint32_t firmware_ctrl;
    uint32_t dsdt;
    uint8_t  reserved;
    uint8_t  preferred_pm_profile;
    uint16_t sci_int;
    uint32_t smi_cmd;
    uint8_t  acpi_enable;
    uint8_t  acpi_disable;
    uint8_t  s4bios_req;
    uint8_t  pstate_ctrl;
    uint32_t pm1a_evt_blk;
    uint32_t pm1b_evt_blk;
    uint32_t pm1a_cnt_blk;
    uint32_t pm1b_cnt_blk;
    uint32_t pm2_cnt_blk;
    uint32_t pm_tmr_blk;
    uint32_t gpe0_blk;
    uint32_t gpe1_blk;
    uint8_t  pm1_evt_len;
    uint8_t  pm1_cnt_len;
    uint8_t  pm2_cnt_len;
    uint8_t  pm_tmr_len;
    uint8_t  gpe0_blk_len;
    uint8_t  gpe1_blk_len;
    uint8_t  gpe1_base;
    uint8_t  cstate_ctrl;
    uint16_t worst_c2_latency;
    uint16_t worst_c3_latency;
    uint16_t flush_size;
    uint16_t flush_stride;
    uint8_t  duty_offset;
    uint8_t  duty_width;
    uint8_t  day_alarm;
    uint8_t  mon_alarm;
    uint8_t  century;
    uint16_t iapc_boot_arch;
    uint8_t  reserved2;
    uint32_t flags;
    struct acpi_gas reset_reg;
    uint8_t  reset_value;
    uint8_t  reserved3[3];
    uint64_t x_firmware_ctrl;
    uint64_t x_dsdt;
    struct acpi_gas x_pm1a_evt_blk;
    struct acpi_gas x_pm1b_evt_blk;
    struct acpi_gas x_pm1a_cnt_blk;
    /* remaining fields unused */
} __attribute__((packed));

static struct acpi_info g_acpi;
static const struct fadt *g_fadt;

/* SLP_TYPa/b extracted from \_S5_ in the DSDT, plus the PM1 control ports. */
static int      s5_valid;
static uint16_t slp_typa, slp_typb;
static uint16_t pm1a_cnt_port, pm1b_cnt_port;

static int sdt_checksum_ok(const struct acpi_sdt_header *h) {
    uint8_t sum = 0;
    const uint8_t *p = (const uint8_t *)h;
    for (uint32_t i = 0; i < h->length; i++) sum = (uint8_t)(sum + p[i]);
    return sum == 0;
}

/* Scan a physical window for "RSD PTR " on a 16-byte boundary. */
static const struct rsdp *scan_rsdp(uint64_t phys_start, uint64_t phys_end) {
    for (uint64_t a = phys_start; a < phys_end; a += 16) {
        const struct rsdp *r = (const struct rsdp *)P2V(a);
        if (memcmp(r->signature, "RSD PTR ", 8) == 0) {
            uint8_t sum = 0;
            const uint8_t *p = (const uint8_t *)r;
            for (int i = 0; i < 20; i++) sum = (uint8_t)(sum + p[i]);
            if (sum == 0) return r;
        }
    }
    return 0;
}

static const struct rsdp *find_rsdp(void) {
    /* EBDA segment pointer lives at physical 0x40E (word, in paragraphs). */
    uint16_t ebda_seg = *(const uint16_t *)P2V(0x40E);
    uint64_t ebda = (uint64_t)ebda_seg << 4;
    const struct rsdp *r;
    if (ebda && (r = scan_rsdp(ebda, ebda + 1024))) return r;
    return scan_rsdp(0xE0000, 0x100000);
}

/* Cache pointers to every SDT referenced by the RSDT/XSDT. */
#define MAX_TABLES 32
static const struct acpi_sdt_header *g_tables[MAX_TABLES];
static int g_table_count;

const struct acpi_sdt_header *acpi_find(const char *sig) {
    for (int i = 0; i < g_table_count; i++)
        if (memcmp(g_tables[i]->signature, sig, 4) == 0) return g_tables[i];
    return 0;
}

static void cache_table(uint64_t phys) {
    if (g_table_count >= MAX_TABLES || !phys) return;
    const struct acpi_sdt_header *h = (const struct acpi_sdt_header *)P2V(phys);
    if (!sdt_checksum_ok(h)) return;
    g_tables[g_table_count++] = h;
}

/* Targeted DSDT scan for \_S5_ -> SLP_TYPa/b. Pattern after the "_S5_" name is
 * a small PackageOp: 12 <pkglen> 04 <a> <b> ... where <a>/<b> are either a
 * BytePrefix (0x0A v) or a constant op (Zero/One/0x00..). */
static void parse_s5(const struct fadt *f) {
    /* The 64-bit X_DSDT field only exists in the extended (ACPI 2.0+) FADT.
     * On a 1.0 FADT (length ~116) reading it walks off the end of the table, so
     * fall back to the 32-bit DSDT pointer unless the table is long enough. */
    uint64_t dsdt_phys = (f->h.length > 140 && f->x_dsdt) ? f->x_dsdt : (uint64_t)f->dsdt;
    if (!dsdt_phys) return;
    const struct acpi_sdt_header *d = (const struct acpi_sdt_header *)P2V(dsdt_phys);
    if (memcmp(d->signature, "DSDT", 4) != 0) return;
    const uint8_t *aml = (const uint8_t *)d + sizeof(*d);
    uint32_t len = d->length > sizeof(*d) ? d->length - sizeof(*d) : 0;

    for (uint32_t i = 0; i + 6 < len; i++) {
        if (aml[i] == '_' && aml[i+1] == 'S' && aml[i+2] == '5' && aml[i+3] == '_') {
            const uint8_t *p = aml + i + 4;
            if (*p != 0x12) continue;          /* PackageOp */
            p++;
            /* pkglength: top 2 bits of first byte give #following length bytes */
            uint8_t lead = *p;
            p += 1 + (lead >> 6);
            p++;                                /* numelements */
            int a = -1, b = -1;
            if (*p == 0x0A) { a = p[1]; p += 2; }   /* BytePrefix */
            else if (*p <= 0x09) { a = *p; p += 1; } /* small const */
            if (*p == 0x0A) { b = p[1]; }
            else if (*p <= 0x09) { b = *p; }
            if (a >= 0) {
                slp_typa = (uint16_t)(a << 10);
                slp_typb = (uint16_t)((b < 0 ? 0 : b) << 10);
                s5_valid = 1;
            }
            return;
        }
    }
}

void acpi_init(void) {
    memset(&g_acpi, 0, sizeof(g_acpi));

    const struct rsdp *r = find_rsdp();
    if (!r) { kprintf("[acpi] no RSDP found\n"); return; }
    g_acpi.present  = 1;
    g_acpi.revision = r->revision;

    /* Prefer the XSDT (64-bit entries) on ACPI 2.0+. */
    if (r->revision >= 2 && r->xsdt_address) {
        const struct acpi_sdt_header *xsdt =
            (const struct acpi_sdt_header *)P2V(r->xsdt_address);
        int n = (xsdt->length - sizeof(*xsdt)) / 8;
        const uint64_t *ent = (const uint64_t *)((const uint8_t *)xsdt + sizeof(*xsdt));
        for (int i = 0; i < n; i++) cache_table(ent[i]);
    } else {
        const struct acpi_sdt_header *rsdt =
            (const struct acpi_sdt_header *)P2V(r->rsdt_address);
        int n = (rsdt->length - sizeof(*rsdt)) / 4;
        const uint32_t *ent = (const uint32_t *)((const uint8_t *)rsdt + sizeof(*rsdt));
        for (int i = 0; i < n; i++) cache_table(ent[i]);
    }

    /* FADT: power management + reset register. */
    g_fadt = (const struct fadt *)acpi_find("FACP");
    if (g_fadt) {
        pm1a_cnt_port = (uint16_t)g_fadt->pm1a_cnt_blk;
        pm1b_cnt_port = (uint16_t)g_fadt->pm1b_cnt_blk;
        parse_s5(g_fadt);
    }

    /* MADT: APIC topology. */
    const struct acpi_sdt_header *madt = acpi_find("APIC");
    if (madt) {
        const uint8_t *p = (const uint8_t *)madt + sizeof(*madt);
        g_acpi.lapic_addr = *(const uint32_t *)p;       /* local APIC addr */
        p += 8;                                          /* skip addr+flags */
        const uint8_t *end = (const uint8_t *)madt + madt->length;
        while (p < end) {
            uint8_t type = p[0], reclen = p[1];
            if (reclen < 2) break;
            switch (type) {
            case 0: /* processor local APIC */
                if ((p[4] & 1) && g_acpi.cpu_count < ACPI_MAX_CPUS)
                    g_acpi.cpu_lapic_id[g_acpi.cpu_count++] = p[3];
                break;
            case 1: /* IO-APIC */
                if (g_acpi.ioapic_count < ACPI_MAX_IOAPIC) {
                    struct acpi_ioapic *io = &g_acpi.ioapic[g_acpi.ioapic_count++];
                    io->id       = p[2];
                    io->address  = *(const uint32_t *)(p + 4);
                    io->gsi_base = *(const uint32_t *)(p + 8);
                }
                break;
            case 2: /* interrupt source override */
                if (g_acpi.iso_count < ACPI_MAX_ISO) {
                    struct acpi_iso *iso = &g_acpi.iso[g_acpi.iso_count++];
                    iso->bus    = p[2];
                    iso->source = p[3];
                    iso->gsi    = *(const uint32_t *)(p + 4);
                    iso->flags  = *(const uint16_t *)(p + 8);
                }
                break;
            case 5: /* local APIC address override (64-bit) */
                g_acpi.lapic_addr = *(const uint64_t *)(p + 4);
                break;
            }
            p += reclen;
        }
    }

    /* HPET base. */
    const struct acpi_sdt_header *hpet = acpi_find("HPET");
    if (hpet) {
        const struct acpi_gas *gas = (const struct acpi_gas *)((const uint8_t *)hpet + 44);
        g_acpi.hpet_addr = gas->address;
        g_acpi.hpet_seq  = *((const uint8_t *)hpet + 52);
    }

    kprintf("[ok] ACPI rev %d: %d CPU(s), %d IOAPIC, lapic@0x%lx%s\n",
            g_acpi.revision, g_acpi.cpu_count, g_acpi.ioapic_count,
            g_acpi.lapic_addr, g_acpi.hpet_addr ? ", HPET" : "");
}

const struct acpi_info *acpi_get(void) { return &g_acpi; }

uint32_t acpi_irq_to_gsi(uint8_t irq, uint16_t *flags_out) {
    for (int i = 0; i < g_acpi.iso_count; i++)
        if (g_acpi.iso[i].source == irq) {
            if (flags_out) *flags_out = g_acpi.iso[i].flags;
            return g_acpi.iso[i].gsi;
        }
    if (flags_out) *flags_out = 0;
    return irq;
}

/* ---- power -------------------------------------------------------------- */

/* QEMU/Bochs ACPI poweroff also responds to a simple word write to PM1a; the
 * 0x604 and 0xB004 ports are QEMU's well-known shutdown channels. */
static void legacy_poweroff(void) {
    outw(0x604,  0x2000);   /* QEMU (newer) */
    outw(0xB004, 0x2000);   /* QEMU (older) / Bochs */
    outw(0x4004, 0x3400);   /* VirtualBox */
}

void acpi_poweroff(void) {
    cli();
    if (s5_valid && pm1a_cnt_port) {
        outw(pm1a_cnt_port, (uint16_t)(slp_typa | (1 << 13)));   /* SLP_EN */
        if (pm1b_cnt_port)
            outw(pm1b_cnt_port, (uint16_t)(slp_typb | (1 << 13)));
    }
    legacy_poweroff();
    /* If still alive, park. */
    for (;;) __asm__ volatile("cli; hlt");
}

void acpi_reboot(void) {
    cli();
    /* 1) ACPI reset register, if the FADT advertises one. */
    if (g_fadt && (g_fadt->flags & (1u << 10)) && g_fadt->reset_reg.address) {
        const struct acpi_gas *rr = &g_fadt->reset_reg;
        if (rr->address_space == 1)               /* system I/O */
            outb((uint16_t)rr->address, g_fadt->reset_value);
        else if (rr->address_space == 0)          /* system memory */
            *(volatile uint8_t *)P2V(rr->address) = g_fadt->reset_value;
    }
    /* 2) 0xCF9 reset control. */
    outb(0xCF9, 0x02);
    outb(0xCF9, 0x06);
    /* 3) 8042 keyboard-controller pulse of the CPU reset line. */
    uint8_t good = 0x02;
    while (good & 0x02) good = inb(0x64);
    outb(0x64, 0xFE);
    /* 4) triple fault: load a zero-limit IDT and raise an interrupt. */
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) nidt = {0, 0};
    __asm__ volatile("lidt %0; int3" :: "m"(nidt));
    for (;;) __asm__ volatile("cli; hlt");
}
