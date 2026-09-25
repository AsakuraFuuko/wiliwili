#!/usr/bin/env python3
"""Print the boot log a PS5 title streams over UDP.

A title can only write to its download data area, which the console persists
lazily, so the boot log helper in native_shims.c also sends every line as a UDP
datagram to the development host. This listener prints them as they arrive.

usage: log-listen.py [port]
"""

import socket
import sys
import time

port = int(sys.argv[1]) if len(sys.argv) > 1 else 9999

listener = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
listener.bind(("0.0.0.0", port))

print(f"listening for title logs on udp/{port}", flush=True)

while True:
    data, sender = listener.recvfrom(4096)
    stamp = time.strftime("%H:%M:%S")
    for line in data.decode("utf-8", "replace").splitlines():
        print(f"[{stamp}] {line}", flush=True)
