; ============================================================================
;  BoltOS  -  ISR stubs for all 256 IDT vectors.
;  Each stub normalises the stack to [err_code][int_no] then funnels into
;  isr_common, which saves registers and calls the C dispatcher isr_handler.
; ============================================================================
[BITS 64]

extern isr_handler
extern fpu_save_current
extern fpu_restore_current

section .text

%macro ISR_NOERR 1
isr_stub_%1:
    push    qword 0          ; dummy error code
    push    qword %1         ; interrupt number
    jmp     isr_common
%endmacro

%macro ISR_ERR 1
isr_stub_%1:
    push    qword %1         ; CPU already pushed the real error code
    jmp     isr_common
%endmacro

%assign i 0
%rep 256
  %if i==8 || i==10 || i==11 || i==12 || i==13 || i==14 || i==17 || i==21 || i==29 || i==30
    ISR_ERR i
  %else
    ISR_NOERR i
  %endif
  %assign i i+1
%endrep

isr_common:
    push    rdi
    push    rsi
    push    rbp
    push    rdx
    push    rcx
    push    rbx
    push    rax
    push    r8
    push    r9
    push    r10
    push    r11
    push    r12
    push    r13
    push    r14
    push    r15

    cld
    ; Save the interrupted thread's FPU/SSE state into its own fxarea before the
    ; C handler (which may use XMM). rsp here is 16-aligned, so the call is
    ; ABI-correct. fpu_* are built -mno-sse and never touch XMM themselves.
    call    fpu_save_current
    mov     rdi, rsp         ; arg0 = pointer to struct registers
    call    isr_handler
    mov     rsp, rax         ; resume on the frame isr_handler returned
                             ; (the scheduler may have switched stacks)
    ; Restore the (possibly switched-to) thread's FPU/SSE state. rsp is now the
    ; resume frame (≡8 mod 16); harmless for a -mno-sse callee, and fxrstor
    ; takes an explicit 16-aligned operand independent of rsp.
    call    fpu_restore_current

    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     r11
    pop     r10
    pop     r9
    pop     r8
    pop     rax
    pop     rbx
    pop     rcx
    pop     rdx
    pop     rbp
    pop     rsi
    pop     rdi

    add     rsp, 16          ; discard int_no + err_code
    iretq

; ---- table of stub addresses for idt_init() ----
section .rodata
global isr_stub_table
isr_stub_table:
%assign i 0
%rep 256
    dq isr_stub_ %+ i
  %assign i i+1
%endrep
