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

def send_checked(cmd, expect, name, retries=2):
    for i in range(retries):
        print(">>>", cmd, f"(try {i+1})")
        type_line(cmd)
        time.sleep(1.5)
        t = vga.screen_str()
        if expect in t[-500:]:
            print("  OK:", expect)
            shot(name)
            return True
        print("  miss, tail:", repr(t[-200:]))
        time.sleep(1.0)
    print("  FAILED")
    shot(name)
    return False

send_checked("ocntools", "ready", "screen-170-v14-boot4", 1)
send_checked("diff demo.csc info.txt", "diff", "screen-171-v14-diff")
send_checked("diff demo.csc demo.csc -q", "identical", "screen-172-v14-diffsame")
print("DONE4")
