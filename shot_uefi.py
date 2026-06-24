#!/usr/bin/env python3
# Boot BoltOS via UEFI (OVMF firmware) headless, screendump to PPM.
# Proves the BOOTX64.EFI loader brings the kernel up with no legacy BIOS/CSM.
# Usage: python shot_uefi.py out.ppm [boot_wait_s]
import socket, json, subprocess, sys, time, os, shutil
ROOT = os.path.dirname(os.path.abspath(__file__))
QEMU = r"C:\Program Files\qemu\qemu-system-x86_64.exe"
SHARE = r"C:\Program Files\qemu\share"
OUT  = sys.argv[1] if len(sys.argv) > 1 else "uefi.ppm"
WAIT = float(sys.argv[2]) if len(sys.argv) > 2 else 18.0
PORT = 55558

# OVMF: read-only code + a writable copy of the vars template
code = os.path.join(SHARE, "edk2-x86_64-code.fd")
vars_tmpl = os.path.join(SHARE, "edk2-i386-vars.fd")
vars_rw = os.path.join(ROOT, "build", "OVMF_VARS.fd")
if not os.path.exists(vars_rw):
    shutil.copy(vars_tmpl, vars_rw)

args = [QEMU,
  "-machine", "q35",
  "-drive", f"if=pflash,format=raw,readonly=on,file={code}",
  "-drive", f"if=pflash,format=raw,file={vars_rw}",
  "-drive", f"format=raw,file=fat:rw:{ROOT}\\esp",
  "-m","2G","-rtc","base=utc","-vga","std","-global","VGA.vgamem_mb=64",
  "-net","none","-display","none",
  "-qmp", f"tcp:127.0.0.1:{PORT},server,nowait",
  "-no-reboot"]
p = subprocess.Popen(args)
def qmp():
    for _ in range(60):
        try: return socket.create_connection(("127.0.0.1", PORT), timeout=2)
        except OSError: time.sleep(0.5)
    raise SystemExit("QMP connect failed")
s = qmp(); f = s.makefile("rwb", buffering=0)
def cmd(d): f.write((json.dumps(d)+"\n").encode())
def rd():
    line = f.readline(); return json.loads(line) if line else None
rd(); cmd({"execute":"qmp_capabilities"}); rd()
print(f"UEFI booting, waiting {WAIT}s..."); time.sleep(WAIT)
outpath = os.path.join(ROOT, OUT)
cmd({"execute":"screendump","arguments":{"filename":outpath}}); print(rd())
time.sleep(1.0)
cmd({"execute":"quit"})
try: p.wait(timeout=10)
except Exception: p.kill()
print("wrote", outpath, os.path.getsize(outpath) if os.path.exists(outpath) else "MISSING")
