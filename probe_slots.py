#!/usr/bin/env python3
"""Probe CatOS CE API slots one at a time via QEMU monitor."""
import socket, time, subprocess, re, sys

MON = "/tmp/catos-mon.sock"

def hmp(cmd, wait=0.15):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.connect(MON)
        s.sendall(cmd.encode() + b"\r\n")
        s.settimeout(2)
        buf = b""
        try:
            while True:
                c = s.recv(8192)
                if not c:
                    break
                buf += c
                if buf.count(b"(qemu) ") >= 2:
                    break
        except socket.timeout:
            pass
        time.sleep(wait)
        return buf

def read_screen():
    rows = []
    for row in range(25):
        out = hmp(f"xp /160bx {0xb8000 + row*160:#x}")
        nums = re.findall(r"0x([0-9a-f]{2})", out.decode(errors="replace"))
        chars = []
        for i in range(0, min(len(nums), 160), 2):
            c = int(nums[i], 16)
            chars.append(chr(c) if 32 <= c < 127 else " ")
        rows.append("".join(chars).rstrip())
    return rows

def state(rows):
    joined = "\n".join(rows)
    if "CatOS" in rows[0]:
        return "booting"
    if any("catos:/$" in r or r.strip().endswith("$") for r in rows):
        return "prompt"
    if "PROBE" in joined:
        return "probe_output"
    return "other"

def wait_prompt(timeout=40):
    t0 = time.time()
    while time.time() - t0 < timeout:
        rows = read_screen()
        st = state(rows)
        if st == "prompt":
            return rows
        time.sleep(1)
    return None

def type_line(text, gap=0.5):
    for ch in text:
        k = ch if ch.isalnum() else {' ': 'spc', '/': 'slash', '.': 'dot', '_': 'shift-minus', ':': 'shift-semicolon'}.get(ch)
        if k is None:
            continue
        hmp(f"sendkey {k}")
        time.sleep(gap)
    time.sleep(1.0)
    hmp("sendkey ret")
    time.sleep(3.0)

def probe_slot(i):
    rows = wait_prompt()
    if rows is None:
        return ("no_prompt", rows or [])
    type_line(f"probe {i} {i+1}")
    time.sleep(3)
    rows = read_screen()
    if "CatOS" in rows[0]:
        return ("CRASH/REBOOT", rows)
    st = state(rows)
    if st == "prompt":
        tail = [r for r in rows if r.strip()]
        return ("DONE", tail)
    return ("UNKNOWN", rows)

def main():
    slots = [int(x) for x in sys.argv[1:]]
    for i in slots:
        r = probe_slot(i)
        print(f"===== slot {i}: {r[0]}")
        for line in r[1][-12:]:
            print("  ", line)
        if r[0] == "CRASH/REBOOT":
            print(f"!! slot {i} crashed guest - stopping (resume later from {i+1})")
            return
        # if probe hung (no prompt, no crash) reboot and continue from next
        if r[0] == "UNKNOWN" or r[0] == "no_prompt":
            print(f"!! slot {i} no prompt - reboot needed (resume from {i+1})")
            return

if __name__ == "__main__":
    main()
