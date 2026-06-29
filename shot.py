#!/usr/bin/env python3
# Boot BoltOS headless, drive via QMP, screendump to PPM->PNG.
# Usage: python shot.py out.ppm [boot_wait_s] [extra clicks...]
import socket, json, subprocess, sys, time, os

ROOT = os.path.dirname(os.path.abspath(__file__))
QEMU = r"C:\Program Files\qemu\qemu-system-x86_64.exe"
OUT  = sys.argv[1] if len(sys.argv) > 1 else "shot.ppm"
WAIT = float(sys.argv[2]) if len(sys.argv) > 2 else 14.0
PORT = 55556

args = [QEMU,
  "-drive", f"id=boot,file={ROOT}\\iso\\os.img,format=raw,if=none",
  "-device","ide-hd,drive=boot,bus=ide.0,unit=0",
  "-drive", f"id=hdd,file={ROOT}\\iso\\disk-hdd.img,format=raw,if=none",
  "-device","ide-hd,drive=hdd,bus=ide.1,unit=0,rotation_rate=7200",
  "-drive", f"id=ssd,file={ROOT}\\iso\\disk-ssd.img,format=raw,if=none",
  "-device","ide-hd,drive=ssd,bus=ide.1,unit=1,rotation_rate=1",
  "-drive", f"id=nvm,file={ROOT}\\iso\\disk-nvme.img,format=raw,if=none",
  "-device","nvme,drive=nvm,serial=BOLTNVME01",
  "-m","2G","-rtc","base=utc","-vga","std","-global","VGA.vgamem_mb=64",
  "-netdev","user,id=net0","-device","e1000,netdev=net0",
  "-audiodev","none,id=snd0","-device","AC97,audiodev=snd0",
  "-display","none",
  "-qmp", f"tcp:127.0.0.1:{PORT},server,nowait",
  "-no-reboot","-no-shutdown"]

p = subprocess.Popen(args)
def qmp():
    for _ in range(60):
        try:
            s = socket.create_connection(("127.0.0.1", PORT), timeout=2); return s
        except OSError:
            time.sleep(0.5)
    raise SystemExit("QMP connect failed")

s = qmp(); f = s.makefile("rwb", buffering=0)
def cmd(d): f.write((json.dumps(d)+"\n").encode());
def rd():
    line = f.readline()
    return json.loads(line) if line else None
rd()  # greeting
cmd({"execute":"qmp_capabilities"}); rd()

print(f"booting, waiting {WAIT}s..."); time.sleep(WAIT)

# optional clicks: pairs of x,y passed as further argv
pos = [512, 384]   # PS/2 cursor starts at screen center
def move(x,y):
    dx, dy = x - pos[0], y - pos[1]
    cmd({"execute":"input-send-event","arguments":{"events":[
       {"type":"rel","data":{"axis":"x","value":dx}},
       {"type":"rel","data":{"axis":"y","value":dy}}]}}); rd()
    pos[0], pos[1] = x, y; time.sleep(0.2)
def tap():
    cmd({"execute":"input-send-event","arguments":{"events":[
      {"type":"btn","data":{"button":"left","down":True}}]}}); rd(); time.sleep(0.1)
    cmd({"execute":"input-send-event","arguments":{"events":[
      {"type":"btn","data":{"button":"left","down":False}}]}}); rd(); time.sleep(0.1)
def click(x,y):
    move(x,y); tap(); time.sleep(0.3)
def dblclick(x,y):
    move(x,y)
    for _ in range(2):
        cmd({"execute":"input-send-event","arguments":{"events":[
          {"type":"btn","data":{"button":"left","down":True}}]}}); rd(); time.sleep(0.12)
        cmd({"execute":"input-send-event","arguments":{"events":[
          {"type":"btn","data":{"button":"left","down":False}}]}}); rd(); time.sleep(0.12)
    time.sleep(0.5)

# argv items: "x,y" single click, "d:x,y" double click, "k:text" type, "s:secs" sleep
for a in sys.argv[3:]:
    if a.startswith("d:"):
        x,y = a[2:].split(","); dblclick(int(x),int(y))
    elif a.startswith("s:"):
        time.sleep(float(a[2:]))
    elif a.startswith("q:"):                       # raw qcode key tap, e.g. q:f12
        qc = a[2:]
        cmd({"execute":"input-send-event","arguments":{"events":[
          {"type":"key","data":{"down":True,"key":{"type":"qcode","data":qc}}}]}}); rd()
        cmd({"execute":"input-send-event","arguments":{"events":[
          {"type":"key","data":{"down":False,"key":{"type":"qcode","data":qc}}}]}}); rd()
        time.sleep(0.3)
    elif a.startswith("bs:"):
        for _ in range(int(a[3:])):
            cmd({"execute":"input-send-event","arguments":{"events":[
              {"type":"key","data":{"down":True,"key":{"type":"qcode","data":"backspace"}}}]}}); rd()
            cmd({"execute":"input-send-event","arguments":{"events":[
              {"type":"key","data":{"down":False,"key":{"type":"qcode","data":"backspace"}}}]}}); rd()
            time.sleep(0.04)
    elif a.startswith("k:"):
        QC = {".":"dot","/":"slash","-":"minus"," ":"spc","_":"minus","\t":"tab","=":"equal"}
        SH = {":":"semicolon","?":"slash","+":"equal","*":"8","(":"9",")":"0"}
        def key(qc, shift=False):
            ev=[]
            if shift: ev.append({"type":"key","data":{"down":True,"key":{"type":"qcode","data":"shift"}}})
            ev.append({"type":"key","data":{"down":True,"key":{"type":"qcode","data":qc}}})
            ev.append({"type":"key","data":{"down":False,"key":{"type":"qcode","data":qc}}})
            if shift: ev.append({"type":"key","data":{"down":False,"key":{"type":"qcode","data":"shift"}}})
            cmd({"execute":"input-send-event","arguments":{"events":ev}}); rd(); time.sleep(0.06)
        for ch in a[2:]:
            if ch == "\n" or ch == "~": key("ret")
            elif ch in SH:              key(SH[ch], True)
            elif ch in QC:              key(QC[ch])
            else:                       key(ch)
    else:
        x,y = a.split(","); click(int(x),int(y))

outpath = os.path.join(ROOT, OUT)
cmd({"execute":"screendump","arguments":{"filename":outpath}}); print(rd())
time.sleep(1.0)
cmd({"execute":"quit"})
try: p.wait(timeout=10)
except Exception: p.kill()
print("wrote", outpath, os.path.getsize(outpath) if os.path.exists(outpath) else "MISSING")
