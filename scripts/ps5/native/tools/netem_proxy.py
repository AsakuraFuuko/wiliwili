#!/usr/bin/env python3
"""带延迟/丢包的 HTTP(S) 代理——给"弱网降级"实验用。

用途：把 PS5 上应用的 `https_proxy` 指到本机，人为注入 RTT 与丢包，验证：
  - 队列缓解（lane 8 / inflight 6 / 无进度 6s 硬释放）在弱网下的表现
  - CA 裁剪后的握手是否仍 ~0.2s
  - 复用连接上那种 tls=0 / first=total≈1.2s 的慢 body 是 CDN 侧还是本机侧

用法：netem_proxy.py [--port 8899] [--delay-ms 150] [--jitter-ms 50] [--loss 0.03]
  --delay-ms  每个方向的首字节前延迟（模拟 RTT/2）
  --loss      按连接丢包概率（直接断开连接，模拟丢包导致的超时/重试）

说明：这是实验工具，只求行为可控、不追求吞吐；CONNECT 隧道逐块转发。
"""
import argparse
import random
import select
import socket
import sys
import threading
import time

BUFSIZE = 65536


def delayed(delay_ms: float, jitter_ms: float) -> None:
    if delay_ms <= 0 and jitter_ms <= 0:
        return
    wait = delay_ms + (random.uniform(0, jitter_ms) if jitter_ms else 0)
    if wait > 0:
        time.sleep(wait / 1000.0)


def pipe(src: socket.socket, dst: socket.socket, delay_ms: float, jitter_ms: float) -> None:
    try:
        while True:
            delayed(delay_ms, jitter_ms)
            chunk = src.recv(BUFSIZE)
            if not chunk:
                break
            delayed(delay_ms, jitter_ms)
            dst.sendall(chunk)
    except OSError:
        pass
    finally:
        for s in (src, dst):
            try:
                s.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass


def handle(client: socket.socket, args) -> None:
    try:
        client.settimeout(10)
        head = b""
        while b"\r\n\r\n" not in head and len(head) < 65536:
            part = client.recv(4096)
            if not part:
                return
            head += part
        line = head.split(b"\r\n", 1)[0].decode("latin-1")
        method, target, _ = (line.split(" ") + ["", ""])[:3]

        if args.loss and random.random() < args.loss:
            # 模拟丢包：握手前直接断开（客户端会超时/重试，RTO 行为更接近真实丢包）
            client.close()
            return

        if method.upper() == "CONNECT":
            host, _, port = target.partition(":")
            upstream = socket.create_connection((host, int(port or 443)), timeout=10)
            delayed(args.delay_ms, args.jitter_ms)
            client.sendall(b"HTTP/1.1 200 Connection Established\r\n\r\n")
        else:
            # 明文 HTTP：只处理绝对 URI 的 GET/POST，够实验用
            if not target.startswith("http://"):
                client.sendall(b"HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n")
                return
            rest = target[len("http://"):]
            hostport, _, path = rest.partition("/")
            host, _, port = hostport.partition(":")
            upstream = socket.create_connection((host, int(port or 80)), timeout=10)
            forwarded = head.replace(
                f"{method} http://{hostport}/{path}".encode("latin-1"),
                f"{method} /{path}".encode("latin-1"), 1)
            delayed(args.delay_ms, args.jitter_ms)
            upstream.sendall(forwarded)

        threading.Thread(target=pipe, args=(client, upstream, args.delay_ms, args.jitter_ms), daemon=True).start()
        threading.Thread(target=pipe, args=(upstream, client, args.delay_ms, args.jitter_ms), daemon=True).start()
    except OSError:
        try:
            client.close()
        except OSError:
            pass


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=8899)
    parser.add_argument("--delay-ms", type=float, default=150.0)
    parser.add_argument("--jitter-ms", type=float, default=50.0)
    parser.add_argument("--loss", type=float, default=0.0)
    args = parser.parse_args()

    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("0.0.0.0", args.port))
    server.listen(64)
    print(f"netem-proxy listening on :{args.port} delay={args.delay_ms}ms jitter={args.jitter_ms}ms loss={args.loss}",
          flush=True)
    while True:
        client, _ = server.accept()
        threading.Thread(target=handle, args=(client, args), daemon=True).start()


if __name__ == "__main__":
    sys.exit(main())
