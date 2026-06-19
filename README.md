# BoltOS

A 64-bit operating system written from scratch in C and x86-64 assembly — no
GRUB, no Multiboot, no external libraries. Custom MBR boot chain, own kernel.

> Status: **active development.** Custom boot chain (stage1 → stage2 → long mode)
> into a 64-bit C kernel. Features include a framebuffer compositor and windowed
> **GUI desktop with 20+ apps**, a preemptive **round-robin scheduler** with
> processes and **ring-3 userland** (ELF64 loader + syscalls), physical and
> virtual memory management, a **persistent filesystem on real ATA disks
> (HDD/SSD)** behind a VFS, hardware interrupts, keyboard and mouse drivers, an
> interactive shell, a full **IPv4/TCP/UDP/TLS network stack** on an e1000 NIC, a
> **multi-language IDE** with from-scratch **C / C++ / C# compilers** and a
> **Python interpreter**, a **GPU / display-adapter driver**, support for running
> real **Windows PE32+ executables**, **PC-speaker audio**, and an image decoder.

## Desktop & apps

The GUI (`kernel/gui.c`) is a software compositor: every window draws into a
backbuffer through vector helpers (`g_fill`, `g_round`, `g_text`, `gui_icon`, …),
which is flipped to the framebuffer. Windows support move/resize, focus/z-order,
minimize/maximize, a taskbar with pinning, right-click context menus, and a
desktop of double-clickable launcher icons. Each app is a `kernel/app_*.c` file
exposing `draw/key/click/drag/tick` callbacks.

Bundled apps:

