#!/usr/bin/env bash
# ===========================================================================
#  BoltOS build: stage1 + stage2 (nasm) + kernel (clang/lld) -> raw disk image
# ===========================================================================
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

# Detect msys64 root path (works from msys2, Git Bash, and WSL)
M=/c/msys64; [ -d "$M" ] || M=/mnt/c/msys64
[ -d "$M" ] || { echo "ERROR: msys64 not found at /c/msys64 or /mnt/c/msys64"; exit 1; }

export PATH="$M/ucrt64/bin:$M/usr/bin:$PATH"

NASM=$M/usr/bin/nasm.exe
CLANG=$M/ucrt64/bin/clang.exe
LLD=$M/ucrt64/bin/ld.lld.exe
OBJCOPY=$M/ucrt64/bin/objcopy.exe

CFLAGS=(--target=x86_64-elf -ffreestanding -fno-stack-protector -fno-pic -fno-pie
        -mcmodel=kernel
        -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -mno-80387
        -Wall -Wextra -O2 -std=c11 -Iinclude -c)

mkdir -p build iso

echo "[1/6] stage1.asm"
"$NASM" -f bin boot/stage1.asm -o build/stage1.bin

echo "[2/6] kernel asm (boot + isr + syscall)"
"$NASM" -f elf64 kernel/boot.asm    -o build/kboot.o
"$NASM" -f elf64 kernel/isr.asm     -o build/isr.o
"$NASM" -f elf64 kernel/syscall.asm -o build/sc_entry.o

echo "[2b/6] user program (user/hello.c) -> static ELF64 -> embed blob"
UCFLAGS=(--target=x86_64-elf -ffreestanding -fno-stack-protector -fno-pic -fno-pie
         -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -mno-80387
         -Wall -Wextra -O2 -std=c11 -Iuser -Iinclude -c)
"$NASM" -f elf64 user/crt0.asm -o build/u_crt0.o
"$CLANG" "${UCFLAGS[@]}" user/ulibc.c  -o build/u_ulibc.o
"$CLANG" "${UCFLAGS[@]}" user/hello.c  -o build/u_hello.o
"$CLANG" "${UCFLAGS[@]}" libc/string.c -o build/u_string.o
"$LLD" -m elf_x86_64 -T user/user.ld -no-pie -o build/hello.elf \
       build/u_crt0.o build/u_hello.o build/u_ulibc.o build/u_string.o
# embed the raw ELF as an object exposing _binary_hello_elf_start/_end
( cd build && "$OBJCOPY" -I binary -O elf64-x86-64 hello.elf hello_blob.o )

echo "[2c/6] windows program (user/winhello.c) -> real PE32+ EXE -> embed blob"
# A genuine Windows x86-64 console .exe: freestanding, imports kernel32, linked
# by the mingw toolchain. BoltOS's PE loader binds the imports to its shim.
# NB: built with SSE/MMX/x87 disabled to match the kernel (which runs with SSE
# off), so the PE contains only general-purpose-register code the loader can run.
"$CLANG" --target=x86_64-w64-mingw32 -nostdlib -ffreestanding -fno-stack-protector \
         -mno-sse -mno-sse2 -mno-mmx -mno-80387 -mno-red-zone \
         -O2 -Wl,-e,entry -Wl,--subsystem,console \
         user/winhello.c -o build/winhello.exe -lkernel32
( cd build && "$OBJCOPY" -I binary -O elf64-x86-64 winhello.exe winhello_blob.o )

