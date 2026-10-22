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

# plot real bars
send_checked("plot /tmp/plotdata.txt", "cannot", "screen-210a-v14-plot-bad", 1)
send_checked("plot /tmp/plotdata.txt", "row", "screen-210-v14-plot", 3)
# tab completion
typecmd.key("0x2e", 0.9); typecmd.key("0x26", 0.9); typecmd.key("tab", 0.9)
time.sleep(1.0)
shot("screen-211-v14-tab")
typecmd.key("ret", 0.9); time.sleep(1.5)
# alias
send_checked("alias ll=dir /bin", "alias", "screen-212-v14-alias")
send_checked("ll", "bin", "screen-213-v14-aliasrun")
# history up x2 + enter (should recall ll)
typecmd.key("up", 0.9); time.sleep(0.6); typecmd.key("up", 0.9); time.sleep(0.6)
typecmd.key("ret", 0.9); time.sleep(1.8)
shot("screen-214-v14-hist")
# shortcut bare demo.csc
send_checked("demo", "ocean", "screen-215-v14-shortcut")
# help
send_checked("help", "commands", "screen-216-v14-help")
print("DONE12")
