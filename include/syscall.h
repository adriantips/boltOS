#pragma once
#include <stdint.h>

/* ring-3 selectors (see kernel/gdt.c ordering) */
#define USER_CS 0x23
#define USER_SS 0x1B

/* ---------------------------------------------------------------------------
 *  syscall numbers (rax). Values mirror Linux x86-64 where convenient; the
 *  libc is hand-rolled (user/ulibc.c) so the mapping is private to BoltOS.
 *
 *  ABI: rax = number; args in rdi, rsi, rdx, r10, r8 (up to 5); result in rax.
 *  (r10 not rcx for the 4th arg, since `syscall` clobbers rcx.)
 * ------------------------------------------------------------------------- */
#define SYS_READ    0   /* (fd, buf, len)            -> bytes / -1            */
#define SYS_WRITE   1   /* (fd, buf, len)            -> bytes / -1            */
#define SYS_OPEN    2   /* (path, flags, mode)       -> fd / -1              */
#define SYS_CLOSE   3   /* (fd)                      -> 0 / -1               */
#define SYS_FSTAT   5   /* (fd, vstat*)              -> 0 / -1               */
#define SYS_LSEEK   8   /* (fd, off, whence)         -> new off / -1         */
#define SYS_MMAP    9   /* (addr, len, prot, flags, fd) -> addr / -1         */
#define SYS_BRK     12  /* (addr)                    -> new break            */
#define SYS_YIELD   24  /* ()                        -> 0                    */
#define SYS_GETPID  39  /* ()                        -> pid                  */
#define SYS_EXIT    60  /* (code)                    -> (no return)          */

/* mmap prot bits */
#define PROT_READ   1
#define PROT_WRITE  2
#define PROT_EXEC   4

void     syscall_init(void);                 /* program STAR/LSTAR/FMASK/EFER.SCE */
void     syscall_entry(void);                /* asm prologue (kernel/syscall.asm) */
uint64_t syscall_dispatch(uint64_t num, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5);
