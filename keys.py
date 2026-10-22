#!/usr/bin/env python3
"""稳定键盘注入器：0.6s/键 + 可重复；用法 python3 keys.py 'run /bin/hello.ce'"""
import socket, sys, time

MON = "/tmp/catos-mon.sock"
KEYMAP = {
    ' ': 'spc', '.': 'dot', '-': 'minus', '/': 'slash', ':': 'shift-semicolon',
    '_': 'shift-minus', '=': 'equal', '+': 'shift-equal', '!': 'shift-1',
    '@': 'shift-2', '#': 'shift-3', '$': 'shift-4', '%': 'shift-5',
    '^': 'shift-6', '&': 'shift-7', '*': 'shift-8', '(': 'shift-9',
    ')': 'shift-0', '~': 'shift-grave', '`': 'grave', ';': 'semicolon',
    "'": 'apostrophe', '"': 'shift-apostrophe', ',': 'comma', '<': 'shift-comma',
    '>': 'shift-dot', '?': 'shift-slash', '\\': 'backslash', '|': 'shift-backslash',
    '[': 'bracket_left', ']': 'bracket_right', '{': 'shift-bracket_left',
    '}': 'shift-bracket_right', '\t': 'tab',
}

def hmp(cmd):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.connect(MON)
        s.sendall(cmd.encode() + b"\r\n")
        s.settimeout(1.0)
        try:
            while True:
                c = s.recv(4096)
                if not c:
                    break
                if b"(qemu) " in c:
                    break
        except socket.timeout:
            pass

def key(k, gap=1.0):
    hmp(f"sendkey {k}")
    time.sleep(gap)

def type_line(text):
    for ch in text:
        k = ch if ch.isalnum() else KEYMAP.get(ch)
        if k is None:
            print(f"[skip {ch!r}]", file=sys.stderr)
            continue
        key(k)
    time.sleep(1.0)
    key("ret", 0.0)
    time.sleep(2.0)

if __name__ == "__main__":
    for line in sys.argv[1:]:
        type_line(line)
    print("typed:", " | ".join(sys.argv[1:]))
