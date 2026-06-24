#include <stdint.h>
#include "commands.h"
#include "hw.h"
#include "fs.h"
#include "blk.h"
#include "pmm.h"
#include "kheap.h"
#include "pit.h"
#include "kprintf.h"
#include "string.h"
#include "framebuffer.h"
#include "ata.h"

#define BOLT_VERSION "BoltOS 0.2"

static void p2(uint8_t v) { kprintf("%s%u", v < 10 ? "0" : "", (unsigned)v); }

static void print_datetime(void) {
    struct rtc_time t; rtc_now(&t);
    kprintf("%u-", (unsigned)t.year); p2(t.mon); kprintf("-"); p2(t.day);
    kprintf(" "); p2(t.hour); kprintf(":"); p2(t.min); kprintf(":"); p2(t.sec);
}

static void print_uptime(void) {
    uint64_t ms = sh_uptime_ms();
    uint64_t s = ms / 1000;
    uint64_t d = s / 86400, h = (s / 3600) % 24, m = (s / 60) % 60, ss = s % 60;
    if (d) kprintf("%lud ", (unsigned long)d);
    kprintf("%luh %lum %lus", (unsigned long)h, (unsigned long)m, (unsigned long)ss);
}

/* ------------------------------ cpuinfo ---------------------------------- */
int cmd_cpuinfo(int argc, char **argv) {
    (void)argc; (void)argv;
    char vendor[13], brand[49];
    hw_cpu_vendor(vendor);
    hw_cpu_brand(brand);
    uint32_t a, b, c, d;
    cpuidx(1, 0, &a, &b, &c, &d);

    uint32_t stepping = a & 0xF;
    uint32_t model    = (a >> 4) & 0xF;
    uint32_t family   = (a >> 8) & 0xF;
    if (family == 0xF) family += (a >> 20) & 0xFF;
    if (family == 0x6 || family == 0xF) model += ((a >> 16) & 0xF) << 4;

    kprintf("vendor   : %s\n", vendor);
    kprintf("brand    : %s\n", brand[0] ? brand : "(unknown)");
    kprintf("family   : %u  model %u  stepping %u\n",
            (unsigned)family, (unsigned)model, (unsigned)stepping);
    kprintf("max leaf : 0x%x\n", hw_cpu_max_leaf());
    kprintf("features :");
    if (d & (1u << 0))  kprintf(" fpu");
    if (d & (1u << 5))  kprintf(" msr");
    if (d & (1u << 6))  kprintf(" pae");
    if (d & (1u << 9))  kprintf(" apic");
    if (d & (1u << 23)) kprintf(" mmx");
    if (d & (1u << 25)) kprintf(" sse");
    if (d & (1u << 26)) kprintf(" sse2");
    if (c & (1u << 0))  kprintf(" sse3");
    if (c & (1u << 9))  kprintf(" ssse3");
    if (c & (1u << 19)) kprintf(" sse4.1");
    if (c & (1u << 20)) kprintf(" sse4.2");
    if (c & (1u << 28)) kprintf(" avx");
    if (c & (1u << 31)) kprintf(" hypervisor");
    kprintf("\n");
    return 0;
}

/* ------------------------------ meminfo ---------------------------------- */
int cmd_meminfo(int argc, char **argv) {
    (void)argc; (void)argv;
    uint64_t freep = pmm_free_count(), totp = pmm_total_count();
    uint64_t usedp = totp - freep;
    kprintf("physical : total %lu MiB  used %lu MiB  free %lu MiB\n",
            (unsigned long)((totp * 4096ull) >> 20),
            (unsigned long)((usedp * 4096ull) >> 20),
            (unsigned long)((freep * 4096ull) >> 20));
    kprintf("frames   : %lu / %lu free  (4 KiB each)\n",
            (unsigned long)freep, (unsigned long)totp);
    kprintf("kheap    : kmalloc/kfree region allocator\n");
    char sz[12]; sh_human(fs_total_bytes(), sz);
    kprintf("ramfs    : %d nodes, %s in files\n", fs_count_nodes(), sz);
    return 0;
}
int cmd_mem(int argc, char **argv) { return cmd_meminfo(argc, argv); }

