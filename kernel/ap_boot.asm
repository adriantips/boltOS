; ===========================================================================
;  AP (application processor) trampoline.
;
;  Copied verbatim to physical 0x8000 by smp.c. A SIPI starts each AP in
;  16-bit real mode at CS=0x0800,IP=0 (== phys 0x8000). We climb real ->
;  protected -> long mode using a self-contained GDT, switch to the kernel's
;  page tables (CR3 passed in the parameter block), then jump to the 64-bit C
;  entry the BSP planted. Built with `nasm -f bin` and embedded as a blob.
;
;  Parameter block lives at phys 0x8F00 (filled by the BSP before each SIPI):
;     +0x00 cr3        (kernel PML4 physical)
;     +0x08 stack_top  (per-AP stack, virtual)
;     +0x10 entry      (ap_entry64, 64-bit virtual address)
;     +0x18 arg        (per-CPU index, passed in RDI)
; ===========================================================================
[BITS 16]
ORG 0x8000

%define PARAM 0x8F00

ap_start:
    cli
    cld
    xor ax, ax
    mov ds, ax              ; DS=0 so absolute [label] refs (ORG 0x8000) resolve
    mov es, ax
    mov ss, ax

    lgdt [gdt32_desc]

    mov eax, cr0
    or  eax, 1             ; CR0.PE
    mov cr0, eax
    jmp 0x08:pm32          ; far jump into 32-bit code segment

[BITS 32]
pm32:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    ; enable PAE
    mov eax, cr4
    or  eax, (1 << 5)
    mov cr4, eax

    ; load kernel PML4
    mov eax, [PARAM + 0x00]
    mov cr3, eax

    ; set EFER.LME (and NXE) via MSR 0xC0000080
    mov ecx, 0xC0000080
    rdmsr
    or  eax, (1 << 8) | (1 << 11)
    wrmsr

    ; enable paging -> activates long mode
    mov eax, cr0
    or  eax, (1 << 31) | (1 << 16)   ; PG | WP
    mov cr0, eax

    jmp 0x18:lm64          ; far jump into 64-bit code segment

[BITS 64]
lm64:
    mov ax, 0x20
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    mov rsp, [PARAM + 0x08]    ; per-AP stack
    mov rdi, [PARAM + 0x18]    ; arg -> first C argument
    mov rax, [PARAM + 0x10]    ; entry
    jmp rax                    ; into ap_entry64 (never returns)

.hang:
    hlt
    jmp .hang

; --- self-contained GDT: null, 32-bit code/data, 64-bit code/data ----------
align 16
gdt32:
    dq 0x0000000000000000      ; 0x00 null
    dq 0x00CF9A000000FFFF      ; 0x08 32-bit code
    dq 0x00CF92000000FFFF      ; 0x10 32-bit data
    dq 0x00AF9A000000FFFF      ; 0x18 64-bit code (L=1)
    dq 0x00AF92000000FFFF      ; 0x20 64-bit data
gdt32_end:

gdt32_desc:
    dw gdt32_end - gdt32 - 1
    dq gdt32

; pad up to (but not into) the parameter block at 0x8F00
times 0x0F00 - ($ - $$) db 0
