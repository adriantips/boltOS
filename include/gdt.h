#pragma once
#include <stdint.h>
void gdt_init(void);

/* Update the ring0 stack the CPU loads on a ring3->ring0 trap (interrupts,
 * exceptions). The scheduler calls this when switching to a user thread so
 * each process traps onto its own kernel stack. */
void tss_set_rsp0(uint64_t top);

/* Load the shared GDT + reload segments on an application processor. */
void gdt_load_ap(void);
