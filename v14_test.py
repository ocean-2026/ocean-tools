import sys, time, socket, os
sys.path.insert(0, os.getcwd())
from typecmd import type_line, hmp, key

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

def step(cmd, name):
    print(">>>", cmd)
    type_line(cmd)
    time.sleep(1.2)
    shot(name)

# 1 grep
step("grep -i ocean DEMO.CSC", "screen-131-v14-grep")
# 2 diff
step("diff DEMO.CSC INFO.TXT", "screen-132-v14-diff")
# 3 vcs flow
step("vcs init", "screen-133-v14-vcsinit")
step("vcs add DEMO.CSC", "screen-134-v14-vcsadd")
step("vcs commit first commit", "screen-135-v14-vcscommit")
step("vcs log", "screen-136-v14-vcslog")
# 4 cedbg
step("cedbg /BIN/OCNTOOLS.CE hdr", "screen-137-v14-cedbg")
print("DONE")
