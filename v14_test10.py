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
    for i in range(5):
        if "ocean:/$" in vga.screen_str():
            return True
        type_line("ocntools")
        time.sleep(2.5)
    return False

if not ensure_ot():
    print("OT FAILED"); sys.exit(1)

# hexedit clean flow
entered = False
for a in range(4):
    type_line("hexedit /tmp/data.txt")
    time.sleep(2.2)
    if "Ctrl+S save" in vga.screen_str():
        entered = True
        shot("screen-180-v14-hexedit")
        break
    time.sleep(1.0)
if not entered:
    print("hexedit FAILED"); sys.exit(1)
for v in ["3","1","4","1","5"]:
    type_line(v)
    time.sleep(1.0)
shot("screen-181-v14-hexedit-typed")
type_line("|ctrl-s")
time.sleep(1.5)
# quit; handle dirty confirm
type_line("|ctrl-q")
time.sleep(1.5)
t = vga.screen_str()
if "unsaved" in t:
    print("dirty confirm, sending ctrl-q again")
    type_line("|ctrl-q")
    time.sleep(1.5)
print("hexedit exited, ot back:", "ocean:/$" in vga.screen_str())
shot("screen-182-v14-hexedit-exit")

# plot
ok = False
for a in range(3):
    type_line("plot /tmp/data.txt")
    time.sleep(1.8)
    t = vga.screen_str()
    if "cannot read" not in t and "plot:" not in t and t.count("|") > 3:
        ok = True
        shot("screen-183-v14-plot")
        break
    time.sleep(1.0)
print("plot ok:", ok)
print("DONE9")
