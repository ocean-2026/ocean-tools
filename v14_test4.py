import sys, time, socket, os
sys.path.insert(0, os.getcwd())
import typecmd, vga
_orig = typecmd.key
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

def screen_has(sub):
    return sub in vga.screen_str()

def send_checked(cmd, expect, name, retries=2):
    for i in range(retries):
        print(">>>", cmd, f"(try {i+1})")
        type_line(cmd)
        time.sleep(1.5)
        t = vga.screen_str()
        tail = t[-400:]
        if expect in tail:
            print("  OK:", expect)
            shot(name)
            return True
        print("  miss, tail:", repr(tail[-160:]))
        time.sleep(1.0)
    print("  FAILED after retries")
    shot(name)
    return False

send_checked("vcs add demo.csc", "tracked", "screen-160-v14-vcsadd")
send_checked("vcs commit third try", "committed", "screen-161-v14-vcscommit")
send_checked("vcs log", "vcs history", "screen-162-v14-vcslog")
send_checked("diff demo.csc info.txt", "diff", "screen-163-v14-diff")
send_checked("cedbg /bin/ocntools.ce hdr", "CE01", "screen-164-v14-cedbg")
send_checked("echo 11 hello ot tools", "hello ot tools", "screen-165-v14-echo")
send_checked("exit", "bye", "screen-166-v14-exit")
print("DONE3")
