#!/bin/bash
set -e
cd "$(dirname "$0")"
echo "== banner width check =="
python3 - <<'EOF'
import re
src = open("ocean_tools.c").read()
m = re.search(r'BANNER\[\] = \{(.*?)\};', src, re.S)
rows = re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1))
bad = [(i, len(r)) for i, r in enumerate(rows) if len(r) > 80]
print("rows:", len(rows), "max_width:", max(len(r) for r in rows))
for i, l in bad:
    print("  WARN row", i, "len", l)
assert not bad, "banner rows exceed 80 cols"
EOF
echo "== compile =="
gcc -m32 -Os -ffreestanding -fno-pic -fno-stack-protector \
    -fno-asynchronous-unwind-tables -mno-sse -mno-mmx -mno-80387 \
    -nostdlib -fno-builtin -Wall -Wextra -Wno-unused-parameter \
    -c ocean_tools.c -o ocean_tools.o
ld -m elf_i386 -T ce.ld -o ocean_tools.elf ocean_tools.o
echo "== entry check =="
objdump -d ocean_tools.elf | head -8
echo "== binary + header =="
objcopy -O binary ocean_tools.elf ocean_tools.bin
python3 mkce.py ocean_tools.elf ocean_tools.bin ocntools.ce
echo "== header hex =="
xxd -l 24 ocntools.ce
ls -la ocntools.ce