- **Terminal** — the full interactive shell in a window
- **File Explorer** — browse/open the persistent filesystem
- **Browser** — HTML renderer over the kernel's own HTTP/HTTPS stack
- **Code** — a multi-language IDE (C / C++ / C# / Python) with syntax
  highlighting, line numbers, a Run button and an output console
- **Task Manager** — live process / CPU / memory view
- **Settings** — themes and system options (persisted)
- **Calculator**, **Clock**, **Stopwatch**, **Calendar**, **Notes** (saved to FS)
- **Paint**, **Color Picker**, **Piano** (PC-speaker, zoomable multi-octave)
- **System Info**, **Matrix** rain
- Games: **Minesweeper**, **Snake**, **2048**, **Tic-Tac-Toe**, **Memory match**,
  **Conway's Game of Life**

> No hardware floating point in the kernel — everything is fixed-point / integer
> math (FPU/SSE state isn't saved across the scheduler).

## Userland (ring 3, ELF64)

BoltOS runs real user programs in **ring 3**. `user/` is a tiny freestanding C
program (`crt0.asm` + `ulibc.c`) linked to a **static ELF64** (`user/user.ld`)
and embedded into the kernel image at build time. The kernel's ELF loader
(`kernel/elf.c`) maps the segments into a fresh address space; the program
traps into the kernel via `syscall` (`kernel/syscall.asm` + `syscall.c`) for I/O.
A preemptive **round-robin scheduler** (`kernel/sched.c`) with per-process
state (`kernel/proc.c`) time-slices processes off the PIT.

## Storage (HDD / SSD) + VFS

An **ATA/IDE PIO block driver** (`drivers/ata.c`) probes the legacy ATA register
file on both channels (ports `0x1F0/0x3F6` and `0x170/0x376`), runs `IDENTIFY` on
each slot, and supports LBA28/LBA48 sector read/write. The same command set drives
both spinning **HDDs** and **SSDs**; the two are told apart from IDENTIFY word 217
(nominal media rotation rate: `1` = non-rotating = SSD). ATAPI/CD-ROM slots are
detected and skipped.

The filesystem (**BoltFS**, `fs/ramfs.c`) is reached through a **VFS layer**
(`kernel/vfs.c`). It is no longer RAM-only: `fs_persist_init()` attaches the first
non-boot ATA disk, **loads a saved image on boot** (or formats the seed tree if
none), and autosaves the whole tree on every mutation, so files survive reboots.
The on-disk format is a superblock plus a serialised pre-order node list.
`diskinfo` lists detected disks + media type; `sync` forces a flush. QEMU exposes
two data disks (`run.sh`): one as an HDD (`rotation_rate=7200`), one as an SSD
(`rotation_rate=1`).

## Web browser & network stack

A windowed **Browser** renders basic HTML — headings, paragraphs, lists, links,
bold, preformatted text, and **inline images**. It loads:

- **`http://` and `https://` pages** over the kernel's own stack
  (DNS → TCP → TLS → HTTP/1.0). HTTPS uses an in-kernel TLS 1.2 client
  (ECDHE-X25519 + AES-128-GCM + SHA-256, `net/tls.c` + `net/crypto.c`). The
  server certificate is **not** verified — it resists passive eavesdropping,
  not an active MITM — and TLS 1.3-only / gzipped / chunked sites won't render.
- **local HTML files** from the filesystem (e.g. the bundled `/web/index.html`).

Click links to navigate, type a URL in the address bar, `<` goes back, and the
scrollbar / Space / `b` / `j` / `k` scroll. The data path rides the **e1000 NIC**
driver (`drivers/e1000.c`) over QEMU/VirtualBox NAT, with a from-scratch stack:
ARP, IPv4, ICMP, UDP, TCP, DNS, HTTP (`net/*.c`). Wi-Fi association is scaffolded
in the kernel but still needs a radio driver — see the `wifi` command. The shell
also has `browse URL` (render a page as text) and `download URL` (save to the FS).

## IDE & compilers (BoltCC + BoltPy)

The **Code** app (`kernel/app_ide.c`) is a real in-kernel IDE: a text editor with
a movable caret, line numbers, mouse click-to-place, scrolling, and live syntax
highlighting, plus a language switcher and a **Run** button that compiles and
executes the buffer, streaming the program's output into a console pane below.

- **BoltCC** (`kernel/boltcc.c`) is a from-scratch **C / C++ / C# compiler**: one
  pipeline — lexer → recursive-descent parser → AST → **stack bytecode** → a
  bytecode **VM**. It genuinely compiles the source and runs the emitted
  bytecode (it is not a text interpreter). Shared across the three dialects:
  functions with recursion and forward references, `int`/`char`/`long`/`bool`/
  `string`/`var` declarations, the full arithmetic / bitwise / comparison
  operator set with correct precedence, `&&`/`||`/`!` short-circuiting, `?:`,
  `++`/`--`, compound assignment, `if`/`else`/`while`/`for`/`return`/`break`/
  `continue`, string concatenation and indexing, and builtins (`len`, `str`,
  `int`, `abs`, `min`, `max`, `chr`, `ord`). Per dialect:
  - **C** — `printf` (real `%d/%i/%u/%x/%c/%s/%%` formatting), `puts`, `putchar`
  - **C++** — `std::cout << … << std::endl` chains, `using namespace`, classes skipped
  - **C#** — `Console.WriteLine`/`Write` (incl. `{0} {1}` formatting), `using` /
    `namespace` / `class` unwrapped, entry point `Main()`
- **BoltPy** (`kernel/boltpy.c`) is a small Python-3 subset interpreter — the
  IDE's Python tab and the `python` shell command run on it. Integer arithmetic,
  variables, strings/lists, `print`, conditionals, `while`/`for`, `def`/recursion.

> The kernel has no FPU (SSE/x87 disabled), so every language here is **integer +
> string** only — there is no floating point.

## GPU / display driver

`drivers/gpu.c` probes the PCI bus for a display-class (`0x03`) controller,
identifies the adapter from its `vendor:device` id (QEMU std-VGA, VirtualBox,
VMware SVGA II, VirtIO-GPU, Cirrus, Intel, NVIDIA, AMD), decodes BAR0 to size the
VRAM aperture, and detects the **Bochs DISPI (VBE)** interface for runtime mode
setting. It backs the **Graphics** line in System Info and exposes a mode list +
`gpu_set_mode()` (the linear framebuffer itself lives in `drivers/framebuffer.c`).

## Windows executables (PE32+)

BoltOS can load and run real **Windows x86-64 console `.exe`** files. The loader
(`kernel/pe.c`) parses the PE32+ headers, copies sections by RVA into a heap
image, binds the import table against an in-kernel **kernel32 shim**
(`GetStdHandle`, `WriteConsoleA`, `WriteFile`, `ExitProcess`, `Heap*`, … exposed
with the Microsoft x64 ABI), and calls the entry point. The build compiles
`user/winhello.c` into a **genuine PE32+** with the mingw toolchain and embeds it;
run it from the shell with `winrun` (no args = the embedded demo, or
`winrun /path/to/app.exe`). The image is position-independent (RIP-relative), so
no base relocations are needed once imports are bound.

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

- `nasm` — bootloader, kernel asm, and the userland `crt0`
- `clang --target=x86_64-elf` — freestanding kernel + userland C (native cross-compiler)
- `clang --target=x86_64-w64-mingw32` — builds `user/winhello.c` into a real PE32+ `.exe`
- `ld.lld` — link kernel to a flat binary and the user program to a static ELF64
- `qemu-system-x86_64` — run / test

## Build & run

```bash
bash build.sh      # -> iso/os.img (+ bootable iso/boltos.iso)
bash run.sh        # boot in QEMU, kernel output on serial (stdio)
```

`build.sh` also assembles + links `user/hello.c` into a static ELF64 and compiles
`user/winhello.c` into a real PE32+ `.exe`, embedding both in the kernel.
PowerShell helpers (`rebuild-all.ps1`, `rebuild-vdi.ps1`, `build-and-run-vbox.bat`)
build VirtualBox-friendly images.

## Layout

```
boot/           Custom MBR bootloader (stage1, stage2)
drivers/        Hardware drivers (framebuffer, GPU/display, keyboard, mouse, ATA HDD/SSD, e1000, PC speaker)
fs/             Filesystem (BoltFS: in-RAM tree, persisted to an ATA disk)
include/        Kernel and system headers
kernel/         Core kernel: scheduler, processes, syscalls, ELF + PE loaders, GUI + apps, shell, BoltCC compilers, Python
libc/           Freestanding C library subset
mm/             Memory management (PMM, heap, virtual memory, DMA)
net/            Network stack (ARP, IP, ICMP, UDP, TCP, DNS, HTTP, TLS, crypto, e1000 glue, Wi-Fi)
user/           Ring-3 userland program (crt0, ulibc) -> static ELF64
linker.ld       Flat-binary kernel layout @ 0x100000
build.sh        Full build script → raw disk image + bootable ISO
run.sh          QEMU launcher
```

## Project Stats

- **Total Lines of Code:** ~20,870
  - **C:** ~18,250 lines
  - **Assembly:** 708 lines
  - **Headers:** ~1,655 lines
  - **Shell Scripts:** 160 lines
  - **Linker Scripts:** 62 lines
  - **Python (build tools):** 46 lines

## Available Shell Commands

The interactive OS shell supports a wide range of commands:

- **File & Directory:** `ls`, `tree`, `cd`, `mkdir`, `rm`, `cp`, `mv`, `find`, `trash`, `recover`, `pwd`, `touch`, `write`
- **File Inspection:** `cat`, `head`, `tail`, `hex`, `meta`, `diff`, `grep`, `checksum`, `preview`, `count`
- **System Information:** `sysinfo`, `cpuinfo`, `meminfo`, `diskinfo`, `sync`, `uptime`, `battery`, `sensors`, `devices`, `version`, `health`
- **Process Management:** `ps`, `kill`, `top`, `freeze`, `resume`, `services`, `service`, `jobs`, `priority`, `monitor`
- **System (cont.):** `winrun` (run a Windows PE32+ `.exe` via the in-kernel loader)
- **Networking:** `netinfo`, `ping`, `trace`, `ports`, `download`, `browse`, `upload`, `wifi`, `firewall`, `share`, `scan`
- **Language:** `python` (BoltPy interpreter / REPL); the **Code** app compiles C / C++ / C#
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
+-- run.sh
+-- build-and-run-vbox.bat
+-- rebuild-all.ps1
+-- rebuild-vdi.ps1
+-- mkhpfs.py
+-- tree.py
+-- linker.ld
+-- drivers
|   +-- ata.c
|   +-- e1000.c
|   +-- framebuffer.c
|   +-- gpu.c
|   +-- keyboard.c
|   +-- mouse.c
|   \-- pcspk.c
+-- fs
|   \-- ramfs.c
+-- include
|   +-- ata.h            +-- gdt.h            +-- net.h
|   +-- boltcc.h         +-- gpu.h            +-- pe.h
|   +-- boltpy.h         +-- gui.h            +-- netif.h
|   +-- boot.h           +-- html.h           +-- pic.h
|   +-- commands.h       +-- http.h           +-- pit.h
|   +-- console.h        +-- hw.h             +-- pmm.h
|   +-- crypto.h         +-- image.h          +-- proc.h
|   +-- dma.h            +-- interrupts.h     +-- sched.h
|   +-- driver.h         +-- io.h             +-- serial.h
|   +-- elf.h            +-- keyboard.h       +-- settings.h
|   +-- firmware.h       +-- kheap.h          +-- shell.h
|   +-- framebuffer.h    +-- kprintf.h        +-- string.h
|   +-- fs.h             +-- mm.h             +-- syscall.h
|   +-- ...              +-- mmio.h           +-- sysreg.h
|   +-- mouse.h          +-- tls.h            +-- vfs.h
|   +-- vmm.h            \-- wifi.h
+-- kernel
|   +-- app_2048.c       +-- app_terminal.c   +-- gui.c
|   +-- app_browser.c    +-- app_ttt.c        +-- hw.c
|   +-- app_calc.c       +-- boltpy.c         +-- idt.c
|   +-- app_calendar.c   +-- boot.asm         +-- image.c
|   +-- app_clock.c      +-- cmd_extra.c      +-- interrupts.c
|   +-- app_colorpick.c  +-- cmd_fs.c         +-- isr.asm
|   +-- app_files.c      +-- cmd_net.c        +-- kprintf.c
|   +-- app_life.c       +-- cmd_proc.c       +-- main.c
|   +-- app_matrix.c     +-- cmd_python.c     +-- pci.c
|   +-- app_memory.c     +-- cmd_sys.c        +-- pic.c
|   +-- app_mines.c      +-- console.c        +-- pit.c
|   +-- app_notes.c      +-- elf.c            +-- proc.c
|   +-- app_paint.c      +-- font8x8.c        +-- sched.c
|   +-- app_piano.c      +-- gdt.c            +-- serial.c
|   +-- app_ide.c        +-- settings.c       +-- shell.c
|   +-- app_settings.c   +-- syscall.asm      +-- sysreg.c
|   +-- app_snake.c      +-- syscall.c        +-- user.asm
|   +-- app_stopwatch.c  +-- app_sysinfo.c    +-- boltcc.c
|   \-- pe.c
+-- libc
|   \-- string.c
+-- mm
|   +-- dma.c
|   +-- kheap.c
|   +-- pmm.c
|   \-- vmm.c
+-- net
|   +-- arp.c    +-- driver.c   +-- firmware.c  +-- icmp.c   +-- netif.c  +-- tls.c
|   +-- crypto.c +-- dns.c      +-- eth.c       +-- ip.c     +-- tcp.c    +-- udp.c
|   \-- http.c   \-- wifi.c
\-- user
    +-- crt0.asm
    +-- hello.c
    +-- winhello.c
    +-- ulibc.c
    +-- ulibc.h
    \-- user.ld
```
