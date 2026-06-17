#!/usr/bin/env bash
# Boot BoltOS in QEMU. Serial -> stdio so kernel output shows in the terminal.
# Extra args are passed through (e.g. -display sdl for a real window).
# An e1000 NIC on user-mode (slirp) NAT gives the browser real internet: slirp
# hands out 10.0.2.15/24, gateway 10.0.2.2, DNS 10.0.2.3 -- the kernel defaults.
ROOT="$(cd "$(dirname "$0")" && pwd)"
QEMU=/c/Program\ Files/qemu/qemu-system-x86_64.exe
exec "$QEMU" \
    -drive file="$ROOT/iso/os.img",format=raw,if=ide \
    -drive file="$ROOT/iso/boltos.iso",if=ide,media=cdrom \
    -boot order=c \
    -m 512M \
    -netdev user,id=net0 \
    -device e1000,netdev=net0 \
    -serial stdio \
    -no-reboot -no-shutdown \
    "$@"
