#!/usr/bin/env python3
"""CatOS FAT32 reader/writer (geometry: FAT @ s33, 2x1024 sectors, data @ s2081, spc=1).
NOTE: the kernel's `format` writes RSVD=32 into the BPB but LAYS OUT and READS
with RSVD=33 (FAT@s33, data@s2081).  The writer must follow the layout, not the BPB value."""
import struct, os

IMG = "/home/user/Doubao/chats/38448438262014978/catos-qemu/disk.img"
BPS = 512
SPC = 1
RSVD = 33
NFATS = 2
FAT_SZ = 1024
DATA_SEC = RSVD + NFATS * FAT_SZ   # 2081
FAT_OFF = RSVD * BPS
DATA_OFF = DATA_SEC * BPS

def r(off, n):
    with open(IMG, "rb") as f:
        f.seek(off); return f.read(n)

def w(off, data):
    with open(IMG, "r+b") as f:
        f.seek(off); f.write(data)

def fat_get(cl):
    return struct.unpack("<I", r(FAT_OFF + cl * 4, 4))[0] & 0x0FFFFFFF

def fat_set(cl, val):
    val &= 0x0FFFFFFF
    ent = struct.unpack("<I", r(FAT_OFF + cl * 4, 4))[0] & 0xF0000000
    w(FAT_OFF + cl * 4, struct.pack("<I", ent | val))
    w(FAT_OFF + FAT_SZ * BPS + cl * 4, struct.pack("<I", ent | val))

def cluster_off(cl):
    return DATA_OFF + (cl - 2) * SPC * BPS

def read_chain(cl):
    out = b""
    seen = set()
    while 2 <= cl < 0x0FFFFFF8 and cl not in seen:
        seen.add(cl)
        out += r(cluster_off(cl), SPC * BPS)
        cl = fat_get(cl)
    return out

def free_clusters():
    f = set()
    n = (DATA_SEC + 0)  # placeholder, computed below
    # max clusters = total_data_sectors
    total = 64 * 1024 * 1024 // BPS
    ncl = (total - DATA_SEC) // SPC
    free = []
    for cl in range(2, ncl):
        if fat_get(cl) == 0:
            free.append(cl)
    return free

