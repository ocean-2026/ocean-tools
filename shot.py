#!/usr/bin/env python3
"""Robust screendump: retry until the PPM file exists, then convert to PNG."""
import socket, subprocess, sys, time, os
from PIL import Image

MON = "/tmp/catos-mon.sock"
OUT = sys.argv[1] if len(sys.argv) > 1 else "shot"   # e.g. screen-09 (no ext)
workdir = "/home/user/Doubao/chats/38448438262014978/catos-qemu"

def hmp(cmd):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.connect(MON)
        s.sendall(cmd.encode() + b"\r\n")
        s.settimeout(2)
        try:
            while True:
                c = s.recv(4096)
                if not c:
                    break
                if b"(qemu) " in c:
                    break
        except socket.timeout:
            pass

ppm = f"/tmp/{OUT}.ppm"
for attempt in range(5):
    if os.path.exists(ppm):
        os.remove(ppm)
    hmp(f"screendump {ppm}")
    time.sleep(1.2)
    if os.path.exists(ppm) and os.path.getsize(ppm) > 0:
        img = Image.open(ppm)
        img.save(os.path.join(workdir, f"{OUT}.png"))
        print(f"OK {OUT}.png {img.size}")
        sys.exit(0)
    print(f"attempt {attempt+1} failed", file=sys.stderr)
    time.sleep(1)
sys.exit(1)
