"""Exercise PrismNet's telnetd from the PC: python test/host/telnettest.py <amiga> [user] [password]
Logs in, runs a few Shell commands, ends with EndCLI and checks the session closes."""
import re, socket, sys, time

host = sys.argv[1]
user = sys.argv[2] if len(sys.argv) > 2 else "ftptest"
pw = sys.argv[3] if len(sys.argv) > 3 else "ftptest-pw"
IAC, DONT, DO, WONT, WILL, SB, SE = 255, 254, 253, 252, 251, 250, 240
fails = 0

def check(ok, what):
    global fails
    print(f"  {what:<55} {'ok' if ok else 'FAIL'}")
    if not ok: fails += 1

s = socket.create_connection((host, 23), timeout=10)
opts = []

def read_until(pat, secs=15):
    """data (telnet commands stripped and noted) until regex pat or timeout"""
    buf, end = b"", time.time() + secs
    while time.time() < end:
        try:
            d = s.recv(4096)
        except socket.timeout:
            continue
        if not d:
            break
        i = 0
        while i < len(d):
            if d[i] == IAC and i + 2 < len(d) and d[i + 1] in (WILL, WONT, DO, DONT):
                opts.append((d[i + 1], d[i + 2])); i += 3; continue
            buf += d[i:i + 1]; i += 1
        if re.search(pat, buf.decode("latin-1")):
            break
    return buf.decode("latin-1")

def send(line):
    s.sendall(line.encode("latin-1") + b"\r\n")

banner = read_until(r"login: ")
check("login:" in banner, "banner and login prompt")
check((WILL, 1) in opts, "server offers WILL ECHO")
send("nopass")
read_until(r"Password: ")
send("anything")
again = read_until(r"login: ")
check("incorrect" in again, "account without a password refused")
send(user)
read_until(r"Password: ")
send(pw)
welcome = read_until(r"> ")
check("Welcome" in welcome, f"logged in as {user}")
check(re.search(r"\d+\.[^\r\n]*> $", welcome) is not None, "Shell prompt")
send("echo hello from the network")
out = read_until(r"> $")
check("hello from the network" in out, "echo")
send("version")
out = read_until(r"> $")
check("Kickstart" in out or "version" in out.lower(), "version")
send("cd RAM:")
read_until(r"> $")
send("cd")
out = read_until(r"> $")
check("RAM" in out.upper(), "cd RAM: then cd shows it")
send("list SYS:C/R#? quick")
out = read_until(r"> $", 20)
check("Rename" in out or "RequestChoice" in out, "list SYS:C/R#?")
check("\r\n" in out and "\n\n" not in out.replace("\r\n", "|"), "line ends are CR LF")
send("endcli")
time.sleep(2)
closed = False
try:
    s.settimeout(5)
    rest = s.recv(4096)
    closed = rest == b"" or s.recv(4096) == b""
except Exception:
    closed = False
check(closed, "EndCLI closes the connection")
s.close()
print(f"telnettest: {fails} failed -> {'FAIL' if fails else 'PASS'}")