def find_first_free_run(n):
    ncl = (64 * 1024 * 1024 // BPS - DATA_SEC) // SPC
    run = []
    for cl in range(2, ncl):
        if fat_get(cl) == 0:
            run.append(cl)
            if len(run) == n:
                return run
        else:
            run = []
    return None

def add_file(path, data):
    """Add a file into a directory (path like /bin/NAME.EXT) with 8.3 name only."""
    parts = [p for p in path.split("/") if p]
    fname = parts[-1].upper()
    base, ext = fname.rsplit(".", 1) if "." in fname else (fname, "")
    base8 = (base[:8] + " " * 8)[:8]
    # if base > 8 -> use 8.3 short form with ~1 (assume caller passes short name)
    assert len(base8) == 8 and len(ext) <= 3
    ext3 = (ext + "   ")[:3]

    # find parent dir cluster chain
    def find_dir(cl, segs):
        buf = read_chain(cl)
        for i in range(0, len(buf), 32):
            e = buf[i:i+32]
            if e[0] == 0 or e[0] == 0xE5 or e[11] == 0x0F:
                continue
            nm = e[0:8].decode("latin1").strip()
            if nm == segs[0] and e[11] & 0x10:
                fcl = struct.unpack("<H", e[26:28])[0] | (struct.unpack("<H", e[20:22])[0] << 16)
                if len(segs) == 1:
                    return fcl
                return find_dir(fcl, segs[1:])
        return None
    parent = 2 if len(parts) <= 1 else find_dir(2, [p.upper() for p in parts[:-1]])
    if parent is None:
        return False, "parent dir not found"

    # need a free slot in parent directory (chain of clusters)
    chain = []
    cl = parent
    seen = set()
    while 2 <= cl < 0x0FFFFFF8 and cl not in seen:
        seen.add(cl)
        chain.append(cl)
        cl = fat_get(cl)
    buf = read_chain(parent)
    # count used slots
    for i in range(0, len(buf), 32):
        e = buf[i:i+32]
        if e[0] == 0 or e[0] == 0xE5:
            continue
    slots_used = sum(1 for i in range(0, len(buf), 32)
                     if buf[i] != 0 and buf[i] != 0xE5)
    need_new_cluster = (slots_used % 16) == 0 and slots_used != 0
    if slots_used % 16 == 0 and slots_used > 0:
        # no free slot in current last cluster -> allocate new one
        free = find_first_free_run(1)
        if not free:
            return False, "no free clusters"
        newcl = free[0]
        last = chain[-1]
        fat_set(last, newcl)
        fat_set(newcl, 0x0FFFFFFF)
        w(cluster_off(newcl), b"\x00" * (SPC * BPS))
        chain.append(newcl)

    # file clusters
    nsec = (len(data) + SPC * BPS - 1) // (SPC * BPS)
    if nsec == 0:
        nsec = 1
    run = find_first_free_run(nsec)
    if not run:
        return False, "no free clusters for data"
    for i, cl in enumerate(run):
        start = i * SPC * BPS
        chunk = data[start:start + SPC * BPS]
        chunk = chunk + b"\x00" * (SPC * BPS - len(chunk))
        w(cluster_off(cl), chunk)
        nxt = run[i + 1] if i + 1 < len(run) else 0x0FFFFFFF
        fat_set(cl, nxt)

    # directory entry at first free slot (scan whole directory chain incl. new cluster)
    ent = b""
    ent += base8.encode("latin1")            # 0-7   name
    ent += ext3.encode("latin1")             # 8-10  ext
    ent += bytes([0x20])                     # 11    attr: archive
    ent += b"\x00" * 8                       # 12-19 reserved + times
    ent += struct.pack("<H", 0)              # 20-21 high cluster
    ent += b"\x00" * 4                       # 22-25 mtime mdate
    ent += struct.pack("<H", run[0] & 0xFFFF)  # 26-27 low cluster
    ent += struct.pack("<I", len(data))      # 28-31 size
    assert len(ent) == 32
    for ci, ccl in enumerate(chain):
        cb = read_chain(ccl)[:SPC * BPS]
        for j in range(0, len(cb), 32):
            if cb[j] == 0 or cb[j] == 0xE5:
                w(cluster_off(ccl) + j, ent)
                return True, f"added at cluster {ccl} slot {j//32}"
    return False, "no slot"

def dump_tree(cl, prefix=""):
    buf = read_chain(cl)
    out = []
    for i in range(0, len(buf), 32):
        e = buf[i:i+32]
        if e[0] == 0 or e[0] == 0xE5 or e[11] == 0x0F:
            continue
        attr = e[11]
        fcl = struct.unpack("<H", e[26:28])[0] | (struct.unpack("<H", e[20:22])[0] << 16)
        sz = struct.unpack("<I", e[28:32])[0]
        nm = e[0:8].decode("latin1").rstrip()
        ex = e[8:11].decode("latin1").rstrip()
        name = nm + ("." + ex if ex else "")
        if attr & 0x10:
            out.append(f"[DIR ] {prefix}{name}/ (cl{fcl})")
            if name not in (".", ".."):
                out.extend(dump_tree(fcl, prefix + name + "/"))
        else:
            out.append(f"[FILE] {prefix}{name} ({sz}B cl{fcl})")
    return out

if __name__ == "__main__":
    import sys
    if sys.argv[1] == "dump":
        for line in dump_tree(2):
            print(line)
    elif sys.argv[1] == "add":
        ok, msg = add_file(sys.argv[2], open(sys.argv[3], "rb").read())
        print(ok, msg)


def delete_file(path):
    """Delete a file: mark entry deleted (0xE5) and free its clusters."""
    parts = [p for p in path.split("/") if p]
    fname = parts[-1].upper()
    base = fname.split(".")[0]
    base8 = (base[:8] + "      ")[:8]
    ext = fname.split(".")[1] if "." in fname else ""
    ext3 = (ext + "   ")[:3]

    def find_dir(cl, segs):
        buf = read_chain(cl)
        for i in range(0, len(buf), 32):
            e = buf[i:i+32]
            if e[0] == 0 or e[0] == 0xE5 or e[11] == 0x0F:
                continue
            nm = e[0:8].decode("latin1").strip()
            if nm == segs[0] and e[11] & 0x10:
                fcl = struct.unpack("<H", e[26:28])[0] | (struct.unpack("<H", e[20:22])[0] << 16)
                if len(segs) == 1:
                    return fcl
                return find_dir(fcl, segs[1:])
        return None
    parent = 2 if len(parts) <= 1 else find_dir(2, [p.upper() for p in parts[:-1]])
    if parent is None:
        return False, "parent not found"
    chain = []
    cl = parent
    seen = set()
    while 2 <= cl < 0x0FFFFFF8 and cl not in seen:
        seen.add(cl)
        chain.append(cl)
        cl = fat_get(cl)
    offset_in_chain = 0
    for ccl in chain:
        buf = r(cluster_off(ccl), SPC * BPS)
        for i in range(0, len(buf), 32):
            e = buf[i:i+32]
            if e[0] == 0 or e[0] == 0xE5 or e[11] == 0x0F:
                offset_in_chain += 32
                continue
            nm = e[0:8].decode("latin1").rstrip()
            ex = e[8:11].decode("latin1").rstrip()
            if nm == base8.rstrip() and ex == ext3.rstrip():
                fcl = struct.unpack("<H", e[26:28])[0] | (struct.unpack("<H", e[20:22])[0] << 16)
                # mark deleted
                w(cluster_off(ccl) + i, b"\xe5")
                # free cluster chain
                fc = fcl
                fseen = set()
                while 2 <= fc < 0x0FFFFFF8 and fc not in fseen:
                    fseen.add(fc)
                    nxt = fat_get(fc)
                    fat_set(fc, 0)
                    fc = nxt
                return True, f"deleted {base8}.{ext3} (freed {len(fseen)} clusters)"
            offset_in_chain += 32
    return False, "entry not found"

def add_file_safe(path, data):
    """Add file with correct slot math across the parent's cluster chain."""
    parts = [p for p in path.split("/") if p]
    fname = parts[-1].upper()
    base, ext = fname.rsplit(".", 1) if "." in fname else (fname, "")
    base8 = (base[:8] + "      ")[:8]
    ext3 = (ext + "   ")[:3]

    def find_dir(cl, segs):
        buf = read_chain(cl)
        for i in range(0, len(buf), 32):
            e = buf[i:i+32]
            if e[0] == 0 or e[0] == 0xE5 or e[11] == 0x0F:
                continue
            nm = e[0:8].decode("latin1").strip()
            if nm == segs[0] and e[11] & 0x10:
                fcl = struct.unpack("<H", e[26:28])[0] | (struct.unpack("<H", e[20:22])[0] << 16)
                if len(segs) == 1:
                    return fcl
                return find_dir(fcl, segs[1:])
        return None
    parent = 2 if len(parts) <= 1 else find_dir(2, [p.upper() for p in parts[:-1]])
    if parent is None:
        return False, "parent dir not found"

    chain = []
    cl = parent
    seen = set()
    while 2 <= cl < 0x0FFFFFF8 and cl not in seen:
        seen.add(cl)
        chain.append(cl)
        cl = fat_get(cl)

    # find free slot
    used_last = 0
    slot_off = None
    for ccl in chain:
        buf = r(cluster_off(ccl), SPC * BPS)
        for i in range(0, len(buf), 32):
            if buf[i] == 0 or buf[i] == 0xE5:
                slot_off = cluster_off(ccl) + i
                break
            used_last += 1
        if slot_off is not None:
            break
    if slot_off is None:
        # append a new cluster to parent
        free = find_first_free_run(1)
        if not free:
            return False, "no free clusters"
        newcl = free[0]
        fat_set(chain[-1], newcl)
        fat_set(newcl, 0x0FFFFFFF)
        w(cluster_off(newcl), b"\x00" * (SPC * BPS))
        chain.append(newcl)
        slot_off = cluster_off(newcl)

    # allocate data clusters
    nsec = (len(data) + SPC * BPS - 1) // (SPC * BPS)
    if nsec == 0:
        nsec = 1
    run = find_first_free_run(nsec)
    if not run:
        return False, "no free clusters for data"
    for i, cl in enumerate(run):
        start = i * SPC * BPS
        chunk = data[start:start + SPC * BPS]
        chunk = chunk + b"\x00" * (SPC * BPS - len(chunk))
        w(cluster_off(cl), chunk)
        fat_set(cl, run[i + 1] if i + 1 < len(run) else 0x0FFFFFFF)

    ent = base8.encode("latin1") + ext3.encode("latin1") + bytes([0x20]) + b"\x00" * 10
    ent += b"\x00\x00"                       # mtime
    ent += b"\x00\x00"                       # mdate
    ent += struct.pack("<H", run[0] & 0xFFFF)  # low cluster
    ent += struct.pack("<I", len(data))        # size
    assert len(ent) == 32
    w(slot_off, ent)
    return True, f"added {base8}.{ext3} ({len(data)}B, {len(run)} clusters, slot@{slot_off//512})"