/* ------------------------------ uptime ----------------------------------- */
int cmd_uptime(int argc, char **argv) {
    (void)argc; (void)argv;
    kprintf("up "); print_uptime();
    kprintf("   (%lu ticks @ %u Hz)\n",
            (unsigned long)pit_ticks(), (unsigned)pit_hz());
    return 0;
}

/* ------------------------------ version ---------------------------------- */
int cmd_version(int argc, char **argv) {
    (void)argc; (void)argv;
    char brand[49]; hw_cpu_brand(brand);
    kprintf("%s  (64-bit long mode)\n", BOLT_VERSION);
    kprintf("arch     : x86-64\n");
    kprintf("cpu      : %s\n", brand[0] ? brand : "x86-64");
    kprintf("clock    : "); print_datetime(); kprintf("\n");
    return 0;
}

/* ------------------------------ devices ---------------------------------- */
int cmd_devices(int argc, char **argv) {
    (void)argc; (void)argv;
    kprintf("platform devices:\n");
    kprintf("  i8253  PIT        timer       @0x40\n");
    kprintf("  i8259  PIC        irq ctrl    @0x20/0xA0\n");
    kprintf("  i8042  PS/2 kbd   input       @0x60\n");
    kprintf("  16550  COM1       serial      @0x3F8\n");
    kprintf("  CMOS   RTC        clock       @0x70\n");
    if (fb_present())
        kprintf("  VBE    framebuffer %ux%u 32bpp\n", fb_width(), fb_height());

    struct pci_dev pds[32];
    int n = pci_scan(pds, 32);
    kprintf("PCI devices: %d\n", n);
    for (int i = 0; i < n; i++) {
        struct pci_dev *p = &pds[i];
        kprintf("  %x:%x.%x  %x:%x  ",
                p->bus, p->slot, p->func, p->vendor, p->device);
        sh_pad(pci_class_name(p->class), 14);
        kprintf("\n");
    }
    return 0;
}

/* ------------------------------ diskinfo --------------------------------- */
int cmd_diskinfo(int argc, char **argv) {
    (void)argc; (void)argv;

    struct pci_dev pds[32];
    int n = pci_scan(pds, 32), found = 0;
    for (int i = 0; i < n; i++) {
        if (pds[i].class == 0x01) {
            kprintf("ctrl   : PCI %x:%x.%x  storage  %x:%x\n",
                    pds[i].bus, pds[i].slot, pds[i].func, pds[i].vendor, pds[i].device);
            found = 1;
        }
    }
    if (!found) kprintf("ctrl   : (no PCI storage controller reported)\n");

    int dn = blk_count();
    kprintf("block devices: %d\n", dn);
    for (int i = 0; i < dn; i++) {
        blkdev_t *d = blk_get(i);
        char sz[12]; sh_human(d->sectors * d->sector_size, sz);
        kprintf("  [%d] %-8s %s  %s  %s%s\n", i, d->name, blk_media(d), sz,
                d->is_boot ? "boot " : "",
                d->model[0] ? d->model : "(disk)");
    }

    char sz[12]; sh_human(fs_total_bytes(), sz);
    if (fs_persist_active())
        kprintf("mounted: BoltFS on %s '%s'  %d nodes  %s (persistent)\n",
                fs_persist_media(), fs_persist_model(), fs_count_nodes(), sz);
    else
        kprintf("mounted: ramfs (in-RAM, volatile)  %d nodes  %s\n",
                fs_count_nodes(), sz);
    return 0;
}

/* ------------------------------ sync ------------------------------------- */
int cmd_sync(int argc, char **argv) {
    (void)argc; (void)argv;
    if (!fs_persist_active()) { kprintf("sync: no persistent disk; FS is RAM-only\n"); return 1; }
    int rc = fs_sync();
    kprintf(rc == 0 ? "sync: flushed FS to %s '%s'\n" : "sync: FAILED\n",
            fs_persist_media(), fs_persist_model());
    return rc == 0 ? 0 : 1;
}

/* ------------------------------ battery ---------------------------------- */
int cmd_battery(int argc, char **argv) {
    (void)argc; (void)argv;
    kprintf("no battery detected.\n");
    kprintf("ACPI power management is not implemented; on a desktop/VM this is\n");
    kprintf("expected (AC power, no smart battery).\n");
    return 0;
}

