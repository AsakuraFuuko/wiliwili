#!/usr/bin/env bash
# Run commands on the console through the shsrv payload (telnet, port 2323).
#
# shsrv prints the banner, then a prompt; commands are newline terminated and
# the reply is streamed back. It has a very small command set (no head/tail/wc),
# so callers should stick to ls/cat/df/du/rm/cp/mkdir/mount.
#
# usage: console-shell.sh <ps5-host> <command> [command...]
#
# environment:
#   PS5_SHSRV_PORT   shsrv port (default 2323)
#   PS5_SHELL_WAIT   seconds to wait per command (default 4)

set -uo pipefail

if [[ $# -lt 2 ]]; then
    echo "usage: $0 <ps5-host> <command> [command...]" >&2
    exit 2
fi

host=$1
shift
port=${PS5_SHSRV_PORT:-2323}
wait_seconds=${PS5_SHELL_WAIT:-4}

python3 - "$host" "$port" "$wait_seconds" "$@" <<'PY'
import socket
import sys
import time

host, port, wait_seconds = sys.argv[1], int(sys.argv[2]), float(sys.argv[3])
commands = sys.argv[4:]

connection = socket.create_connection((host, port), timeout=8)
connection.settimeout(max(1.0, wait_seconds))


def drain():
    text = b""
    try:
        while True:
            chunk = connection.recv(8192)
            if not chunk:
                break
            text += chunk
    except Exception:
        pass
    return text.decode("utf-8", "replace")


time.sleep(1.5)
drain()

for command in commands:
    connection.sendall((command + "\n").encode())
    time.sleep(wait_seconds)
    reply = drain()
    # Drop the banner/prompt noise: keep the lines the command produced.
    lines = [line for line in reply.splitlines() if line.strip() and not line.startswith("/$")]
    print(f"$ {command}")
    print("\n".join(lines))

connection.close()
PY
