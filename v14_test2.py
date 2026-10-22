import sys, time, socket, os
sys.path.insert(0, os.getcwd())
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

def step(cmd, name, wait=1.2):
    print(">>>", cmd)
    type_line(cmd)
    time.sleep(wait)
    shot(name)

step("ocntools", "screen-140-v14-boot2", 2.5)
step("grep -i ocean demo.csc", "screen-141-v14-grep")
step("diff demo.csc info.txt", "screen-142-v14-diff")
step("vcs init", "screen-143-v14-vcsinit")
step("vcs add demo.csc", "screen-144-v14-vcsadd")
step("vcs commit first commit", "screen-145-v14-vcscommit")
step("vcs log", "screen-146-v14-vcslog")
step("cedbg /bin/ocntools.ce hdr", "screen-147-v14-cedbg")
print("DONE1")
