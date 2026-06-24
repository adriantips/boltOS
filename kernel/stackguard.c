/* ===========================================================================
 *  BoltOS  -  kernel/stackguard.c
 *  Stack-smashing protection for the kernel. The kernel is built with
 *  -fstack-protector-strong -mstack-protector-guard=global, so the compiler
 *  inserts a canary (read from the global __stack_chk_guard below) into the
 *  frame of any function with stack buffers and checks it on return. A mismatch
 *  means a buffer overran its bounds and clobbered the saved return address;
 *  __stack_chk_fail() is then called and we panic instead of returning into
 *  attacker-controlled memory.
 *
 *  The guard is a fixed non-zero value placed in .data so it is valid from the
 *  very first instruction (no init ordering hazard). It is re-seeded once early
 *  in boot from the timestamp counter to make the value unpredictable per boot.
 * ===========================================================================*/
#include <stdint.h>
#include "kprintf.h"

uintptr_t __stack_chk_guard = (uintptr_t)0x595e9fbd94fda766ull;

/* Mix in some boot-time entropy so the canary is not a build-time constant. */
void stackguard_seed(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t tsc = ((uint64_t)hi << 32) | lo;
    /* keep the low byte non-zero-terminated so string overruns still trip it */
    __stack_chk_guard ^= (tsc << 8) | 0x55;
}

__attribute__((noreturn))
void __stack_chk_fail(void) {
    kprintf("\n\n================ KERNEL PANIC ================\n");
    kprintf("  STACK SMASHING DETECTED\n");
    kprintf("  a buffer overflowed and corrupted a return frame\n");
    kprintf("  faulting return address: 0x%lx\n",
            (uint64_t)(uintptr_t)__builtin_return_address(0));
    kprintf("  System halted.\n");
    kprintf("=============================================\n");
    for (;;) __asm__ volatile("cli; hlt");
}
