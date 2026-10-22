import sys, time, socket, os
sys.path.insert(0, os.getcwd())
import typecmd, vga
def slow_key(k, gap=0.9):
    typecmd.hmp(f"sendkey {k}")
    time.sleep(gap)
typecmd.key = slow_key
from typecmd import type_line

OUT = "/home/user/Doubao/chats/38448438262014978/catos-qemu"

def shot(name):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.connect(vga.MON)
        s.sendall(f"screendump {OUT}/{name}.ppm\n".encode())
        time.sleep(0.8)
        s.settimeout(1)
        try:
            while s.recv(4096): pass
        except Exception: pass
    os.system(f"python3 -c \"from PIL import Image; Image.open('{OUT}/{name}.ppm').save('{OUT}/{name}.png')\" 2>/dev/null")
    print("shot", name)

def ensure_ot():
    for i in range(4):
        if "ocean:/$" in vga.screen_str():
            return True
        type_line("ocntools")
        time.sleep(2.5)
    return "ocean:/$" in vga.screen_str()

# 1) hexedit create /tmp/data.txt with 5 numeric lines
def hexedit_flow():
    print(">>> hexedit /tmp/data.txt")
    type_line("hexedit /tmp/data.txt")
    time.sleep(2.0)
    shot("screen-180-v14-hexedit")
    for v in ["3","1","4","1","5"]:
        type_line(v)
        time.sleep(1.0)
    shot("screen-181-v14-hexedit-typed")
    type_line("|ctrl-s")
    time.sleep(1.5)
    type_line("|ctrl-q")
    time.sleep(1.5)
    shot("screen-182-v14-hexedit-exit")
    t = vga.screen_str()
    print("after hexedit, ot prompt back:", "ocean:/$" in t)

# 2) plot the data file
def plot_flow():
    print(">>> plot /tmp/data.txt")
    type_line("plot /tmp/data.txt")
    time.sleep(1.5)
    shot("screen-183-v14-plot")
    t = vga.screen_str()
    print("plot output has bars:", "|" in t or "#" in t or "[X]" in t or t.count("\n") > 20)

# 3) fmgr
def fmgr_flow():
    print(">>> fmgr")
    type_line("fmgr")
    time.sleep(2.0)
    shot("screen-184-v14-fmgr")
    t = vga.screen_str()
    print("fmgr screen shows dir:", "dir:" in t or "fmgr" in t)
    type_line("|ctrl-q")
    time.sleep(1.5)
    shot("screen-185-v14-fmgr-exit")
    print("fmgr exit, ot back:", "ocean:/$" in vga.screen_str())

if not ensure_ot():
    print("OT FAILED"); sys.exit(1)
hexedit_flow()
plot_flow()
fmgr_flow()
print("DONE6")