/* ------------------------------ sensors ---------------------------------- */
int cmd_sensors(int argc, char **argv) {
    (void)argc; (void)argv;
    kprintf("no hardware sensors available.\n");
    kprintf("thermal/voltage/fan readouts need ACPI or MSR/SMBus drivers,\n");
    kprintf("which are not implemented.\n");
    kprintf("live counters:\n");
    kprintf("  pit ticks : %lu @ %u Hz\n", (unsigned long)pit_ticks(), (unsigned)pit_hz());
    kprintf("  rtc clock : "); print_datetime(); kprintf("\n");
    return 0;
}

/* ------------------------------ sysinfo ---------------------------------- */
int cmd_sysinfo(int argc, char **argv) {
    (void)argc; (void)argv;
    char brand[49]; hw_cpu_brand(brand);
    uint64_t freep = pmm_free_count(), totp = pmm_total_count();
    struct pci_dev pds[32];
    int npci = pci_scan(pds, 32);

    kprintf("=============== BoltOS system summary ===============\n");
    kprintf(" os      : %s (x86-64 long mode)\n", BOLT_VERSION);
    kprintf(" cpu     : %s\n", brand[0] ? brand : "x86-64");
    kprintf(" memory  : %lu MiB total, %lu MiB free\n",
            (unsigned long)((totp * 4096ull) >> 20),
            (unsigned long)((freep * 4096ull) >> 20));
    if (fb_present()) kprintf(" display : %ux%u 32bpp framebuffer\n", fb_width(), fb_height());
    else              kprintf(" display : serial only\n");
    kprintf(" pci     : %d devices\n", npci);
    kprintf(" ramfs   : %d nodes\n", fs_count_nodes());
    kprintf(" uptime  : "); print_uptime(); kprintf("\n");
    kprintf(" clock   : "); print_datetime(); kprintf("\n");
    kprintf("=====================================================\n");
    return 0;
}

/* ------------------------ health / doctor -------------------------------- */
/* Runs real self-checks against live subsystems. Returns problem count. */
static int run_diagnostics(int suggest) {
    int problems = 0;

    /* heap round-trip */
    void *a = kmalloc(256), *b = kmalloc(4096);
    if (a && b) { kfree(a); kfree(b); kprintf("[ok]   kheap   alloc/free round-trip\n"); }
    else        { kprintf("[FAIL] kheap   allocation returned NULL\n"); problems++;
                  if (suggest) kprintf("       -> heap exhausted; free objects or grow kheap\n"); }

    /* physical memory */
    uint64_t freep = pmm_free_count(), totp = pmm_total_count();
    if (totp && freep) kprintf("[ok]   pmm     %lu/%lu frames free\n",
                               (unsigned long)freep, (unsigned long)totp);
    else { kprintf("[WARN] pmm     no free frames\n"); problems++; }

    /* timer advancing (IRQ0 must be firing) */
    uint64_t t0 = pit_ticks();
    for (volatile uint64_t i = 0; i < 60000000ull && pit_ticks() == t0; i++) { }
    if (pit_ticks() != t0) kprintf("[ok]   pit     timer interrupts firing\n");
    else { kprintf("[FAIL] pit     no timer ticks (IRQ0 stalled?)\n"); problems++;
           if (suggest) kprintf("       -> check PIC mask / sti / IDT vector 0x20\n"); }

    /* filesystem writable */
    fs_node *t = fs_create("/.health", 0);
    if (t && fs_write(t, "ok", 2) == 0) { fs_remove_node(t); kprintf("[ok]   ramfs   create/write/remove\n"); }
    else { kprintf("[FAIL] ramfs   cannot create temp file\n"); problems++; }

    return problems;
}

int cmd_health(int argc, char **argv) {
    (void)argc; (void)argv;
    int p = run_diagnostics(0);
    kprintf(p ? "health: %d problem(s) found\n" : "health: all systems nominal\n", p);
    return p ? 1 : 0;
}

int cmd_doctor(int argc, char **argv) {
    (void)argc; (void)argv;
    kprintf("BoltDoctor: running diagnostics...\n");
    int p = run_diagnostics(1);
    if (!p) kprintf("diagnosis: healthy. No action needed.\n");
    else    kprintf("diagnosis: %d issue(s). See [FAIL]/[WARN] hints above.\n", p);
    return 0;
}
