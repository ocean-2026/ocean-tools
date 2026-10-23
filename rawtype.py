#!/usr/bin/env python3
"""rawtype: 无 esc 纯慢速注入。用法: python3 rawtype.py 'text' [gap]"""
import socket, sys, time
MON = "/tmp/catos-mon.sock"
LETTERS = {c: k for c, k in zip(
    "abcdefghijklmnopqrstuvwxyz",
    [0x1E,0x30,0x2E,0x20,0x12,0x21,0x22,0x23,0x17,0x24,0x25,0x26,0x32,
     0x31,0x18,0x19,0x10,0x13,0x1F,0x14,0x16,0x2F,0x11,0x2D,0x15,0x2C])}
DIGITS = {str(i): 0x02 + i for i in range(1, 10)}
DIGITS['0'] = 0x0B
SYMB = {' ': 0x39, '.': 0x34, '-': 0x0C, '/': 0x35, ':': 0x27,
        '_': 0x0C, '=': 0x0D, '+': 0x0D, ';': 0x27, ',': 0x33,
        "'": 0x28, '\\': 0x2B, '[': 0x1A, ']': 0x1B, '\t': 0x0F,
        '<': 0x33, '>': 0x34, '?': 0x35, '|': 0x2B, '~': 0x29, '`': 0x29,
        '!': 0x02, '@': 0x03, '#': 0x04, '$': 0x05, '%': 0x06, '^': 0x07,
        '&': 0x08, '*': 0x09, '(': 0x0A, ')': 0x0B, '"': 0x28}

def hmp(cmd):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.connect(MON)
        s.sendall(cmd.encode() + b"\r\n")
        s.settimeout(1.0)
        try:
            while True:
                c = s.recv(4096)
                if not c: break
                if b"(qemu) " in c: break
        except socket.timeout: pass

def main():
    gap = float(sys.argv[2]) if len(sys.argv) > 2 else 2.0
    text = sys.argv[1]
    for ch in text:
        if ch.isupper():
            kc = LETTERS.get(ch.lower())
            if kc:
                hmp(f"sendkey shift-0x{kc:02x}")
                time.sleep(gap)
                continue
        kc = LETTERS.get(ch) or DIGITS.get(ch) or SYMB.get(ch)
        if kc is None:
            print(f"[skip {ch!r}]", file=sys.stderr); continue
        hmp(f"sendkey 0x{kc:02x}")
        time.sleep(gap)
    hmp("sendkey ret")
    print("sent:", text, f"(gap={gap})")

if __name__ == "__main__":
    main()
