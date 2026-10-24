#!/bin/bash
# Build a CatOS CE01 image from a single C source in this directory.
# Usage: bash build.sh probe_time        -> probe_time.ce
#        bash build.sh adofai            -> adofai.ce
set -e
cd "$(dirname "$0")"
SRC="${1:-adofai}"
CFLAGS="-m32 -Os -ffreestanding -fno-pic -fno-stack-protector \
    -fno-asynchronous-unwind-tables -mno-sse -mno-mmx -mno-80387 \
    -nostdlib -fno-builtin -Wall -Wextra -Wno-unused-parameter"
echo "== compile $SRC.c =="
gcc $CFLAGS -c "$SRC.c" -o "$SRC.o"
ld -m elf_i386 -T ce.ld -o "$SRC.elf" "$SRC.o"
echo "== entry check =="
objdump -d "$SRC.elf" | head -4
objcopy -O binary "$SRC.elf" "$SRC.bin"
python3 mkce.py "$SRC.elf" "$SRC.bin" "$SRC.ce"
ls -la "$SRC.ce"
