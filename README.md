# BoltOS

A 64-bit operating system written from scratch in C and x86-64 assembly — no
GRUB, no Multiboot, no external libraries. Custom MBR boot chain, own kernel.

> Status: **active development.** Custom boot chain (stage1 → stage2 → long mode) into a 64-bit C kernel. Features include a framebuffer console, physical memory management, RAM filesystem, hardware interrupts, keyboard and mouse drivers, an interactive shell, a graphical user interface (GUI), an IPv4/TCP/UDP network stack with an e1000 NIC driver, and windowed apps (terminal, browser, taskmgr, settings).

## Web browser

A small windowed **Browser** app renders basic HTML — headings, paragraphs,
lists, links, bold and preformatted text. It loads:

- **`http://` pages** over the kernel's own TCP/IP stack (DNS → TCP → HTTP/1.0).
  There is no TLS yet, so `https://` is unsupported.
- **local HTML files** from the ramfs (e.g. the bundled `/web/index.html`).

Click links to navigate, type a URL in the address bar, `<` goes back, and the
scrollbar / Space / `b` / `j` / `k` scroll. The data path rides the e1000 NIC
(the link QEMU/VirtualBox NAT exposes); Wi-Fi association is scaffolded in the
kernel but still needs a radio driver — see the `wifi` command. The shell also
has `browse URL` (render a page as text) and `download URL` (save to the ramfs).

## Boot flow (from scratch)

```
BIOS ── loads ──▶ stage1.asm (512 B MBR @ 0x7C00)
                      │ INT 13h LBA read
                      ▼
                  stage2.asm (@ 0x8000, real mode)
                      │  enable A20
                      │  BIOS E820 memory map
                      │  unreal mode → copy kernel to 1 MiB
                      │  build PML4/PDPT/PD (identity 0..4 GiB, 2 MiB pages)
                      │  enter long mode
                      ▼
                  kernel/boot.asm (_start, 64-bit @ 0x100000)
                      │  set stack, zero BSS
                      ▼
                  kmain(bootinfo)   ← kernel/main.c
```

The kernel is linked to a **flat binary** (`ld.lld --oformat binary`) loaded at
physical `0x100000`. `bootinfo` (framebuffer + E820 map) is handed to the kernel
in `RDI` and lives at physical `0x0500`.

## Toolchain (Windows / msys2)

- `nasm` — bootloader + kernel asm
- `clang --target=x86_64-elf` — freestanding kernel C (native cross-compiler)
- `ld.lld` — link kernel to flat binary
- `qemu-system-x86_64` — run / test

## Build & run

```bash
bash build.sh      # -> build/os.img
bash run.sh        # boot in QEMU, kernel output on serial (stdio)
```

## Layout

```
boot/           Custom MBR bootloader (stage1, stage2)
build/          Build outputs and OS image
drivers/        Hardware drivers (framebuffer, keyboard, mouse)
fs/             Filesystem implementations (RAMFS)
include/        Kernel and system headers
kernel/         Core kernel components (shell, GUI, interrupts, apps, etc.)
libc/           Freestanding C library subset
mm/             Memory management (PMM, heap)
linker.ld       Flat-binary kernel layout @ 0x100000
build.sh        Full build script → raw disk image
run.sh          QEMU launcher
```

## Project Stats

- **Total Lines of Code:** 4,703
  - **C:** 3,486 lines
  - **Assembly:** 509 lines
  - **Headers:** 354 lines
  - **Shell Scripts:** 332 lines
  - **Linker Script:** 22 lines

## Available Shell Commands

The interactive OS shell supports a wide range of commands:

- **File & Directory:** `ls`, `tree`, `cd`, `mkdir`, `rm`, `cp`, `mv`, `find`, `trash`, `recover`, `pwd`, `touch`, `write`
- **File Inspection:** `cat`, `head`, `tail`, `hex`, `meta`, `diff`, `grep`, `checksum`, `preview`, `count`
- **System Information:** `sysinfo`, `cpuinfo`, `meminfo`, `diskinfo`, `uptime`, `battery`, `sensors`, `devices`, `version`, `health`
- **Process Management:** `ps`, `kill`, `top`, `freeze`, `resume`, `services`, `service`, `jobs`, `priority`, `monitor`
- **Networking:** `netinfo`, `ping`, `trace`, `ports`, `download`, `browse`, `upload`, `wifi`, `firewall`, `share`, `scan`
- **Bonus / Unique:** `focus`, `snapshot`, `timeline`, `vault`, `doctor`, `assistant`, `sandbox`, `workspace`, `panic`, `story`
- **Core:** `help`, `echo`, `clear`, `mem`

## Full File Tree

```text
+-- .gitignore
+-- README.md
+-- boot
|   +-- stage1.asm
|   \-- stage2.asm
+-- build.sh
+-- drivers
|   +-- framebuffer.c
|   +-- keyboard.c
|   \-- mouse.c
+-- fs
|   \-- ramfs.c
+-- include
|   +-- boot.h
|   +-- commands.h
|   +-- console.h
|   +-- framebuffer.h
|   +-- fs.h
|   +-- gdt.h
|   +-- gui.h
|   +-- hw.h
|   +-- interrupts.h
|   +-- io.h
|   +-- keyboard.h
|   +-- kheap.h
|   +-- kprintf.h
|   +-- mouse.h
|   +-- pic.h
|   +-- pit.h
|   +-- pmm.h
|   +-- serial.h
|   +-- shell.h
|   +-- string.h
|   \-- sysreg.h
+-- kernel
|   +-- app_taskmgr.c
|   +-- app_terminal.c
|   +-- boot.asm
|   +-- cmd_extra.c
|   +-- cmd_fs.c
|   +-- cmd_net.c
|   +-- cmd_proc.c
|   +-- cmd_sys.c
|   +-- console.c
|   +-- font8x8.c
|   +-- gdt.c
|   +-- gui.c
|   +-- hw.c
|   +-- idt.c
|   +-- interrupts.c
|   +-- isr.asm
|   +-- kprintf.c
|   +-- main.c
|   +-- pic.c
|   +-- pit.c
|   +-- serial.c
|   +-- shell.c
|   \-- sysreg.c
+-- libc
|   \-- string.c
+-- linker.ld
+-- mm
|   +-- kheap.c
|   \-- pmm.c
\-- run.sh
```
