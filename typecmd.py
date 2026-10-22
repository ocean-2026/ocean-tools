#!/usr/bin/env python3
"""CatOS 键盘注入器（十六进制键码版）：sendkey 0xNN，比键名可靠。
用法：python3 typecmd.py '命令串'   或  python3 typecmd.py 'run x' 'dir'
特殊键：在命令串中用 |f2 / |ctrl-s / |ret 等标记
"""
import socket, sys, time

MON = "/tmp/catos-mon.sock"

# Linux evdev / QEMU keycode 表（与 PS/2 set1 一致）
LETTERS = {c: k for c, k in zip(
    "abcdefghijklmnopqrstuvwxyz",
    [0x1E,0x30,0x2E,0x20,0x12,0x21,0x22,0x23,0x17,0x24,0x25,0x26,0x32,
     0x31,0x18,0x19,0x10,0x13,0x1F,0x14,0x16,0x2F,0x11,0x2D,0x15,0x2C])}
DIGITS = {str(i): 0x02 + i for i in range(1, 10)}
DIGITS['0'] = 0x0B
SYMB = {
    ' ': 0x39, '.': 0x34, '-': 0x0C, '/': 0x35, ':': 0x27,
    '_': 0x0C, '=': 0x0D, '+': 0x0D, ';': 0x27, ',': 0x33,
    "'": 0x28, '\\': 0x2B, '[': 0x1A, ']': 0x1B, '\t': 0x0F,
    '<': 0x33, '>': 0x34, '?': 0x35, '|': 0x2B, '~': 0x29, '`': 0x29,
    '!': 0x02, '@': 0x03, '#': 0x04, '$': 0x05, '%': 0x06, '^': 0x07,
    '&': 0x08, '*': 0x09, '(': 0x0A, ')': 0x0B, '"': 0x28,
}
# 特殊键（键名方式）：|f2 表示 F2，|ctrl-s 表示 Ctrl+S 等
SPECIAL = {'f1':'f1','f2':'f2','f3':'f3','f4':'f4','f5':'f5','f6':'f6',
           'f7':'f7','f8':'f8','esc':'esc','up':'up','down':'down','left':'left',
           'right':'right','home':'home','end':'end','pgup':'pgup','pgdn':'pgdn',
           'del':'delete','bs':'backspace','ins':'insert','tab':'tab',
           'ctrl-s':'ctrl-s','ctrl-o':'ctrl-o','ctrl-g':'ctrl-g','ctrl-z':'ctrl-z',
           'ctrl-y':'ctrl-y','ctrl-q':'ctrl-q','ctrl-c':'ctrl-c','ctrl-v':'ctrl-v',
           'ctrl-f':'ctrl-f','ctrl-h':'ctrl-h','ret':'ret','space':'spc'}

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

def key(k, gap=0.50):
    hmp(f"sendkey {k}")
    time.sleep(gap)

def type_line(text):
    key('esc', 0.2)   # 清理 shell 输入缓冲
    i = 0
    while i < len(text):
        ch = text[i]
        if ch == '|':
            j = text.index('|', i)
            k = text.index('|', j + 1) if '|' in text[j+1:] else len(text)
            name = text[j+1:k]
            key(SPECIAL.get(name, name))
            i = k
            continue
        kc = LETTERS.get(ch) or DIGITS.get(ch) or SYMB.get(ch)
        if kc is None:
            print(f"[skip {ch!r}]", file=sys.stderr)
            i += 1
            continue
        key(f"0x{kc:02x}")
        i += 1
    time.sleep(0.8)
    key("ret", 0.0)
    time.sleep(1.5)

if __name__ == "__main__":
    for line in sys.argv[1:]:
        type_line(line)
    print("typed:", " | ".join(sys.argv[1:]))
