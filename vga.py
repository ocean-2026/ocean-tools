import socket, re, time

MON = "/tmp/catos-mon.sock"

def q_cmd(cmd, timeout=1.0):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.connect(MON)
        s.settimeout(timeout)
        s.sendall(cmd.encode() + b"\r\n")
        data = b""
        try:
            while True:
                d = s.recv(8192)
                if not d: break
                data += d
                if b"(qemu) " in data:
                    # drain one more prompt cycle to avoid leftovers
                    try:
                        while True:
                            d2 = s.recv(8192)
                            if not d2: break
                            data += d2
                            if b"(qemu) " in data[data.rfind(b"(qemu) ")+8:]: break
                    except Exception: pass
                    break
        except Exception:
            pass
    return data.decode('latin1', 'replace')

def vga_text():
    """Read the full 25x80 text screen as a list of strings."""
    out = []
    for r in range(25):
        resp = q_cmd(f"xp /80hx 0xb8000+{r*160}")
        # response contains lines like: 00000000: 0x480x65... after the cmd echo
        vals = re.findall(r'0x([0-9a-f]{4})', resp)
        # pairs little-endian: word = attr<<8 | char ; char = lo byte
        chars = []
        for v in vals:
            w = int(v, 16)
            chars.append(chr(w & 0xFF))
        out.append(''.join(chars))
    return out

def screen_str():
    rows = vga_text()
    # strip trailing spaces per row
    return '\n'.join(r.rstrip() for r in rows)
