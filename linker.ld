/* BoltOS kernel link script. Higher-half kernel: linked at virtual
 * 0xFFFFFFFF80100000, loaded (LMA) at physical 1 MiB. ld --oformat binary
 * lays the image out by LMA, so the flat binary still starts at phys 0x100000.
 * .text.boot (the _start stub) must come first so its LMA == load address. */
ENTRY(_start)

KERNEL_VBASE = 0xFFFFFFFF80000000;

SECTIONS
{
    . = 0xFFFFFFFF80100000;

    .text : AT(ADDR(.text) - KERNEL_VBASE) {
        *(.text.boot)
        *(.text .text.*)
    }

    .rodata : AT(ADDR(.rodata) - KERNEL_VBASE) { *(.rodata .rodata.*) }

    /* Link-time driver table (P4): DRIVER_REGISTER drops a driver* here; the
     * bounds let driver_probe_all() walk it. KEEP so GC never drops entries. */
    . = ALIGN(8);
    .drivers : AT(ADDR(.drivers) - KERNEL_VBASE) {
        __drivers_start = .;
        KEEP(*(.drivers))
        __drivers_end = .;
    }

    .data   : AT(ADDR(.data)   - KERNEL_VBASE) { *(.data .data.*) }

    . = ALIGN(16);
    __bss_start = .;
    .bss (NOLOAD) : AT(ADDR(.bss) - KERNEL_VBASE) {
        *(COMMON)
        *(.bss .bss.*)
    }
    __bss_end = .;

    __kernel_end = .;

    /DISCARD/ : { *(.comment) *(.note .note.*) *(.eh_frame) *(.eh_frame_hdr) }
}
