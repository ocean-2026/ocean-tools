#!/usr/bin/env python3
"""mkce.py - wrap a flat i386 binary + ELF metadata into a CatOS CE01 image."""
import struct, subprocess, sys

def readelf_section_size(elf, sec):
    out = subprocess.check_output(
        ["readelf", "-S", "-W", elf], text=True, errors="replace")
    for line in out.splitlines():
        parts = line.split()
        # -W format: [ 4] .bss NOBITS <addr> <off> <size> ...
        if len(parts) >= 7 and parts[2] == sec:
            return int(parts[5], 16)
    return 0

def nm_symbol_addr(elf, name):
    out = subprocess.check_output(["nm", elf], text=True, errors="replace")
    for line in out.splitlines():
        p = line.split()
        if len(p) >= 3 and p[2] == name:
            return int(p[0], 16)
    return None

def main():
    elf, binfile, outfile = sys.argv[1], sys.argv[2], sys.argv[3]
    raw = open(binfile, "rb").read()
    api_off = nm_symbol_addr(elf, "__ce_api")
    bss_sz = readelf_section_size(elf, ".bss")
    assert api_off is not None, "no __ce_api symbol"
    assert len(raw) <= 64 * 1024, "image too big"
    LOAD_BASE = 0x02000000
    hdr = struct.pack("<4sIIII", b"CE01", 0, bss_sz, api_off - LOAD_BASE, len(raw))
    open(outfile, "wb").write(hdr + raw)
    print(f"api_offset={api_off - 0x02000000:#x} bss={bss_sz} image={len(raw)} -> {outfile}")

if __name__ == "__main__":
    main()
