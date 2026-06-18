; ============================================================================
;  BoltOS  -  SYSCALL entry stub (IA32_LSTAR target).
;  On entry (from a ring-3 `syscall`): RCX = user RIP, R11 = user RFLAGS,
;  IF cleared by FMASK, and RSP is still the *user* stack. We switch to the
;  running process's kernel stack, marshal args to the SysV C ABI, dispatch,
;  then SYSRET back.
;
;  syscall ABI:  rax = number, rdi/rsi/rdx/r10/r8 = arg1..arg5, rax = return.
;  Per the syscall contract the kernel must preserve every register except
;  rax (return), rcx and r11 (clobbered by the SYSCALL/SYSRET hardware). The C
;  dispatcher is free to clobber the caller-saved registers, so we save the
;  user's rdi/rsi/rdx/r10/r8/r9 (callee-saved regs are preserved by C) and
;  restore them before returning to ring 3.
; ============================================================================
[BITS 64]

extern syscall_dispatch
extern syscall_kstack_top
extern syscall_user_rsp
global syscall_entry

section .text
syscall_entry:
    mov     [rel syscall_user_rsp], rsp     ; stash user RSP
    mov     rsp, [rel syscall_kstack_top]   ; per-process kernel stack

    push    rcx                             ; save user RIP
    push    r11                             ; save user RFLAGS
    push    rdi                             ; save user arg/temp registers that
    push    rsi                             ; the C dispatcher may clobber
    push    rdx
    push    r10
    push    r8
    push    r9

    ; (num=rax, a1=rdi, a2=rsi, a3=rdx, a4=r10, a5=r8)
    ;   -> C ABI dispatch(num, a1, a2, a3, a4, a5) in (rdi,rsi,rdx,rcx,r8,r9)
    mov     r9,  r8                         ; a5
    mov     r8,  r10                        ; a4
    mov     rcx, rdx                        ; a3
    mov     rdx, rsi                        ; a2
    mov     rsi, rdi                        ; a1
    mov     rdi, rax                        ; num
    call    syscall_dispatch                ; result in rax

    pop     r9                              ; restore user registers
    pop     r8
    pop     r10
    pop     rdx
    pop     rsi
    pop     rdi
    pop     r11                             ; restore user RFLAGS
    pop     rcx                             ; restore user RIP
    mov     rsp, [rel syscall_user_rsp]     ; restore user RSP
    o64 sysret                              ; 64-bit SYSRET -> ring 3
