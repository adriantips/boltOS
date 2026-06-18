/* BoltOS user program: static ET_EXEC, loaded at 0x400000. Sections are
   page-aligned so the ELF loader maps code (RX) and data (RW) on distinct
   pages. */
ENTRY(_start)

SECTIONS {
    . = 0x400000;

    .text : { *(.text*) }

    . = ALIGN(0x1000);
    .rodata : { *(.rodata*) }

    . = ALIGN(0x1000);
    .data : { *(.data*) }

    .bss : { *(.bss*) *(COMMON) }

    /DISCARD/ : { *(.note*) *(.comment) *(.eh_frame*) }
}
