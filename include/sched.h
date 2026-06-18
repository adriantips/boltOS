#pragma once
#include <stdint.h>
#include "interrupts.h"

/* Minimal round-robin preemptive scheduler over kernel threads. The timer
 * IRQ0 calls schedule() (installed as the tick hook); it saves the outgoing
 * thread's register frame and returns the incoming thread's frame so the ISR
 * epilogue restores it. All threads run in ring 0 sharing the kernel CR3. */
struct proc;                                        /* defined in proc.h */

void sched_init(void);                              /* adopt current context as thread 0 */
int  sched_add(void (*entry)(void), const char *name);
/* Ring-3 thread: starts at user rip on user stack in address space cr3. The
 * proc owns the per-process kernel stack used for syscall/trap entry; the
 * scheduler loads it into tss.rsp0 + the SYSCALL stack on every switch in. */
int  sched_add_user(uint64_t rip, uint64_t ustack_top, uint64_t cr3,
                    struct proc *p, const char *name);
struct registers *schedule(struct registers *r);   /* tick hook */

/* Retire the running thread: free its slot so schedule() never resumes it.
 * Caller must not return to the abandoned context (spin in hlt). */
void         sched_exit_current(void);
struct proc *sched_current_proc(void);              /* proc of running thread, or 0 */

int          sched_count(void);                     /* live threads */
int          sched_current(void);
const char  *sched_name(int i);
uint64_t     sched_switches(void);