echo "[3/6] kernel C sources"
SRCS=(
    kernel/main.c kernel/serial.c kernel/console.c kernel/shell.c kernel/kprintf.c kernel/font8x8.c
    kernel/gdt.c kernel/idt.c kernel/interrupts.c kernel/pic.c kernel/pit.c
    kernel/hw.c kernel/pci.c kernel/sysreg.c kernel/sched.c kernel/syscall.c
    kernel/vfs.c kernel/proc.c kernel/elf.c kernel/pe.c
    net/netif.c net/driver.c net/eth.c net/arp.c net/ip.c net/icmp.c net/udp.c
    net/tcp.c net/dns.c net/crypto.c net/tls.c net/http.c
    net/wifi.c net/firmware.c drivers/e1000.c
    kernel/cmd_fs.c kernel/cmd_sys.c kernel/cmd_proc.c kernel/cmd_net.c kernel/cmd_extra.c
    kernel/html.c kernel/image.c
    kernel/gui.c kernel/app_terminal.c kernel/app_taskmgr.c kernel/app_settings.c kernel/app_browser.c kernel/app_files.c
    kernel/app_ide.c kernel/app_calc.c kernel/app_clock.c kernel/app_notes.c kernel/app_calendar.c kernel/app_piano.c kernel/app_paint.c kernel/app_mines.c kernel/app_snake.c kernel/app_2048.c kernel/app_stopwatch.c kernel/app_sysinfo.c kernel/app_life.c kernel/app_ttt.c kernel/app_colorpick.c kernel/app_memory.c kernel/app_matrix.c
    kernel/settings.c
    kernel/boltpy.c kernel/cmd_python.c kernel/boltcc.c
    fs/ramfs.c
    drivers/keyboard.c drivers/framebuffer.c drivers/gpu.c drivers/mouse.c drivers/ata.c drivers/pcspk.c mm/pmm.c mm/vmm.c mm/kheap.c mm/dma.c libc/string.c
)
KOBJS=(build/kboot.o build/isr.o build/sc_entry.o build/hello_blob.o build/winhello_blob.o)
for c in "${SRCS[@]}"; do
    o="build/$(basename "${c%.c}").o"
    "$CLANG" "${CFLAGS[@]}" "$c" -o "$o"
    KOBJS+=("$o")
done

echo "[4/6] link kernel -> flat binary"
"$LLD" -m elf_x86_64 -T linker.ld --oformat binary -o build/kernel.bin "${KOBJS[@]}"

KSIZE=$(stat -c %s build/kernel.bin)
KSECT=$(( (KSIZE + 511) / 512 ))
echo "      kernel.bin = $KSIZE bytes -> $KSECT sectors"

echo "[5/6] stage2.asm (KERNEL_SECTORS=$KSECT)"
"$NASM" -f bin -DKERNEL_SECTORS="$KSECT" boot/stage2.asm -o build/stage2.bin
S2SIZE=$(stat -c %s build/stage2.bin)
if [ "$S2SIZE" -gt $((32 * 512)) ]; then
    echo "ERROR: stage2 is $S2SIZE bytes, exceeds 16384 (32 sectors)"; exit 1
fi

echo "[6/7] assemble disk image"
IMG=iso/os.img
TOTAL=$(( 33 + KSECT ))
dd if=/dev/zero          of="$IMG" bs=512 count="$TOTAL"      status=none
dd if=build/stage1.bin   of="$IMG" bs=512 seek=0  conv=notrunc status=none
dd if=build/stage2.bin   of="$IMG" bs=512 seek=1  conv=notrunc status=none
dd if=build/kernel.bin   of="$IMG" bs=512 seek=33 conv=notrunc status=none

echo "OK -> $IMG ($(stat -c %s "$IMG") bytes)"

# Persistent data disks for the filesystem. Created once and left alone on
# rebuilds so files survive across runs. One is presented to QEMU as a spinning
# HDD, the other as an SSD (rotation_rate flag in run.sh) - the ATA driver tells
# them apart from the IDENTIFY page. 64 MiB each.
DATA_MB=64
for d in iso/disk-hdd.img iso/disk-ssd.img; do
    if [ ! -f "$d" ]; then
        dd if=/dev/zero of="$d" bs=1M count="$DATA_MB" status=none
        echo "created $d (${DATA_MB} MiB)"
    fi
done

echo "[7/7] build bootable ISO"
P=/c/Users/adria/AppData/Local/Programs/Python/Python311/python.exe
[ -f "$P" ] || P=/mnt/c/Users/adria/AppData/Local/Programs/Python/Python311/python.exe
[ -f "$P" ] || { echo "ERROR: Python not found at /c/... or /mnt/c/..."; exit 1; }
PYTHON=$P
ISOPATH=iso/boltos.iso
"$PYTHON" mkhpfs.py "$IMG" "$ISOPATH" "BoltOS"
echo "OK -> $ISOPATH ($(stat -c %s "$ISOPATH") bytes)"
