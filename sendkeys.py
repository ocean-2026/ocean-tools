#!/usr/bin/env python3
"""Type a string into the QEMU guest via HMP sendkey."""
import socket, sys, time

sock_path = sys.argv[1]
text = sys.argv[2]
delay = float(sys.argv[3]) if len(sys.argv) > 3 else 0.05

KEYMAP = {
    ' ': 'spc', '.': 'dot', '-': 'minus', '/': 'slash', ':': 'shift-semicolon',
    '_': 'shift-minus', '=': 'equal', '+': 'shift-equal', '!': 'shift-1',
    '@': 'shift-2', '#': 'shift-3', '$': 'shift-4', '%': 'shift-5',
    '^': 'shift-6', '&': 'shift-7', '*': 'shift-8', '(': 'shift-9',
    ')': 'shift-0', '~': 'shift-grave', '`': 'grave', ';': 'semicolon',
    "'": 'apostrophe', '"': 'shift-apostrophe', ',': 'comma', '<': 'shift-comma',
    '>': 'shift-dot', '?': 'shift-slash', '\\': 'backslash', '|': 'shift-backslash',
    '[': 'bracket_left', ']': 'bracket_right', '{': 'shift-bracket_left',
    '}': 'shift-bracket_right',
}

def send(cmd):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.connect(sock_path)
        s.sendall(cmd.encode() + b"\r\n")
        s.settimeout(1.5)
        try:
            while True:
                chunk = s.recv(4096)
                if not chunk:
                    break
                if b"(qemu) " in chunk:
                    break
        except socket.timeout:
            pass

for ch in text:
    if ch.isalnum():
        key = ch
    else:
        key = KEYMAP.get(ch)
        if key is None:
            print(f"[skip {ch!r}]", file=sys.stderr)
            continue
    send(f"sendkey {key}")
    time.sleep(delay)
send("ret")
