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

/* ---- BoltOS device syscalls (private; let a ring-3 program own the screen,
 *      read the keyboard, and fetch over the network -- the substrate the
 *      ring-3 web browser /bin/browser runs on). The drivers stay in ring 0;
 *      ring 3 reaches them only through these calls. -------------------------*/
#define SYS_FBINFO  100 /* (struct user_fbinfo*)     -> 0/-1; claim panel+dims */
#define SYS_GETKEY  101 /* ()                        -> key, or -1 if none    */
#define SYS_HTTPGET 102 /* (url, buf, cap, status*)  -> body len / -1         */
#define SYS_FBEND   103 /* ()                        -> 0; release the screen  */
#define SYS_FBPRESENT 104 /* (xrgb w*h buffer)       -> 0/-1; blit to panel    */

/* Filled by SYS_FBINFO: the panel geometry. The renderer draws a packed w*h
 * xRGB image into its own buffer and hands it to SYS_FBPRESENT each frame; the
 * compositor is paused until SYS_FBEND / process exit. `ptr` is reserved (0).
 * Layout shared verbatim with user/ulibc.h. */
struct user_fbinfo { uint64_t ptr; uint32_t w, h, pitch_px; };

/* mmap prot bits */
#define PROT_READ   1
#define PROT_WRITE  2
#define PROT_EXEC   4

void     syscall_init(void);                 /* program STAR/LSTAR/FMASK/EFER.SCE */
void     syscall_entry(void);                /* asm prologue (kernel/syscall.asm) */
uint64_t syscall_dispatch(uint64_t num, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5);
