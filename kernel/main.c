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
#include "gdt.h"
#include "interrupts.h"
#include "pic.h"
#include "pit.h"
#include "pmm.h"
#include "vmm.h"
#include "kheap.h"
#include "keyboard.h"
#include "mouse.h"
#include "fs.h"
#include "sysreg.h"
#include "sched.h"
#include "shell.h"
#include "gui.h"
#include "netif.h"

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

void kmain(struct bootinfo *bi) {
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

    gdt_init();   kprintf("[ok] GDT + TSS loaded\n");
    idt_init();   kprintf("[ok] IDT (256 vectors)\n");
    pic_init();   kprintf("[ok] PIC remapped to 0x20-0x2F\n");

    pmm_init(bi);
    vmm_init();   kprintf("[ok] VMM online (kernel PML4 @0x%lx)\n", vmm_kernel_pml4());
    kheap_init(); kprintf("[ok] kernel heap online (16 MiB @ 16 MiB)\n");

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

    pit_init(1000);  kprintf("[ok] PIT @ 1000 Hz\n");
    keyboard_init(); kprintf("[ok] PS/2 keyboard\n");
    if (fb_present()) { mouse_init((int)fb_width(), (int)fb_height()); kprintf("[ok] PS/2 mouse\n"); }

    sti();
    kprintf("[ok] interrupts enabled\n");

    fs_init();       kprintf("[ok] ramfs mounted (/)\n");
    sysreg_init();   kprintf("[ok] service + task registry\n");
    net_init();      /* netif core + NIC driver probe (e1000 under QEMU) */

    sched_init();                         /* adopt current context as thread 0 */
    sched_add(demo_thread_a, "demoA");
    sched_add(demo_thread_b, "demoB");
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
