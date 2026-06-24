/* ===========================================================================
 *  BoltOS  -  kernel/main.c   (kmain)
 *  Brings up the framebuffer console first so the whole boot log renders on
 *  screen, then the core kernel subsystems, then the idle loop.
 *  All output also mirrors to COM1 serial.
 * ===========================================================================*/
#include <stdint.h>
#include "boot.h"
#include "io.h"
#include "kprintf.h"
#include "serial.h"
#include "console.h"
#include "framebuffer.h"
#include "gpu.h"
#include "gdt.h"
#include "interrupts.h"
#include "pic.h"
#include "pit.h"
#include "acpi.h"
#include "apic.h"
#include "hpet.h"
#include "smp.h"
#include "pmm.h"
#include "vmm.h"
#include "mm.h"
#include "syscall.h"
#include "kheap.h"
#include "keyboard.h"
#include "mouse.h"
#include "fs.h"
#include "ata.h"
#include "ahci.h"
#include "blk.h"
#include "nvme.h"
#include "xhci.h"
#include "sysreg.h"
#include "sched.h"
#include "shell.h"
#include "gui.h"
#include "netif.h"
#include "vfs.h"
#include "proc.h"
#include "fat32.h"
#include "ext2.h"
#include "audio.h"
#include "dhcp.h"
#include "hw.h"
#include "users.h"
#include "ttf.h"
#include "string.h"

/* Turn on the CPU's supervisor-mode protections where the silicon offers them:
 *   SMEP - faults if ring 0 ever executes from a user page (blocks a whole
 *          class of privilege-escalation exploits).
 *   UMIP - makes SGDT/SIDT/SLDT/SMSW/STR fault in user mode, hiding kernel
 *          descriptor addresses from ring 3.
 * SMAP is deliberately left off: the syscall path reads user buffers directly
 * and would need STAC/CLAC bracketing first. CPUID leaf 7 reports support. */
static void cpu_harden(void) {
    uint32_t a, b, c, d;
    cpuidx(7, 0, &a, &b, &c, &d);
    uint64_t cr4; __asm__ volatile("mov %%cr4,%0" : "=r"(cr4));
    int smep = (b >> 7)  & 1;
    int umip = (c >> 2)  & 1;
    if (smep) cr4 |= (1ull << 20);
    if (umip) cr4 |= (1ull << 11);
    if (smep || umip) __asm__ volatile("mov %0,%%cr4" :: "r"(cr4) : "memory");
    kprintf("[ok] CPU hardening: SMEP=%s UMIP=%s\n", smep ? "on" : "n/a", umip ? "on" : "n/a");
}

/* Demo kernel threads: prove preemptive switching by heart-beating to serial
 * (the GUI owns the framebuffer console, so threads stay off it). hlt parks the
 * CPU until the next timer tick, which is also when the scheduler runs. */
static void demo_thread_a(void) {
    uint64_t last = 0;
    for (;;) {
        if (pit_ticks() - last >= 1000) { last = pit_ticks(); serial_putc('A'); }
        __asm__ volatile("hlt");
    }
}
static void demo_thread_b(void) {
    uint64_t last = 0;
    for (;;) {
        if (pit_ticks() - last >= 1000) { last = pit_ticks(); serial_putc('B'); }
        __asm__ volatile("hlt");
    }
}

/* Ring-3 user program. The build cross-compiles user/hello.c into a static
 * ET_EXEC ELF and embeds the raw bytes here. We seed it into the filesystem at
 * /bin/hello, then exec it through the ELF64 loader: a fresh address space is
 * built, the PT_LOAD segments are mapped, a SysV entry stack is laid out, and
 * the program is scheduled as a ring-3 thread with its own fd table + heap. */
extern const uint8_t _binary_hello_elf_start[];
extern const uint8_t _binary_hello_elf_end[];

static void launch_user_demo(void) {
    syscall_init();   kprintf("[ok] SYSCALL/SYSRET armed (LSTAR + EFER.SCE)\n");

    uint64_t len = (uint64_t)(_binary_hello_elf_end - _binary_hello_elf_start);
    fs_node *n = fs_lookup("/bin/hello");
    if (!n) { fs_create("/bin", 1); n = fs_create("/bin/hello", 0); }
    if (!n || fs_write(n, _binary_hello_elf_start, (uint32_t)len) != 0) {
        kprintf("[--] could not seed /bin/hello\n");
        return;
    }
    kprintf("[ok] seeded /bin/hello (%lu bytes)\n", len);

    if (proc_exec("/bin/hello") < 0)
        kprintf("[--] proc_exec(/bin/hello) failed\n");
}

void stackguard_seed(void);   /* kernel/stackguard.c: re-seed the stack canary */

