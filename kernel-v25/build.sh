#!/bin/sh
# build.sh - build CatOS on Windows MinGW (no make required)
#
#   sh build.sh          -> build/kernel.bin + catos.iso
#   sh build.sh kernel   -> build/kernel.bin only
#   sh build.sh iso      -> kernel.bin + catos.iso
#
# ISO: built by build/patch_work_iso.py, which appends kernel.bin to the
# proven GRUB template build/catos_work.iso (El Torito no-emulation boot).
#
# Run the result with:
#   qemu-system-i386 -kernel build/kernel.bin -m 256
#   qemu-system-i386 -cdrom catos.iso -m 256
set -e

CC=gcc
LD=ld
OBJCOPY=objcopy
NM=nm
# -fleading-underscore: the asm files (cpu.s/boot.s) export/import symbols
# with a leading '_' (MinGW convention); on GNU/Linux gcc does not add it
# by default, so we ask gcc to match the asm exactly.
CFLAGS="-m32 -ffreestanding -fno-pic -fno-stack-protector -fleading-underscore -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -mno-3dnow -mno-80387 -Wall -O2 -std=c99 -Iinclude"

# find a working python (needed for the built-in ISO generator and for
# embedding the CE apps into the kernel image)
PY=""
if [ -n "$PYTHON" ] && command -v "$PYTHON" >/dev/null 2>&1; then
    PY="$PYTHON"
fi
if [ -z "$PY" ] && [ -x /d/Tools/Python312/python.exe ]; then
    PY=/d/Tools/Python312/python.exe
fi
if [ -z "$PY" ]; then
    for c in python3 py python; do
        if command -v "$c" >/dev/null 2>&1; then PY="$c"; break; fi
    done
fi

mkdir -p build

echo "== building CE apps (extapp) =="
for app in hello calc sysinfo demo curl netinfo spinner crash perms hw; do
    sh extapp/build_ce.sh $app.c
done

echo "== generating built-in app blobs =="
if [ -n "$PY" ]; then
    "$PY" build/gen_builtin.py
else
    echo "ERROR: no python found for gen_builtin.py" >&2
    exit 1
fi

echo "== compiling boot.s =="
$CC -m32 -c boot/boot.s -o build/boot.o
echo "== compiling cpu.s =="
$CC -m32 -c src/cpu.s -o build/cpu.o

for src in src/kernel.c src/vga.c src/gfx.c src/keyboard.c src/rtc.c src/shell.c src/fs.c src/ata.c src/fat.c src/ce.c src/net.c src/netstack.c src/builtin_apps.c src/intr.c src/paging.c src/task.c src/hw.c; do
    obj="build/$(basename $src .c).o"
    echo "== compiling $src =="
    $CC $CFLAGS -c $src -o $obj
done

echo "== linking (PE/COFF) =="
# NOTE: ld warns "section below image base" because our segments sit at
# 0x100000 (the i386 kernel load address) while PE's default image base
# is 0x400000.  Harmless: we only objcopy the flat image below.
# Do NOT add --image-base 0x100000: it makes objcopy emit a 1 MB zero
# gap at the start of kernel.bin, which breaks the multiboot image.
$LD -m i386pe -T pe.ld -o build/kernel.pe \
    build/boot.o build/cpu.o build/kernel.o build/vga.o build/gfx.o build/keyboard.o build/rtc.o build/shell.o build/fs.o build/ata.o build/fat.o build/ce.o build/net.o build/netstack.o build/builtin_apps.o build/intr.o build/paging.o build/task.o build/hw.o

echo "== extracting flat binary =="
$OBJCOPY -O binary build/kernel.pe build/kernel.bin
# NOTE: the bss is zero-filled into the flat file on purpose.  Truncating
# it at _load_end (letting the loader's [load_end, bss_end) fill do the
# zeroing) breaks on GRUB 2.06's iso9660+a.out-kludge path: the first
# timer interrupt returns with a smashed frame (observed EIP=1) - the
# loader does not honor the kludge fill for flat binaries.  Cost: the ISO
# carries ~4.6 MiB of zeros; correctness wins.
echo "OK -> build/kernel.bin"

build_iso() {
    echo "== building catos.iso =="
    # patch_work_iso.py appends kernel.bin to the proven bootable GRUB
    # template build/catos_work.iso.
    if [ -n "$PY" ]; then
        echo "using patch_work_iso.py (proven GRUB template)"
        "$PY" build/patch_work_iso.py
    else
        echo "ERROR: no python found. Set PYTHON=/path/to/python.exe or install python." >&2
        exit 1
    fi
    echo "OK -> catos.iso"
}

case "$1" in
    kernel) : ;;
    iso)     build_iso ;;
    *)       build_iso ;;
esac
