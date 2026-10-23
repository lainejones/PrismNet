"""Exercise PrismNet's httpd from the PC: python test/host/httptest.py <amiga> [port] [bigfile]
The server's ROOT must hold index.html at the top and a folder sub/ (no index) with
big.bin (bigfile here, compared byte for byte) and "a & b.txt"."""
import re, socket, sys, time

host = sys.argv[1]
port = int(sys.argv[2]) if len(sys.argv) > 2 else 80
big = open(sys.argv[3], "rb").read() if len(sys.argv) > 3 else None
fails = 0

def check(ok, what):
    global fails
    print(f"  {what:<55} {'ok' if ok else 'FAIL'}")
    if not ok: fails += 1

left = {}                               # bytes read past an answer, per socket

def read_answer(s, head=False):
    """(status, headers dict, body) of one answer on socket s"""
    buf = left.pop(s, b"")
    while b"\r\n\r\n" not in buf:
        d = s.recv(4096)
        if not d: break
        buf += d
    top, _, rest = buf.partition(b"\r\n\r\n")
    lines = top.decode("latin-1").split("\r\n")
    m = re.match(r"HTTP/1\.1 (\d+)", lines[0] if lines else "")
    hdr = {l.split(":", 1)[0].lower(): l.split(":", 1)[1].strip() for l in lines[1:] if ":" in l}
    n = 0 if head else int(hdr.get("content-length", "0"))
    while len(rest) < n:
        d = s.recv(65536)
        if not d: break
        rest += d
    left[s] = rest[n:]
    return (int(m.group(1)) if m else 0), hdr, rest[:n]

def get(path, extra="", method="GET", ver="1.1"):
    s = socket.create_connection((host, port), timeout=20)
    s.sendall(f"{method} {path} HTTP/{ver}\r\nHost: {host}\r\nConnection: close\r\n{extra}\r\n".encode("latin-1"))
    r = read_answer(s, head=(method == "HEAD"))
    s.close()
    return r

st, h, b = get("/")
check(st == 200 and b"PrismNet httpd test" in b, "/ gives index.html")
check(h.get("content-type", "").startswith("text/html"), "index.html is text/html")
st, h, b = get("/sub")
check(st == 301 and h.get("location") == "/sub/", "/sub redirects to /sub/")
st, h, b = get("/sub/")
check(st == 200 and b"big.bin" in b and b"a%20%26%20b.txt" in b and b"a &amp; b.txt" in b,
      "/sub/ lists, names escaped in links and text")
st, h, b = get("/sub/a%20%26%20b.txt")
check(st == 200 and b == b"special name\n", "a name with spaces and & is served")
t = time.time()
st, h, b = get("/sub/big.bin")
dt = time.time() - t
check(st == 200 and h.get("content-type") == "application/octet-stream", "big.bin 200, octet-stream")
if big is not None:
    check(b == big, f"big.bin byte-identical ({len(b)} bytes, {len(b) / dt / 1024:.0f} KB/s)")
st, h, b = get("/sub/big.bin", method="HEAD")
check(st == 200 and int(h.get("content-length", 0)) == 300000 and b == b"", "HEAD: length, no body")
st, h, b = get("/sub/big.bin", "Range: bytes=100-199\r\n")
check(st == 206 and len(b) == 100 and h.get("content-range") == "bytes 100-199/300000" and
      (big is None or b == big[100:200]), "Range a-b: 206, the right bytes")
st, h, b = get("/sub/big.bin", "Range: bytes=-50\r\n")
check(st == 206 and len(b) == 50 and (big is None or b == big[-50:]), "Range -n: the last n bytes")
st, h, b = get("/sub/big.bin", "Range: bytes=299990-\r\n")
check(st == 206 and len(b) == 10, "Range a-: to the end")
st, h, b = get("/sub/big.bin", "Range: bytes=400000-\r\n")
check(st == 416, "Range past the end: 416")
check(get("/nothere.html")[0] == 404, "missing file: 404")
for bad in ("/../", "/sub/..", "/%2e%2e/", "/sub/%2E%2E/sub", "/SYS:", "/sub//", "/RAM%3a/", "/a%5cb"):
    check(get(bad)[0] in (403, 400), f"{bad} refused")
check(get("/", method="POST")[0] == 405, "POST: 405")
check(get("/", ver="2.0")[0] == 505, "HTTP/2.0: 505")

# several requests on one connection, then Connection: close ends it
s = socket.create_connection((host, port), timeout=20)
codes = []
for i in range(5):
    s.sendall(f"GET / HTTP/1.1\r\nHost: {host}\r\n\r\n".encode())
    codes.append(read_answer(s)[0])
s.sendall(f"GET /sub/a%20%26%20b.txt HTTP/1.1\r\nHost: {host}\r\nConnection: close\r\n\r\n".encode())
st, h, b = read_answer(s)
s.settimeout(5)
try:
    closed = s.recv(1) == b""
except socket.timeout:
    closed = False
s.close()
check(codes == [200] * 5 and st == 200 and h.get("connection") == "close" and closed,
      "keep-alive: 6 requests on one connection, then closed")
# two requests sent at once (pipelined)
s = socket.create_connection((host, port), timeout=20)
s.sendall(f"GET / HTTP/1.1\r\nHost: x\r\n\r\nGET /sub/a%20%26%20b.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n".encode())
a1, a2 = read_answer(s), read_answer(s)
s.close()
check(a1[0] == 200 and a2[0] == 200 and a2[2] == b"special name\n", "pipelined requests both answered")
# HTTP/1.0 closes by default
s = socket.create_connection((host, port), timeout=20)
s.sendall(b"GET / HTTP/1.0\r\n\r\n")
st, h, b = read_answer(s)
s.settimeout(5)
try:
    closed = s.recv(1) == b""
except socket.timeout:
    closed = False
s.close()
check(st == 200 and closed, "HTTP/1.0: answered, then closed")
check(get("/", "X-Big: " + "x" * 9000 + "\r\n")[0] == 431, "9 KB of headers: 431")

print(f"httptest: {fails} of the checks failed")
sys.exit(1 if fails else 0)
