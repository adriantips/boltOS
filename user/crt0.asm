; ============================================================================
;  BoltOS  -  user crt0. ELF entry point. The kernel lays out a SysV-style
;  entry stack: [rsp]=argc, [rsp+8]=argv[0], ... so we hand argc/argv to main,
;  then turn main's return value into exit(code).
; ============================================================================
[BITS 64]
global _start
extern main
extern exit

section .text
_start:
    xor     rbp, rbp
    mov     rdi, [rsp]          ; argc
    lea     rsi, [rsp + 8]      ; argv
    and     rsp, -16            ; 16-byte align before the call
    call    main
    mov     edi, eax            ; exit status
    call    exit
.hang:
    jmp     .hang               ; exit never returns