void kmain(struct bootinfo *bi) {
    stackguard_seed();   /* before any other call, so all frames see one value */
    serial_init();
    fb_init(bi);
    console_init();          /* paints background + taskbar, starts text console */

    kprintf("==================================================\n");
    kprintf("   BoltOS kernel  -  64-bit long mode\n");
    kprintf("==================================================\n");
    if (fb_present())
        kprintf("[ok] framebuffer %ux%u @0x%lx (32bpp)\n",
                fb_width(), fb_height(), bi->fb_addr);
    else
        kprintf("[--] no framebuffer; serial only\n");

    gpu_init();   /* identify the display adapter + VRAM behind the framebuffer */

    gdt_init();   kprintf("[ok] GDT + TSS loaded\n");
    idt_init();   kprintf("[ok] IDT (256 vectors)\n");
    pic_init();   kprintf("[ok] PIC remapped to 0x20-0x2F\n");

    pmm_init(bi);
    vmm_init();   kprintf("[ok] VMM online (kernel PML4 @0x%lx)\n", vmm_kernel_pml4());
    kheap_init(); kprintf("[ok] kernel heap online (96 MiB @ 16 MiB)\n");
    cpu_harden(); /* SMEP/UMIP supervisor protections where supported */

    /* VMM self-test: map a fresh user page in a scratch address space and read
     * the translation back, then tear it down. */
    {
        uint64_t as = vmm_new_address_space();
        uint64_t pg = pmm_alloc_frame();
        vmm_map(as, 0x400000, pg, PTE_PRESENT | PTE_WRITE | PTE_USER);
        kprintf("[vmm] map 0x400000 -> 0x%lx, lookup -> 0x%lx %s\n",
                pg, vmm_get_phys(as, 0x400000),
                vmm_get_phys(as, 0x400000) == pg ? "OK" : "FAIL");
        vmm_unmap(as, 0x400000);
        pmm_free_frame(pg);
    }

    acpi_init();     /* parse ACPI tables (RSDP/XSDT/FADT/MADT/HPET) */

    pit_init(1000);  kprintf("[ok] PIT @ 1000 Hz\n");
    keyboard_init(); kprintf("[ok] PS/2 keyboard\n");
    if (fb_present()) { mouse_init((int)fb_width(), (int)fb_height()); kprintf("[ok] PS/2 mouse\n"); }

    /* Hand interrupt delivery to the APIC: masks the 8259s, routes PS/2 lines
     * through the IO-APIC, and runs the scheduler tick off the local-APIC timer.
     * Falls back to the PIC/PIT path if no ACPI/APIC is present. */
    apic_init();
    hpet_init();     /* high-resolution monotonic clock for fine delays */

    sti();
    kprintf("[ok] interrupts enabled\n");

    smp_init();      /* bring application processors online (INIT-SIPI-SIPI) */

    ata_init();      /* probe ATA disks (HDD/SSD) before the FS attaches */
    ahci_init();     /* probe AHCI/SATA controller; registers disks into block layer */
    nvme_init();     /* probe NVMe; registers its namespace into the block layer */
    xhci_init();     /* probe xHCI USB controller, enumerate attached devices */
    fs_init();       kprintf("[ok] ramfs mounted (/)\n");
    fs_persist_init();/* back the tree with a real disk + load saved image */
    vfs_init();      kprintf("[ok] VFS (ramfs + /dev + /proc)\n");

    /* FAT32: mount the SATA volume; format it on first boot. Kept separate from
     * the NVMe-backed BoltFS persistence so removable FAT media interoperates
     * with Windows/Linux. */
    for (int i = 0; i < blk_count(); i++) {
        blkdev_t *d = blk_get(i);
        if (d->is_boot || d->name[0] != 's' || d->name[1] != 'd') continue;
        if (fat32_mount(d) != 0) {
            if (fat32_format(d, "BOLTFAT") == 0) {
                const char *welcome =
                    "BoltOS FAT32 volume.\r\n"
                    "Read/written by the in-kernel FAT32 driver, and by any PC.\r\n";
                fat32_write("/README.TXT", welcome, (uint32_t)strlen(welcome));
                fat32_mkdir("/DOCS");
            }
        }
        break;
    }
    /* ext2: mount the first non-boot disk that carries an ext2 superblock, so a
     * Linux-formatted volume (e.g. a USB stick) can be read. Skipped for the
     * BoltFS/FAT disks above. */
    for (int i = 0; i < blk_count(); i++) {
        blkdev_t *d = blk_get(i);
        if (d->is_boot) continue;
        if (ext2_mount(d) == 0) break;
    }

    proc_init();
    users_init();    kprintf("[ok] user accounts (%d) loaded\n", users_count());
    ttf_init();      /* parse the embedded TrueType font for scalable text */
    sysreg_init();   kprintf("[ok] service + task registry\n");
    net_init();      /* netif core + NIC driver probe (e1000 under QEMU) */
    dhcp_configure(2000);  /* lease IP/mask/gw/DNS; falls back to static on timeout */
    audio_init();    /* AC97 / Intel HDA codec (PC speaker fallback) */

    sched_init();                         /* adopt current context as thread 0 */
    sched_add(demo_thread_a, "demoA");
    sched_add(demo_thread_b, "demoB");
    launch_user_demo();                   /* ring-3 user thread + syscalls */
    kprintf("[ok] preemptive scheduler (%d threads)\n", sched_count());

    evlog("framebuffer console came up");
    evlog("memory managers initialised");
    evlog("timer and keyboard online");
    evlog("ramfs mounted and shell started");

    if (fb_present()) {
        kprintf("\nBoltOS is up. Launching desktop.\n");
        gui_run();          /* enter the GUI desktop; never returns */
    }

    kprintf("\nBoltOS is up. Starting BoltShell.\n");
    shell_run();    /* serial-only fallback; never returns */
}
