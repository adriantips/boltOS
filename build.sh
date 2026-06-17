#!/usr/bin/env bash
# ===========================================================================
#  BoltOS build: stage1 + stage2 (nasm) + kernel (clang/lld) -> raw disk image
# ===========================================================================
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"

NASM=/c/msys64/usr/bin/nasm.exe
CLANG=/c/msys64/ucrt64/bin/clang.exe
LLD=/c/msys64/ucrt64/bin/ld.lld.exe

CFLAGS=(--target=x86_64-elf -ffreestanding -fno-stack-protector -fno-pic -fno-pie
        -mcmodel=kernel
        -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -mno-80387
        -Wall -Wextra -O2 -std=c11 -Iinclude -c)

mkdir -p build iso

echo "[1/6] stage1.asm"
"$NASM" -f bin boot/stage1.asm -o build/stage1.bin

echo "[2/6] kernel asm (boot + isr + syscall + user)"
"$NASM" -f elf64 kernel/boot.asm    -o build/kboot.o
"$NASM" -f elf64 kernel/isr.asm     -o build/isr.o
"$NASM" -f elf64 kernel/syscall.asm -o build/sc_entry.o
"$NASM" -f elf64 kernel/user.asm    -o build/user_blob.o

echo "[3/6] kernel C sources"
SRCS=(
    kernel/main.c kernel/serial.c kernel/console.c kernel/shell.c kernel/kprintf.c kernel/font8x8.c
    kernel/gdt.c kernel/idt.c kernel/interrupts.c kernel/pic.c kernel/pit.c
    kernel/hw.c kernel/pci.c kernel/sysreg.c kernel/sched.c
    net/netif.c net/driver.c net/eth.c net/arp.c net/ip.c net/icmp.c net/udp.c
    net/tcp.c net/dns.c net/http.c
    net/wifi.c net/firmware.c drivers/e1000.c
    kernel/cmd_fs.c kernel/cmd_sys.c kernel/cmd_proc.c kernel/cmd_net.c kernel/cmd_extra.c
    kernel/html.c
    kernel/gui.c kernel/app_terminal.c kernel/app_taskmgr.c kernel/app_settings.c kernel/app_browser.c
    kernel/settings.c
    fs/ramfs.c
    drivers/keyboard.c drivers/framebuffer.c drivers/mouse.c mm/pmm.c mm/vmm.c mm/kheap.c mm/dma.c libc/string.c
)
KOBJS=(build/kboot.o build/isr.o)
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

echo "[7/7] build bootable ISO"
PYTHON=/c/Users/adria/AppData/Local/Programs/Python/Python311/python.exe
ISOPATH=iso/boltos.iso
"$PYTHON" mkhpfs.py "$IMG" "$ISOPATH" "BoltOS"
echo "OK -> $ISOPATH ($(stat -c %s "$ISOPATH") bytes)"
