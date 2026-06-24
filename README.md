# BoltOS

A 64-bit operating system written from scratch in C and x86-64 assembly — no
GRUB, no Multiboot, no external libraries. Custom MBR boot chain, own kernel.

> Status: **active development.** Custom boot chain (stage1 → stage2 → long mode)
> into a 64-bit C kernel. Features include a framebuffer compositor and windowed
> **GUI desktop with 20+ apps** (including a playable port of **DOOM**), a
> preemptive **round-robin scheduler** with processes and **ring-3 userland**
> (ELF64 loader + syscalls), physical and virtual memory management, a
> **persistent filesystem on real ATA / NVMe disks (HDD/SSD)** behind a VFS and a
> generic block layer, hardware interrupts, keyboard and mouse drivers, an
> **xHCI USB** host-controller driver, an interactive shell, a full
> **IPv4/TCP/UDP/TLS 1.2+1.3 network stack** (with X.509 certificate verification)
> on an e1000 NIC, a **standards-ish web browser** with a real DOM tree, CSS
> cascade, box/flex/grid layout engine and a from-scratch **JavaScript
> interpreter**, a **multi-language IDE** with from-scratch **C / C++ / C#
> compilers** and a **Python interpreter**, a **GPU / display-adapter driver**,
> support for running real **Windows PE32+ executables**, **PC-speaker audio**,
> and an image decoder.

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
- **Browser** — a layout-engine web browser (DOM + CSS cascade + box/flex/grid +
  JavaScript) over the kernel's own HTTP/HTTPS stack
- **OldBrowser** — a **NetSurf** port: the NetSurf core architecture (nsurl,
  content cache, handler pipeline, `browser_window` + back/forward history) with
  its classic framebuffer-frontend toolbar (Back / Forward / Reload / Stop /
  Home, address bar, throbber, bookmarks), rendering through the BoltOS layout
  engine over the kernel TCP/TLS stack
