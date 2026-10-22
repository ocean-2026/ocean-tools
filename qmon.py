#!/usr/bin/env python3
"""Send an HMP command to the QEMU monitor over a unix socket."""
import socket, sys

sock_path = sys.argv[1]
cmd = sys.argv[2]
with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
    s.connect(sock_path)
    s.sendall(cmd.encode() + b"\r\n")
    s.settimeout(3)
    buf = b""
    try:
        while True:
            chunk = s.recv(4096)
            if not chunk:
                break
            buf += chunk
            if buf.count(b"(qemu) ") >= 2:
                break
    except socket.timeout:
        pass
    print(buf.decode(errors="replace"))
