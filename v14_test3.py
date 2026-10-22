import sys, time, socket, os
sys.path.insert(0, os.getcwd())
import typecmd
# slow down to avoid dropped keys
_orig_key = typecmd.key
def slow_key(k, gap=0.9):
    typecmd.hmp(f"sendkey {k}")
    time.sleep(gap)
typecmd.key = slow_key
from typecmd import type_line

MON = "/tmp/catos-mon.sock"
OUT = "/home/user/Doubao/chats/38448438262014978/catos-qemu"

def shot(name):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.connect(MON)
        s.sendall(f"screendump {OUT}/{name}.ppm\n".encode())
        time.sleep(0.8)
        s.settimeout(1)
        try:
            while s.recv(4096): pass
        except Exception: pass
    os.system(f"python3 -c \"from PIL import Image; Image.open('{OUT}/{name}.ppm').save('{OUT}/{name}.png')\" 2>/dev/null")
    print("shot", name)

def step(cmd, name, wait=1.4):
    print(">>>", cmd)
    type_line(cmd)
    time.sleep(wait)
    shot(name)

step("ocntools", "screen-150-v14-boot3", 2.5)
# vcs full flow with fixed path
step("vcs init", "screen-151-v14-vcsinit")
step("vcs add demo.csc", "screen-152-v14-vcsadd")
step("vcs commit second try", "screen-153-v14-vcscommit")
step("vcs status", "screen-154-v14-vcsstatus")
step("vcs log", "screen-155-v14-vcslog")
print("DONE2")
