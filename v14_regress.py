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

def type_raw(s):
    for ch in s:
        typecmd.key(typecmd.LETTERS.get(ch) and f"0x{typecmd.LETTERS[ch]:02x}" or typecmd.SYMB.get(ch) and f"0x{typecmd.SYMB[ch]:02x}" or None)
        time.sleep(0.2)

if not ensure_ot():
    print("OT FAILED"); sys.exit(1)

# 1) tab completion: cl + TAB
print(">>> cl<TAB>")
typecmd.key("0x2e", 0.9); typecmd.key("0x26", 0.9)   # c l
typecmd.key("tab", 0.9)
time.sleep(1.0)
shot("screen-190-v14-tab")

# clear + enter
typecmd.key("ret", 0.9); time.sleep(1.5)
# 2) alias
type_line("alias ll=dir /bin")
time.sleep(1.2)
shot("screen-191-v14-alias")
type_line("ll")
time.sleep(1.5)
shot("screen-192-v14-aliasrun")
# 3) history: up twice should recall ll
typecmd.key("up", 0.9); time.sleep(0.6)
typecmd.key("up", 0.9); time.sleep(0.6)
typecmd.key("ret", 0.9); time.sleep(1.5)
shot("screen-193-v14-hist")
# 4) shortcut: bare demo (csc default) 
type_line("demo")
time.sleep(1.8)
shot("screen-194-v14-shortcut")
# 5) help
type_line("help")
time.sleep(1.5)
shot("screen-195-v14-help")
print("DONE10")
