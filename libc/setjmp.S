/* ===========================================================================
 *  libc/setjmp.S -- non-local jumps for x86-64 System V.
 *  jmp_buf layout (8 x uint64): rbx rbp r12 r13 r14 r15 rsp rip
 *  Used by image codecs (libjpeg/libpng-style error escapes).
 * ===========================================================================*/
    .intel_syntax noprefix
    .text
    .globl setjmp
    .globl _setjmp
    .globl longjmp
    .globl _longjmp

setjmp:
_setjmp:
    mov     [rdi + 0x00], rbx
    mov     [rdi + 0x08], rbp
    mov     [rdi + 0x10], r12
    mov     [rdi + 0x18], r13
    mov     [rdi + 0x20], r14
    mov     [rdi + 0x28], r15
    lea     rax, [rsp + 8]          /* caller's rsp (after the call returns) */
    mov     [rdi + 0x30], rax
    mov     rax, [rsp]              /* saved return address (rip)            */
    mov     [rdi + 0x38], rax
    xor     eax, eax
    ret

longjmp:
_longjmp:
    mov     rbx, [rdi + 0x00]
    mov     rbp, [rdi + 0x08]
    mov     r12, [rdi + 0x10]
    mov     r13, [rdi + 0x18]
    mov     r14, [rdi + 0x20]
    mov     r15, [rdi + 0x28]
    mov     rsp, [rdi + 0x30]
    mov     rax, rsi               /* return value                          */
    test    rax, rax
    jnz     1f
    inc     rax                    /* longjmp(env,0) must surface as 1       */
1:
    mov     rdx, [rdi + 0x38]
    jmp     rdx