- **Code** — a multi-language IDE (C / C++ / C# / Python) with syntax
  highlighting, line numbers, a Run button and an output console
- **Task Manager** — live process / CPU / memory view
- **Settings** — themes and system options (persisted)
- **Calculator**, **Clock**, **Stopwatch**, **Calendar**, **Notes** (saved to FS)
- **Paint**, **Color Picker**, **Piano** (PC-speaker, zoomable multi-octave)
- **System Info**, **Matrix** rain
- Games: **DOOM** (real doomgeneric port, playable E1M1), **Minesweeper**,
  **Snake**, **2048**, **Tic-Tac-Toe**, **Memory match**, **Conway's Game of Life**

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

## Storage (HDD / SSD / NVMe) + VFS

Storage sits behind a **generic block layer** (`kernel/blk.c` + `include/blk.h`):
a small registry of `blkdev_t` transports plus shared MBR-partition helpers, so
the filesystem doesn't care whether a disk is ATA or NVMe.

An **ATA/IDE PIO block driver** (`drivers/ata.c`) probes the legacy ATA register
file on both channels (ports `0x1F0/0x3F6` and `0x170/0x376`), runs `IDENTIFY` on
each slot, and supports LBA28/LBA48 sector read/write. The same command set drives
both spinning **HDDs** and **SSDs**; the two are told apart from IDENTIFY word 217
(nominal media rotation rate: `1` = non-rotating = SSD). ATAPI/CD-ROM slots are
detected and skipped.

An **NVMe driver** (`drivers/nvme.c`) brings up a controller over PCI/MMIO: it
sets up the admin queue pair, runs `IDENTIFY`, creates one I/O queue pair, and
registers namespace 1 with the block layer. It is polled — completions are read
off the CQ phase bit — which fits BoltOS's synchronous, one-command-at-a-time I/O.

The filesystem (**BoltFS**, `fs/ramfs.c`) is reached through a **VFS layer**
(`kernel/vfs.c`). It is no longer RAM-only: `fs_persist_init()` attaches the first
non-boot disk (preferring NVMe, then SSD, then HDD), **loads a saved image on
boot** (or formats the seed tree if none), and autosaves the whole tree on every
mutation, so files survive reboots. The on-disk format is a superblock plus a
serialised pre-order node list. `diskinfo` lists detected disks + media type;
`sync` forces a flush. QEMU exposes data disks (`run.sh`) across both transports.

## Web browser & network stack

The windowed **Browser** is no longer an HTML flattener — it is a small but real
rendering engine. A page is parsed into a **DOM tree** (`kernel/dom.c`): a proper
tokenizer + tree builder with implicit-close rules, void/self-close/raw-text
handling, and a **selector engine** (type/`.class`/`#id`/universal, compound
selectors, descendant and child combinators, selector lists) backing
`querySelector` / `querySelectorAll` plus DOM mutations.

On top of the tree sits a **CSS cascade + layout engine** (`kernel/layout.c`):

- **Cascade** — `#hex` / `rgb()` / `rgba()` / `hsl()` / `hsla()` / named /
  `transparent` colors, selector **specificity** (a,b,c) + source order,
  property **inheritance**, intrinsic tag defaults, and inline `style=""`.
- **Layout** — the box model (margin / padding / border / width → border-box),
  **block** + **inline** flow with text word-wrap on the bitmap-font grid,
  **flexbox** (row/column, `flex-grow`, `gap`, `justify-content`, `align-items`)
  and **CSS grid** (px + `fr` tracks, `repeat()`, gaps, row wrapping).

This box tree is what the Browser actually paints — backgrounds, borders, wrapped
text, images, inputs and hit-tested links — relaid out on width change
(responsive). A flat run-list path is kept as a fallback for plain text and a few
scrape targets.

A from-scratch **JavaScript interpreter** (`kernel/js.c`, "BoltJS") runs page
scripts: lexer → recursive-descent (precedence-climbing) parser → AST walker.
Numbers are int64 (no FPU). It is wired to the **real DOM**:
`document.querySelector`, `createElement`, `appendChild`, `setAttribute` /
`getAttribute`, element `id` / `className` / `href` / `value`, `document.cookie`,
and `localStorage` / `sessionStorage`; mutations trigger reflow. A **persistent VM**
lives for the page's lifetime, so it also has an **event loop**:
`addEventListener` with click dispatch, `setTimeout` / `setInterval` /
`requestAnimationFrame` (driven off the PIT tick), and **`fetch()` returning real
Promises** (`.then` / `.catch` / `.finally`, `Promise.resolve` / `reject` / `all`,
`new Promise`) drained through a microtask queue, plus `JSON.parse` / `stringify`.
The `js` shell command runs code inline (`js -c "…"`) or a script from the
filesystem (`js FILE.js`).

Pages load over the kernel's own stack (DNS → TCP → TLS → HTTP/1.1):

- **`http://` and `https://`** with cookies, redirects, keep-alive, and **gzip /
  deflate** content-encoding (`net/inflate.c`). HTTPS uses an in-kernel
  **TLS 1.2 *and* 1.3 client** (`net/tls.c` + `net/crypto.c`): ECDHE over
  **X25519 / secp256r1 (P-256)**, **AES-128-GCM-SHA256** and
  **AES-256-GCM-SHA384**. The server **certificate chain is now verified**
  (`net/x509.c` + `net/rsa.c` + `net/p256.c` / `net/p384.c`): RSA PKCS#1 v1.5 /
  PSS and ECDSA P-256 signatures, hostname and validity dates, against a small
  runtime trust-anchor store — so it resists an active MITM, not just passive
  eavesdropping.
- **local HTML files** from the filesystem (e.g. the bundled `/web/index.html`).

Click links to navigate, type a URL in the address bar, `<` goes back, and the
scrollbar / Space / `b` / `j` / `k` scroll. The data path rides the **e1000 NIC**
driver (`drivers/e1000.c`) over QEMU/VirtualBox NAT, with a from-scratch stack:
ARP, IPv4, ICMP, UDP, TCP, DNS, HTTP, a **firewall** (`net/firewall.c`), and TLS
(`net/*.c`). Wi-Fi association is scaffolded in the kernel but still needs a radio
driver — see the `wifi` command. The shell also has `browse URL` (render a page as
text) and `download URL` (save to the FS).

## OldBrowser — a NetSurf port

**OldBrowser** is a port of **NetSurf** — the small, fast, portable C web browser
— onto BoltOS. NetSurf is built around a clean split between a portable *core*
(content cache + handler pipeline + `browser_window` state machine) and
per-platform *frontends*; OldBrowser reproduces that architecture faithfully.
The port lives under `oldbrowser/`, with each file mapping to the NetSurf
subsystem named in its banner:

| OldBrowser file        | NetSurf module                                   |
| ---------------------- | ------------------------------------------------ |
| `ob_nsurl.c`           | `utils/nsurl.c` + `utils/url.c` — URL object, RFC 3986 `nsurl_join` |
| `ob_llcache.c`         | `content/llcache.c` — fetch + redirect following, `about:` pages |
| `ob_content.c`         | `content/content.c` + handlers — `text/html`, `text/plain`, `image/*` |
| `ob_window.c`          | `desktop/browser_window.c` + `browser_history.c` — navigation, back/forward |
| `ob_hotlist.c`         | `desktop/hotlist.c` — bookmarks, persisted to the filesystem |
| `ob_fbtk.c`            | `frontends/framebuffer/fbtk` — the widget toolkit |
| `ob_gui.c`             | `frontends/framebuffer/gui.c` — toolbar, throbber, viewport, status bar |

Where NetSurf leans on **libdom** and **libcss**, this port drives BoltOS's own
DOM tree (`kernel/dom.c`) and CSS-cascade + box/flex/grid layout engine
(`kernel/layout.c`); where it uses **libnsgif / libnspng**, it calls the BoltOS
image decoder (`kernel/image.c`); fetches ride the kernel **HTTP/HTTPS** stack
(`net/http.c`). The result is the recognisable NetSurf chrome — a toolbar of
**Back / Forward / Reload / Stop / Home**, an address bar, an animated
**throbber** and a bookmark toggle — over a scrolling content viewport. Typed
addresses that don't look like URLs become a web search; the `about:welcome`
home page and `about:credits` render offline. The BoltOS window glue is
`kernel/app_oldbrowser.c`.

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

## DOOM

BoltOS runs **real DOOM**. The **doomgeneric** port lives under `doom/`, built
against a tiny in-house freestanding libc shim (`doom/dg_libc.c`) and a BoltOS
platform layer (`doom/doomgeneric_boltos.c`); the shareware WAD is embedded in the
kernel image as a blob. The **DOOM** desktop app (`kernel/app_doom.c`) pumps the
engine, routes keyboard input, and blits the engine's 640×400 framebuffer into its
window — E1M1 is playable. As everywhere else there is no FPU, so the port is
driven with integer math only.

## USB (xHCI)

An **xHCI USB host-controller driver** (`drivers/xhci.c`) brings up the controller
(DCBAA, command ring, single-segment event ring, scratchpad buffers) and, for each
connected root-hub port, resets it, issues **Enable Slot** / **Address Device**,
and walks EP0 control transfers to read the device and configuration descriptors.
Enumerated devices are reported at boot. The driver is polled, matching the rest of
the kernel's synchronous I/O model.

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
doom/           Vendored doomgeneric port + in-house libc shim + BoltOS platform layer
oldbrowser/     NetSurf port (OldBrowser): nsurl, content cache + handlers, browser_window, fbtk frontend
drivers/        Hardware drivers (framebuffer, GPU/display, keyboard, mouse, ATA HDD/SSD, NVMe, xHCI USB, e1000, PC speaker)
fs/             Filesystem (BoltFS: in-RAM tree, persisted via the block layer)
include/        Kernel and system headers
kernel/         Core kernel: scheduler, processes, syscalls, ELF + PE loaders, block layer, GUI + apps, shell, browser engine (DOM/layout), BoltCC compilers, Python + JS
libc/           Freestanding C library subset
mm/             Memory management (PMM, heap, virtual memory, DMA)
net/            Network stack (ARP, IP, ICMP, UDP, TCP, DNS, HTTP, TLS 1.2/1.3, crypto, RSA, X.509, inflate, firewall, e1000 glue, Wi-Fi)
user/           Ring-3 userland program (crt0, ulibc) -> static ELF64
linker.ld       Flat-binary kernel layout @ 0x100000
build.sh        Full build script → raw disk image + bootable ISO
run.sh          QEMU launcher
```

## Project Stats

First-party code (excludes the vendored `doom/` doomgeneric port):

- **Total Lines of Code:** ~30,000
  - **C:** ~26,770 lines
  - **Headers:** ~2,495 lines
  - **Assembly:** 729 lines
  - **Shell Scripts:** ~160 lines
  - **Linker Scripts:** 62 lines
  - **Python (build tools):** ~46 lines

## Available Shell Commands

The interactive OS shell supports a wide range of commands:

- **File & Directory:** `ls`, `tree`, `cd`, `mkdir`, `rm`, `cp`, `mv`, `find`, `trash`, `recover`, `pwd`, `touch`, `write`
- **File Inspection:** `cat`, `head`, `tail`, `hex`, `meta`, `diff`, `grep`, `checksum`, `preview`, `count`
- **System Information:** `sysinfo`, `cpuinfo`, `meminfo`, `diskinfo`, `sync`, `uptime`, `battery`, `sensors`, `devices`, `version`, `health`
- **Process Management:** `ps`, `kill`, `top`, `freeze`, `resume`, `services`, `service`, `jobs`, `priority`, `monitor`
- **System (cont.):** `winrun` (run a Windows PE32+ `.exe` via the in-kernel loader)
- **Networking:** `netinfo`, `ping`, `trace`, `ports`, `download`, `browse`, `upload`, `wifi`, `firewall`, `share`, `scan`
- **Language:** `python` (BoltPy interpreter / REPL), `js` (BoltJS interpreter, `js -c "…"` or `js FILE.js`); the **Code** app compiles C / C++ / C#
- **Bonus / Unique:** `focus`, `snapshot`, `timeline`, `vault`, `doctor`, `assistant`, `sandbox`, `workspace`, `panic`, `story`
- **Core:** `help`, `echo`, `clear`, `mem`

## Full File Tree

```text
+-- README.md
+-- BROWSER_UPGRADE.md     Browser engine upgrade plan + status log
+-- build.sh  run.sh       Build (-> disk image + ISO) and QEMU launcher
+-- build-and-run-vbox.bat  rebuild-all.ps1  rebuild-vdi.ps1   VirtualBox image helpers
+-- shot.py  testnet.py    GUI screendump + network test harnesses (over QEMU QMP)
+-- linker.ld
+-- boot/
|   +-- stage1.asm  stage2.asm
+-- doom/                  Vendored doomgeneric port (+ dg_libc.c, doomgeneric_boltos.c)
+-- drivers/
|   +-- ata.c    e1000.c   framebuffer.c  gpu.c   keyboard.c
|   +-- mouse.c  nvme.c     pcspk.c        xhci.c
+-- fs/
|   \-- ramfs.c            BoltFS (in-RAM tree, persisted via the block layer)
+-- include/               58 kernel/system headers (blk, dom, layout, js, nvme, xhci, rsa, x509, ...)
+-- kernel/
|   +-- main.c   boot.asm  shell.c        console.c   gui.c       hw.c
|   +-- sched.c  proc.c    syscall.c/.asm  elf.c       pe.c        idt.c
|   +-- interrupts.c  isr.asm  pic.c  pit.c  pci.c  serial.c  settings.c  vfs.c
|   +-- blk.c             Generic block layer (ATA/NVMe registry + MBR helpers)
|   +-- dom.c   layout.c  html.c          Browser engine: DOM tree, CSS cascade, box/flex/grid layout
|   +-- js.c    cmd_js.c  boltcc.c  boltpy.c   JavaScript, C/C++/C#, Python
|   +-- image.c  font8x8.c  kprintf.c  gdt.c  sysreg.c  user.asm
|   +-- cmd_extra.c  cmd_fs.c  cmd_net.c  cmd_proc.c  cmd_python.c  cmd_sys.c
|   \-- app_*.c           23 desktop apps (browser, files, ide, taskmgr, doom, paint, games, ...)
+-- libc/
|   \-- string.c
+-- mm/
|   +-- dma.c  kheap.c  pmm.c  vmm.c
+-- net/
|   +-- arp.c   eth.c    ip.c    icmp.c  udp.c   tcp.c   dns.c   netif.c
|   +-- http.c  inflate.c  firewall.c  driver.c  firmware.c  wifi.c
|   +-- tls.c   crypto.c  rsa.c   x509.c  p256.c  p384.c     TLS 1.2/1.3 + cert verification
\-- user/
    +-- crt0.asm  hello.c  winhello.c     Ring-3 ELF64 + a real PE32+ demo
    +-- ulibc.c/.h  boltrt.c  libm.c  stdlib_ext.c  stdio.h  stdlib.h  math.h  ctype.h
    \-- user.ld
```
