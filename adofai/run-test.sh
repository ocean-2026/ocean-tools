#!/bin/bash
# isolated VM for ADOFAI testing: separate disk copy + separate monitor socket
cd "$(dirname "$0")/.."
PATH="/home/user/bin:$PATH"
qemu-system-x86_64 \
  -L /home/user/qemu-bios -m 512 -cdrom ./catos.iso \
  -blockdev driver=file,node-name=diskfile,filename=./adofai/disk-test.img,locking=off \
  -blockdev driver=raw,node-name=diskc,file=diskfile -device ide-hd,drive=diskc \
  -boot order=dc -netdev user,id=n0 -device pcnet,netdev=n0 \
  -display none -vga std -serial file:adofai/serial-test.log \
  -monitor unix:/tmp/catos-mon2.sock,server,nowait \
  -no-reboot -no-shutdown -daemonize -pidfile adofai/qemu2.pid
 echo "test VM started (socket /tmp/catos-mon2.sock)"