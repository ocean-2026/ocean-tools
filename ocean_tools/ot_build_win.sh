#!/bin/sh
# ot_build_win.sh - build ocean_tools CE on Windows (MinGW PE toolchain)
# Pipeline: gcc(m32) -> ld -m i386pe (ce.ld PE) -> objcopy -O binary -> mkce.py
set -e
cd "$(dirname "$0")"
export PATH=/mingw32/bin:/usr/bin:$PATH

echo "== banner width check =="
python3 - <<'EOF'
import re
src = open("ocean_tools.c").read()
m = re.search(r'BANNER\[\] = \{(.*?)\};', src, re.S)
rows = re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1))
bad = [(i, len(r)) for i, r in enumerate(rows) if len(r) > 80]
print("rows:", len(rows), "max_width:", max(len(r) for r in rows))
assert not bad, "banner rows exceed 80 cols"
EOF

echo "== compile =="
gcc -m32 -Os -ffreestanding -fno-pic -fno-stack-protector \
    -fno-asynchronous-unwind-tables -fno-stack-check -mno-stack-arg-probe \
    -mno-sse -mno-mmx -mno-80387 \
    -nostdlib -fno-builtin -Wall -Wextra -Wno-unused-parameter \
    -c ocean_tools.c -o ocean_tools.o

echo "== link (PE) =="
ld -m i386pe -T ot_ce_pe.ld -e _ce_main ocean_tools.o -o ocean_tools.exe

echo "== entry check =="
MAIN=$(nm ocean_tools.exe | awk '/[TD] _*ce_main$/{print $1; exit}')
echo "ce_main @ $MAIN"
if [ "$MAIN" != "02000000" ] && [ "$MAIN" != "2000000" ]; then
    echo "ERROR: ce_main not at image offset 0"; exit 1
fi

echo "== binary + header =="
objcopy -O binary -R .reloc ocean_tools.exe ocean_tools.bin
BSS=$(objdump -h ocean_tools.exe | awk '/\.bss/{print $3; exit}')
[ -z "$BSS" ] && BSS=0
API=$(nm ocean_tools.exe | awk '/[dDbBrR] _*_?ce_api$/{print $1; exit}')
if [ -z "$API" ]; then echo "ERROR: no __ce_api symbol"; exit 1; fi
API_OFF=$(printf '%x' $((16#$API - 16#2000000)))
echo "bss=$BSS api_off=$API_OFF"
python3 "$(pwd)/mkce_win.py" ocean_tools.bin ocntools.ce "$BSS" "$API_OFF"
ls -la ocntools.ce
