#!/bin/bash
# 一键复现 CatOS 在 QEMU 下的运行环境（本容器内已验证）
# 用法: bash run.sh   （QEMU 以后台方式启动，VGA 抓屏走 unix socket monitor）
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$DIR"

ISO="${1:-./catos.iso}"

# 容器文件系统对 qemu-img/-drive 的写锁不兼容，用 dd 建盘 + -blockdev locking=off 绕过
if [ ! -f disk.img ]; then
  dd if=/dev/zero of=disk.img bs=1M count=64 status=none
fi

qemu-system-x86_64 \
  -m 512 \
  -cdrom "$ISO" \
  -blockdev driver=file,node-name=diskfile,filename="$DIR/disk.img",locking=off \
  -blockdev driver=raw,node-name=diskc,file=diskfile \
  -device ide-hd,drive=diskc \
  -boot order=dc \
  -netdev user,id=n0 -device pcnet,netdev=n0 \
  -display none -vga std \
  -serial file:serial.log \
  -monitor unix:/tmp/catos-mon.sock,server,nowait \
  -no-reboot -no-shutdown \
  -daemonize -pidfile qemu.pid

echo "QEMU 已启动 (pid $(cat qemu.pid))。"
echo "VGA 抓屏:   python3 shot.py screen-NN"
echo "键盘输入:   python3 typecmd.py '命令'   (注意：需慢速输入，丢键时重试)"
echo "QEMU 监控:  python3 qmon.py /tmp/catos-mon.sock 'info status'"
