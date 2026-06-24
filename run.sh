#!/usr/bin/env bash
# Boot BoltOS in QEMU. Serial -> stdio so kernel output shows in the terminal.
# Extra args are passed through (e.g. -display sdl for a real window).
# An e1000 NIC on user-mode (slirp) NAT gives the browser real internet: slirp
# hands out 10.0.2.15/24, gateway 10.0.2.2, DNS 10.0.2.3 -- the kernel defaults.
ROOT="$(cd "$(dirname "$0")" && pwd)"

# Detect QEMU across msys2, Git Bash, and WSL mount styles
[ -f /c/Program\ Files/qemu/qemu-system-x86_64.exe ] && QEMU=/c/Program\ Files/qemu/qemu-system-x86_64.exe
[ -z "${QEMU-}" ] && [ -f /mnt/c/Program\ Files/qemu/qemu-system-x86_64.exe ] && QEMU=/mnt/c/Program\ Files/qemu/qemu-system-x86_64.exe
[ -z "${QEMU-}" ] && { echo "ERROR: QEMU not found"; exit 1; }

# Convert ROOT to Windows path when under WSL (QEMU is a Windows exe)
command -v wslpath &>/dev/null && ROOT=$(wslpath -w "$ROOT")
exec "$QEMU" \
    -drive id=boot,file="$ROOT/iso/os.img",format=raw,if=none \
    -device ide-hd,drive=boot,bus=ide.0,unit=0 \
    -drive id=cd,file="$ROOT/iso/boltos.iso",format=raw,if=none \
    -device ide-cd,drive=cd,bus=ide.0,unit=1 \
    -drive id=hdd,file="$ROOT/iso/disk-hdd.img",format=raw,if=none \
    -device ide-hd,drive=hdd,bus=ide.1,unit=0,rotation_rate=7200 \
    -drive id=ssd,file="$ROOT/iso/disk-ssd.img",format=raw,if=none \
    -device ide-hd,drive=ssd,bus=ide.1,unit=1,rotation_rate=1 \
    -drive id=nvm,file="$ROOT/iso/disk-nvme.img",format=raw,if=none \
    -device nvme,drive=nvm,serial=BOLTNVME01 \
    -device ich9-ahci,id=ahci \
    -drive id=fat,file="$ROOT/iso/disk-fat.img",format=raw,if=none \
    -device ide-hd,drive=fat,bus=ahci.0 \
    -drive id=ext2,file="$ROOT/iso/disk-ext2.img",format=raw,if=none \
    -device ide-hd,drive=ext2,bus=ide.0,unit=1 \
    -boot order=c \
    -cpu max \
    -m 2G \
    -smp 4 \
    -rtc base=utc \
    -vga std -global VGA.vgamem_mb=64 \
    -audiodev dsound,id=snd0 \
    -machine pcspk-audiodev=snd0 \
    -device AC97,audiodev=snd0 \
    -netdev user,id=net0 \
    -device e1000,netdev=net0 \
    -device qemu-xhci,id=xhci \
    -device usb-kbd,bus=xhci.0 \
    -device usb-mouse,bus=xhci.0 \
    -serial stdio \
    -no-reboot -no-shutdown \
    "$@"
