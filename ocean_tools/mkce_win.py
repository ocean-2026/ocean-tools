#!/usr/bin/env python3
"""mkce_win.py - wrap raw flat binary into CatOS CE01 image (PE toolchain).
usage: python mkce_win.py in.bin out.ce bss_size api_offset  (hex, 0x optional)
"""
import struct
import sys


def to_int(s: str) -> int:
    s = s.strip().lower().replace("0x", "")
    return int(s, 16)


def main() -> None:
    if len(sys.argv) != 5:
        print("usage: mkce_win.py in.bin out.ce bss_size api_offset")
        sys.exit(1)
    with open(sys.argv[1], "rb") as f:
        data = f.read()
    bss = to_int(sys.argv[3])
    api = to_int(sys.argv[4])
    if len(data) > 0x10000:
        print("error: image too big (%d bytes, max 65536)" % len(data))
        sys.exit(1)
    hdr = b"CE01" + struct.pack("<IIII", 0, bss, api, len(data))
    with open(sys.argv[2], "wb") as f:
        f.write(hdr + data)
    print("%s: image=%d bss=0x%x api_off=0x%x" % (sys.argv[2], len(data), bss, api))


if __name__ == "__main__":
    main()
