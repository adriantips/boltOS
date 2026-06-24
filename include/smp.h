#pragma once
#include <stdint.h>

/* ===========================================================================
 *  Symmetric multiprocessing: bring application processors online.
 *
 *  smp_init() copies the real-mode trampoline to low memory, then walks the
 *  ACPI CPU list and INIT-SIPI-SIPIs every non-boot core into long mode. Each
 *  AP loads the shared GDT/IDT, enables its local APIC, registers a per-CPU
 *  block reachable via the GS base, and parks in an interrupt-driven idle.
 * ===========================================================================*/

#define SMP_MAX_CPUS 64

struct percpu {
    uint64_t self;           /* GS base points here (must be first field) */
    uint32_t cpu_index;
    uint8_t  lapic_id;
    uint8_t  online;
    uint8_t  is_bsp;
    uint64_t idle_ticks;
    uint64_t kernel_stack;   /* top of this CPU's stack */
};

void  smp_init(void);
int   smp_cpu_count(void);          /* CPUs successfully brought online (incl. BSP) */
struct percpu *this_cpu(void);      /* per-CPU block of the calling core */
uint32_t smp_cpu_index(void);
