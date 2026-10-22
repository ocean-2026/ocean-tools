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

def send_checked(cmd, expect, name, retries=3):
    for i in range(retries):
        type_line(cmd)
        time.sleep(1.8)
        t = vga.screen_str()
        if expect in t[-600:]:
            shot(name)
            return True
        time.sleep(1.0)
    shot(name)
    return False

if not ensure_ot():
    print("OT FAILED"); sys.exit(1)
shot("screen-220-v14-start-final")

# help with 7 tools
send_checked("help", "hexedit", "screen-221-v14-help")

# hexedit: create small file -> save real name -> quit
entered = False
for a in range(4):
    type_line("hexedit /tmp/ndata.txt")
    time.sleep(2.2)
    if "Ctrl+S save" in vga.screen_str():
        entered = True
        break
    time.sleep(1.0)
if not entered:
    print("hexedit enter FAILED"); sys.exit(1)
for v in ["7","3","8","3","9"]:
    type_line(v)
    time.sleep(1.0)
typecmd.key("ctrl-s", 1.5)
t = vga.screen_str()
print("saved no-MOD:", "[MOD]" not in t)
typecmd.key("ctrl-q", 1.5)
time.sleep(1.2)
t = vga.screen_str()
if "unsaved" in t:
    typecmd.key("ctrl-q", 1.5); time.sleep(1.2)
print("hexedit exit ot back:", "ocean:/$" in vga.screen_str())

# plot reads the real file now
send_checked("plot /tmp/ndata.txt", "cannot", "screen-222a-plot-bad", 1)
send_checked("plot /tmp/ndata.txt", "row", "screen-222-v14-plot", 3)

# alias
send_checked("alias ll=dir /bin", "ll=dir /bin", "screen-223-v14-alias")
send_checked("ll", "bin", "screen-224-v14-aliasrun")

# shortcut demo.csc
send_checked("demo", "ocean", "screen-225-v14-shortcut")

# exit
send_checked("exit", "bye", "screen-226-v14-exit")
print("DONE13")
