#!/usr/bin/env python3
"""v2.5 full retest: format disk -> inject happens outside -> run tests."""
import socket, time, subprocess, sys

MON = ('127.0.0.1', 4444)
OUT = '/home/user/Doubao/chats/38448438262014978/catos-qemu'
ISO = OUT + '/Catos_Better_better/Catos_Better_better/catos.iso'
RAW = '/tmp/disk-raw.img'

def cmd(s, c, w=0.4):
    s.sendall(c.encode() + b"\n"); time.sleep(w)

def type_text(s, t, spd=0.1):
    for ch in t:
        m = {' ': 'spc', '/': 'slash', '.': 'dot', '-': 'minus',
             '_': 'shift-minus', ':': 'shift-semicolon', '>': 'shift-dot', '|': 'shift-backslash'}
        if ch.isupper():
            cmd(s, 'sendkey shift-%s' % ch.lower(), spd)
        elif ch in m:
            cmd(s, 'sendkey %s' % m[ch], spd)
        else:
            cmd(s, 'sendkey %s' % ch, spd)

def shot(s, n):
    cmd(s, 'screendump %s/%s.png' % (OUT, n), 1.2)

subprocess.run(['pkill', '-x', 'qemu-system-i386'], capture_output=True)
time.sleep(1)
q = subprocess.Popen(['qemu-system-i386', '-m', '128', '-no-reboot', '-no-shutdown',
    '-cdrom', ISO, '-hda', RAW,
    '-netdev', 'user,id=n0', '-device', 'pcnet,netdev=n0',
    '-boot', 'order=d', '-display', 'none',
    '-monitor', 'tcp:127.0.0.1:4444,server,nowait'])
time.sleep(22)
s = socket.create_connection(MON, timeout=5)
time.sleep(1)

mode = sys.argv[1] if len(sys.argv) > 1 else 'format'
if mode == 'format':
    type_text(s, 'format')
    cmd(s, 'sendkey ret', 2.0); time.sleep(2.0)
    type_text(s, 'y')
    cmd(s, 'sendkey ret', 3.0); time.sleep(6.0)
    shot(s, 'k25_full_formatted')
elif mode == 'test':
    type_text(s, 'ls')
    cmd(s, 'sendkey ret', 2.0); time.sleep(2.0)
    shot(s, 'k25_full_ls')
    type_text(s, 'run /bin/minimal.ce')
    cmd(s, 'sendkey ret', 2.0); time.sleep(2.5)
    shot(s, 'k25_full_minimal')
    type_text(s, 'run /bin/hello.ce')
    cmd(s, 'sendkey ret', 2.0); time.sleep(2.5)
    shot(s, 'k25_full_hello')
    type_text(s, 'run /bin/hello.ce > /tmp/h.txt')
    cmd(s, 'sendkey ret', 2.0); time.sleep(6.0)
    type_text(s, 'run /bin/hello.ce >> /tmp/h.txt')
    cmd(s, 'sendkey ret', 2.0); time.sleep(6.0)
    shot(s, 'k25_full_redir')
    type_text(s, 'run /bin/test_ab.ce')
    cmd(s, 'sendkey ret', 2.0); time.sleep(20.0)
    shot(s, 'k25_full_testab')
s.close(); q.terminate()
print(mode, 'done')
