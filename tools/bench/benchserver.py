#!/usr/bin/env python3
"""Host-side server for httpbench (PrismNet). GPL-3.0-only.

  python benchserver.py [--bind 127.0.0.1] [--http 8080] [--sink 8081]

HTTP: GET /<size> returns <size> bytes (suffix k or m, e.g. /4m); GET / is 1 MiB.
Sink: accepts a connection, discards everything until EOF, replies "OK <bytes>".
From a WinUAE slirp guest the host is 10.0.2.2.
"""
import argparse, socket, socketserver, threading

CHUNK = b"PrismNet" * 8192  # 64 KiB


def parse_size(path):
    p = path.strip("/").lower() or "1m"
    mul = 1
    if p[-1] in "km":
        mul = 1024 if p[-1] == "k" else 1024 * 1024
        p = p[:-1]
    try:
        return max(0, int(p)) * mul
    except ValueError:
        return None


class Http(socketserver.StreamRequestHandler):
    def handle(self):
        line = self.rfile.readline(1024).decode("latin-1").split()
        while self.rfile.readline(1024) not in (b"\r\n", b"\n", b""):
            pass
        size = parse_size(line[1]) if len(line) >= 2 and line[0] == "GET" else None
        if size is None:
            self.wfile.write(b"HTTP/1.0 404 Not Found\r\nContent-Length: 0\r\n\r\n")
            return
        self.wfile.write(b"HTTP/1.0 200 OK\r\nContent-Type: application/octet-stream\r\n"
                         b"Content-Length: %d\r\nConnection: close\r\n\r\n" % size)
        while size > 0:
            n = min(size, len(CHUNK))
            self.wfile.write(CHUNK[:n])
            size -= n


class Sink(socketserver.BaseRequestHandler):
    def handle(self):
        total = 0
        while True:
            d = self.request.recv(65536)
            if not d:
                break
            total += len(d)
        self.request.sendall(b"OK %d\n" % total)


# A client that goes away mid-transfer is normal here: httpbench's connect
# test opens and closes connections without a request, and a stopped
# emulator drops whatever was in flight. Count those, don't print tracebacks.
QUIET_ERRORS = (ConnectionResetError, ConnectionAbortedError, BrokenPipeError, TimeoutError)
dropped = 0


class TS(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def handle_error(self, request, client_address):
        global dropped
        import sys
        if isinstance(sys.exc_info()[1], QUIET_ERRORS):
            dropped += 1
            return
        super().handle_error(request, client_address)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bind", default="127.0.0.1")
    ap.add_argument("--http", type=int, default=8080)
    ap.add_argument("--sink", type=int, default=8081)
    a = ap.parse_args()
    servers = [TS((a.bind, a.http), Http), TS((a.bind, a.sink), Sink)]
    for s in servers:
        threading.Thread(target=s.serve_forever, daemon=True).start()
    print(f"benchserver: http {a.bind}:{a.http}, sink {a.bind}:{a.sink}", flush=True)
    threading.Event().wait()


if __name__ == "__main__":
    main()
