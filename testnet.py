#!/usr/bin/env python3
# Boot BoltOS headless to the SERIAL shell (-vga none), type a command via QMP
# key events, capture serial output to a log. Deterministic network test.
# Usage: python testnet.py "command line" [boot_wait_s] [run_wait_s]
import socket, json, subprocess, sys, time, os

ROOT = os.path.dirname(os.path.abspath(__file__))
QEMU = r"C:\Program Files\qemu\qemu-system-x86_64.exe"
CMD  = sys.argv[1] if len(sys.argv) > 1 else "help"
BOOT = float(sys.argv[2]) if len(sys.argv) > 2 else 11.0
RUN  = float(sys.argv[3]) if len(sys.argv) > 3 else 12.0
PORT = 55557
LOG  = os.path.join(ROOT, "serial.log")
if os.path.exists(LOG): os.remove(LOG)

args = [QEMU,
  "-drive", f"id=boot,file={ROOT}\\iso\\os.img,format=raw,if=none",
  "-device","ide-hd,drive=boot,bus=ide.0,unit=0",
  "-drive", f"id=hdd,file={ROOT}\\iso\\disk-hdd.img,format=raw,if=none",
  "-device","ide-hd,drive=hdd,bus=ide.1,unit=0,rotation_rate=7200",
  "-drive", f"id=nvm,file={ROOT}\\iso\\disk-nvme.img,format=raw,if=none",
  "-device","nvme,drive=nvm,serial=BOLTNVME01",
  "-drive", f"id=ext2,file={ROOT}\\iso\\disk-ext2.img,format=raw,if=none",
  "-device","ide-hd,drive=ext2,bus=ide.0,unit=1",
  "-cpu","max","-smp","4","-m","2G","-rtc","base=utc",
  "-netdev","user,id=net0","-device","e1000,netdev=net0",
  "-device","qemu-xhci,id=xhci",
  "-device","usb-kbd,bus=xhci.0",
  "-vga","none","-display","none",
  "-serial", f"file:{LOG}",
  "-qmp", f"tcp:127.0.0.1:{PORT},server,nowait",
  "-no-reboot","-no-shutdown"]

p = subprocess.Popen(args)
def qmp():
    for _ in range(60):
        try: return socket.create_connection(("127.0.0.1", PORT), timeout=2)
        except OSError: time.sleep(0.5)
    raise SystemExit("QMP connect failed")
s = qmp(); f = s.makefile("rwb", buffering=0)
def c(d): f.write((json.dumps(d)+"\n").encode())
def r():
    l = f.readline(); return json.loads(l) if l else None
r(); c({"execute":"qmp_capabilities"}); r()
time.sleep(BOOT)

QC = {".":"dot","/":"slash","-":"minus"," ":"spc","_":"minus",";":"semicolon",
      "0":"0","1":"1","2":"2","3":"3","4":"4","5":"5","6":"6","7":"7","8":"8","9":"9"}
SH = {":":"semicolon","?":"slash","_":"minus","~":"grave_accent",
      ">":"dot","<":"comma","|":"backslash","&":"7"}
def key(qc, shift=False):
    ev=[]
    if shift: ev.append({"type":"key","data":{"down":True,"key":{"type":"qcode","data":"shift"}}})
    ev.append({"type":"key","data":{"down":True,"key":{"type":"qcode","data":qc}}})
    ev.append({"type":"key","data":{"down":False,"key":{"type":"qcode","data":qc}}})
    if shift: ev.append({"type":"key","data":{"down":False,"key":{"type":"qcode","data":"shift"}}})
    c({"execute":"input-send-event","arguments":{"events":ev}}); r(); time.sleep(0.05)
for ch in CMD:
    if ch in SH: key(SH[ch], True)
    elif ch in QC: key(QC[ch])
    elif ch.isalpha(): key(ch.lower(), ch.isupper())
    else: key(ch)
key("ret")
time.sleep(RUN)
c({"execute":"quit"})
try: p.wait(timeout=10)
except Exception: p.kill()
print("=== serial.log tail ===")
if os.path.exists(LOG):
    data = open(LOG,"rb").read().decode("latin1")
    print(data[-3000:])
