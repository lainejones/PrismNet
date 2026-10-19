#!/usr/bin/env python3
"""Ask a PrismNet machine for its name the way other computers do (PrismNet).

  python namequery.py NAME [--host 127.0.0.1] [--nbport 137] [--mdnsport 5353]

Sends unicast queries (so they also work through a WinUAE slirp port
forward) and prints the answers:
  NetBIOS name query      NAME<20>        -> IPv4 address (like "ping NAME")
  NetBIOS node status     *               -> the name table (like "nbtstat -A")
  mDNS query              NAME.local A    -> IPv4 address (like "ping NAME.local")
Exit status 0 when all three answered.
"""
import argparse, random, socket, struct, sys


def nb_encode(name, suffix=0x20):
    raw = name.upper().ljust(15)[:15].encode() + bytes([suffix])
    return bytes([32]) + b"".join(bytes([0x41 + (c >> 4), 0x41 + (c & 15)]) for c in raw) + b"\0"


def ask(sock, pkt, addr):
    sock.sendto(pkt, addr)
    data, _ = sock.recvfrom(2048)
    return data


def netbios_query(sock, name, addr):
    tid = random.randint(0, 0xffff)
    pkt = struct.pack(">HHHHHH", tid, 0x0100, 1, 0, 0, 0) + nb_encode(name) + struct.pack(">HH", 0x20, 1)
    r = ask(sock, pkt, addr)
    if struct.unpack(">H", r[:2])[0] != tid or len(r) < 62:
        return None
    return socket.inet_ntoa(r[-4:])


def netbios_status(sock, addr):
    tid = random.randint(0, 0xffff)
    pkt = struct.pack(">HHHHHH", tid, 0, 1, 0, 0, 0) + nb_encode("*", 0) + struct.pack(">HH", 0x21, 1)
    r = ask(sock, pkt, addr)
    if struct.unpack(">H", r[:2])[0] != tid:
        return None
    off = 12 + 34 + 10           # header, name, type/class/ttl/rdlength
    n = r[off]
    names = []
    for i in range(n):
        e = r[off + 1 + i * 18: off + 1 + (i + 1) * 18]
        names.append("%s<%02x>" % (e[:15].decode(errors="replace").rstrip(), e[15]))
    return names


def mdns_query(sock, name, addr):
    tid = random.randint(0, 0xffff)
    q = b"".join(bytes([len(p)]) + p.encode() for p in (name, "local")) + b"\0"
    pkt = struct.pack(">HHHHHH", tid, 0, 1, 0, 0, 0) + q + struct.pack(">HH", 1, 1)
    r = ask(sock, pkt, addr)
    an = struct.unpack(">H", r[6:8])[0]
    if not an:
        return None
    return socket.inet_ntoa(r[-4:])      # the A record is the last thing in the answer


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("name")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--nbport", type=int, default=137)
    ap.add_argument("--mdnsport", type=int, default=5353)
    a = ap.parse_args()
    ok = 0
    for label, fn in (("NetBIOS name query", lambda s: netbios_query(s, a.name, (a.host, a.nbport))),
                      ("NetBIOS node status", lambda s: netbios_status(s, (a.host, a.nbport))),
                      ("mDNS query", lambda s: mdns_query(s, a.name, (a.host, a.mdnsport)))):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.settimeout(3)
        try:
            r = fn(s)
        except (socket.timeout, OSError) as e:
            r = None
            print("%-20s no answer (%s)" % (label, e.__class__.__name__))
        else:
            print("%-20s %s" % (label, r))
        if r:
            ok += 1
        s.close()
    return 0 if ok == 3 else 1


if __name__ == "__main__":
    sys.exit(main())
