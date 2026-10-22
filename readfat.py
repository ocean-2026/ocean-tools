#!/usr/bin/env python3
"""Minimal FAT32 reader for CatOS disk.img: list tree + dump files to ./fat_out/."""
import struct, os, sys

IMG = "/home/user/Doubao/chats/38448438262014978/catos-qemu/disk.img"
OUT = "/home/user/Doubao/chats/38448438262014978/catos-qemu/fat_out"

def r(off, n):
    with open(IMG, "rb") as f:
        f.seek(off)
        return f.read(n)

bps = struct.unpack("<H", r(0x0B, 2))[0]
spc = r(0x0D, 1)[0]
rsvd = struct.unpack("<H", r(0x0E, 2))[0]
nfats = r(0x10, 1)[0]
fat_sz = struct.unpack("<I", r(0x24, 4))[0]
root_cl = struct.unpack("<I", r(0x2C, 4))[0]
fat_off = rsvd * bps
data_off = (rsvd + nfats * fat_sz) * bps

def fat_next(cl):
    if cl < 2:
        return 0
    ent = struct.unpack("<I", r(fat_off + cl * 4, 4))[0] & 0x0FFFFFFF
    return ent if ent < 0x0FFFFFF8 else 0

def cluster_off(cl):
    return data_off + (cl - 2) * spc * bps

def read_clusters(cl, size):
    out = b""
    while cl and len(out) < size:
        out += r(cluster_off(cl), spc * bps)
        cl = fat_next(cl)
    return out[:size]

def clean_name(entry):
    name = entry[0:8].decode("latin1").strip() if entry[0] != 0xE5 else "[del]"
    ext = entry[8:11].decode("latin1").strip()
    return name + ("." + ext if ext else "")

def walk(cl, prefix=""):
    buf = read_clusters(cl, 0xFFFFFFFF)
    items = []
    for i in range(0, len(buf), 32):
        e = buf[i:i+32]
        if e[0] == 0 or e[0] == 0xE5:
            continue
        if e[11] == 0x0F:  # LFN
            continue
        attr = e[11]
        fcl = struct.unpack("<I", e[20:24])[0] & 0x0FFFFF
        sz = struct.unpack("<I", e[28:32])[0]
        nm = clean_name(e)
        if attr & 0x10:  # dir
            print(f"[DIR ] {prefix}{nm}/")
            if nm not in (".", ".."):
                items.extend(walk(fcl, prefix + nm + "/"))
        else:
            print(f"[FILE] {prefix}{nm} ({sz} bytes)")
            os.makedirs(OUT, exist_ok=True)
            data = read_clusters(fcl, sz)
            with open(f"{OUT}/{prefix}{nm}".replace("/", "_"), "wb") as f:
                f.write(data)
    return items

walk(root_cl)
print("--- done ---")
