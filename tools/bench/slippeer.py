"""
slippeer.py - the far end of a SLIP link, for testing PrismNet's
point-to-point support in WinUAE without any real serial hardware.

WinUAE's serial port is set to a TCP server (serial_port=TCP://127.0.0.1:5555);
this script connects to it and speaks SLIP as the peer, 192.168.240.1 by
default (the Amiga side is 192.168.240.2):

  - ICMP echo to the peer address is answered (ping 192.168.240.1)
  - UDP to the peer port 7 is echoed; UDP to anywhere else is forwarded from
    the host and the replies come back (so nslookup to a real DNS server works)
  - TCP to the peer address, port P, is relayed to 127.0.0.1:P on the host
    (benchserver.py: http 8080, sink 8081), through a small TCP of our own

  python slippeer.py [--connect 127.0.0.1:5555] [--peer 192.168.240.1]
"""
import argparse
import select
import socket
import struct
import sys
import time

END, ESC, ESC_END, ESC_ESC = 0xC0, 0xDB, 0xDC, 0xDD
TRACE = '--trace' in sys.argv


def csum(data):
    if len(data) & 1:
        data += b"\0"
    s = sum(struct.unpack("!%dH" % (len(data) // 2), data))
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return (~s) & 0xFFFF


def ip_packet(src, dst, proto, payload, ident=[0]):
    ident[0] = (ident[0] + 1) & 0xFFFF
    h = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(payload), ident[0], 0, 64, proto, 0, src, dst)
    h = h[:10] + struct.pack("!H", csum(h)) + h[12:]
    return h + payload


def l4_csum(src, dst, proto, seg):
    return csum(src + dst + struct.pack("!BBH", 0, proto, len(seg)) + seg)


class Slip:
    def __init__(self, sock, rate=0):
        self.sock = sock
        self.rate = rate                    # bytes/s on the line, 0 = as fast as the emulator takes it
        self.next_t = 0.0
        self.buf = bytearray()
        self.esc = False
        self.tx_bytes = 0

    def send(self, pkt):
        out = bytearray([END])
        for b in pkt:
            if b == END:
                out += bytes([ESC, ESC_END])
            elif b == ESC:
                out += bytes([ESC, ESC_ESC])
            else:
                out.append(b)
        out.append(END)
        if self.rate:
            now = time.time()
            if self.next_t > now:
                time.sleep(self.next_t - now)
            self.next_t = max(now, self.next_t) + len(out) / float(self.rate)
        self.sock.sendall(out)
        self.tx_bytes += len(out)

    def feed(self, data):
        pkts = []
        for b in data:
            if self.esc:
                self.buf.append(END if b == ESC_END else ESC if b == ESC_ESC else b)
                self.esc = False
            elif b == ESC:
                self.esc = True
            elif b == END:
                if self.buf:
                    pkts.append(bytes(self.buf))
                self.buf = bytearray()
            else:
                self.buf.append(b)
        return pkts


class Tcp:
    """One relayed connection: Amiga <-> our TCP <-> host socket."""
    MSS = 960

    def __init__(self, peer, key, syn_seq, wnd, host_port, opts_mss):
        self.peer = peer
        self.key = key                      # (amiga_ip, amiga_port, our_ip, our_port)
        self.rcv_nxt = (syn_seq + 1) & 0xFFFFFFFF
        self.iss = int(time.time() * 1000) & 0xFFFFFFFF
        self.snd_una = self.iss
        self.snd_nxt = (self.iss + 1) & 0xFFFFFFFF
        self.snd_max = self.snd_nxt         # highest sequence sent; acks may reach it after a go-back
        self.wnd = wnd
        self.mss = min(self.MSS, opts_mss or 536)
        self.cwnd = 2 * self.mss            # small congestion window: slow start, back to 1 MSS on loss
        self.out = bytearray()              # unacked + unsent bytes, starting at snd_una (+1 before SYN acked)
        self.syn_acked = False
        self.host_eof = False
        self.fin_sent = False
        self.fin_seq = None
        self.amiga_fin = False
        self.last_progress = time.time()
        self.closed = False
        try:
            self.host = socket.create_connection(("127.0.0.1", host_port), timeout=3)
            self.host.setblocking(False)
        except OSError:
            self.host = None

    def seg(self, flags, seq, data=b"", mss_opt=False):
        a_ip, a_port, o_ip, o_port = self.key
        opts = struct.pack("!BBH", 2, 4, self.MSS) if mss_opt else b""
        off = (20 + len(opts)) // 4
        hdr = struct.pack("!HHIIBBHHH", o_port, a_port, seq, self.rcv_nxt, off << 4, flags,
                          16384, 0, 0) + opts
        tcp = hdr + data
        c = l4_csum(o_ip, a_ip, 6, tcp)
        tcp = tcp[:16] + struct.pack("!H", c) + tcp[18:]
        self.peer.slip.send(ip_packet(o_ip, a_ip, 6, tcp))

    def send_syn_ack(self):
        self.seg(0x12, self.iss, mss_opt=True)

    def pump(self):
        if not self.syn_acked:
            return
        inflight = (self.snd_nxt - self.snd_una) & 0xFFFFFFFF
        limit = min(self.wnd, self.cwnd)
        while inflight < limit:
            start = inflight
            chunk = bytes(self.out[start:start + min(self.mss, limit - inflight)])
            if not chunk:
                break
            if TRACE:
                print("%.3f out seq %d len %d" % (time.time() % 100, (self.snd_nxt - self.iss) & 0xFFFFFFFF, len(chunk)), flush=True)
            self.seg(0x18, self.snd_nxt, chunk)
            self.snd_nxt = (self.snd_nxt + len(chunk)) & 0xFFFFFFFF
            if ((self.snd_nxt - self.snd_max) & 0xFFFFFFFF) < 0x80000000:
                self.snd_max = self.snd_nxt
            inflight += len(chunk)
        if self.host_eof and not self.fin_sent and inflight == len(self.out):
            self.fin_seq = self.snd_nxt
            self.seg(0x11, self.snd_nxt)
            self.snd_nxt = (self.snd_nxt + 1) & 0xFFFFFFFF
            self.snd_max = self.snd_nxt
            self.fin_sent = True

    def on_segment(self, flags, seq, ack, wnd, data):
        if TRACE:
            print("%.3f in  fl %02x seq %d ack %d wnd %d len %d | una %d nxt %d cwnd %d" % (
                time.time() % 100, flags, (seq - self.rcv_nxt) & 0xFFFFFFFF, (ack - self.iss) & 0xFFFFFFFF,
                wnd, len(data), (self.snd_una - self.iss) & 0xFFFFFFFF, (self.snd_nxt - self.iss) & 0xFFFFFFFF,
                self.cwnd), flush=True)
        if flags & 0x04:                    # RST
            self.close()
            return
        self.wnd = wnd
        if flags & 0x10:                    # ACK
            if not self.syn_acked and ack == self.snd_nxt:
                self.syn_acked = True
                self.snd_una = ack
                self.last_progress = time.time()
            elif self.syn_acked:
                n = (ack - self.snd_una) & 0xFFFFFFFF
                if 0 < n <= ((self.snd_max - self.snd_una) & 0xFFFFFFFF):
                    if self.fin_sent and ack == self.snd_max:
                        n -= 1
                    del self.out[:n]
                    self.snd_una = ack
                    if ((self.snd_nxt - ack) & 0xFFFFFFFF) >= 0x80000000:
                        self.snd_nxt = ack      # acked past what we resent: carry on from there
                    self.cwnd = min(self.cwnd + n, 8 * self.mss)
                    self.last_progress = time.time()
        if data and seq == self.rcv_nxt:
            self.rcv_nxt = (self.rcv_nxt + len(data)) & 0xFFFFFFFF
            if self.host:
                try:
                    self.host.setblocking(True)
                    self.host.sendall(data)
                    self.host.setblocking(False)
                except OSError:
                    pass
        if flags & 0x01 and seq + len(data) == self.rcv_nxt:   # FIN in order
            self.rcv_nxt = (self.rcv_nxt + 1) & 0xFFFFFFFF
            self.amiga_fin = True
            if self.host:
                try:
                    self.host.shutdown(socket.SHUT_WR)
                except OSError:
                    pass
        if data or flags & 0x01:
            self.seg(0x10, self.snd_nxt)
        if self.fin_sent and self.amiga_fin and self.snd_una == self.snd_nxt:
            self.close()
        else:
            self.pump()

    def on_host_readable(self):
        try:
            d = self.host.recv(65536)
        except BlockingIOError:
            return
        except OSError:
            d = b""
        if d:
            self.out += d
        else:
            self.host_eof = True
        self.pump()

    def timer(self):
        if self.syn_acked and self.snd_una != self.snd_nxt and time.time() - self.last_progress > 1.0:
            self.snd_nxt = self.snd_una     # go back N, from one segment
            self.cwnd = self.mss
            self.fin_sent = False
            self.last_progress = time.time()
            self.pump()

    def close(self):
        self.closed = True
        if self.host:
            self.host.close()
            self.host = None


class Peer:
    def __init__(self, sock, peer_ip, rate=0):
        self.slip = Slip(sock, rate)
        self.ip = socket.inet_aton(peer_ip)
        self.udp = {}                       # (amiga_ip, amiga_port, dst_ip, dst_port) -> socket
        self.tcp = {}
        self.stats = {"icmp": 0, "udp_echo": 0, "udp_fwd": 0, "tcp_conn": 0}

    def handle(self, pkt):
        if len(pkt) < 20 or pkt[0] >> 4 != 4:
            return
        ihl = (pkt[0] & 15) * 4
        proto, src, dst = pkt[9], pkt[12:16], pkt[16:20]
        tot = struct.unpack("!H", pkt[2:4])[0]
        if csum(pkt[:ihl]) != 0 or tot > len(pkt) or (proto in (6, 17) and l4_csum(src, dst, proto, pkt[ihl:tot]) != 0):
            self.stats["bad_in"] = self.stats.get("bad_in", 0) + 1
            print("slippeer: bad frame from the Amiga, len %d (ip says %d): %s" % (len(pkt), tot, pkt[:48].hex()), flush=True)
            return
        body = pkt[ihl:struct.unpack("!H", pkt[2:4])[0]]
        if proto == 1 and body[:1] == b"\x08" and dst == self.ip:
            rep = b"\x00\x00\x00\x00" + body[4:]
            rep = rep[:2] + struct.pack("!H", csum(rep)) + rep[4:]
            self.slip.send(ip_packet(dst, src, 1, rep))
            self.stats["icmp"] += 1
        elif proto == 17 and len(body) >= 8:
            sport, dport = struct.unpack("!HH", body[:4])
            data = body[8:]
            if dst == self.ip and dport == 7:
                self.udp_reply(dst, dport, src, sport, data)
                self.stats["udp_echo"] += 1
            elif dst != self.ip:
                key = (src, sport, dst, dport)
                s = self.udp.get(key)
                if not s:
                    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                    s.setblocking(False)
                    self.udp[key] = s
                s.sendto(data, (socket.inet_ntoa(dst), dport))
                self.stats["udp_fwd"] += 1
        elif proto == 6 and len(body) >= 20 and dst == self.ip:
            sport, dport, seq, ack, off, flags, wnd = struct.unpack("!HHIIBBH", body[:16])
            hl = (off >> 4) * 4
            opts, data = body[20:hl], body[hl:]
            key = (src, sport, dst, dport)
            c = self.tcp.get(key)
            if flags & 0x02 and not flags & 0x10:          # SYN
                mss = 0
                i = 0
                while i < len(opts):
                    if opts[i] == 0:
                        break
                    if opts[i] == 1:
                        i += 1
                        continue
                    if opts[i] == 2 and i + 3 < len(opts):
                        mss = struct.unpack("!H", opts[i + 2:i + 4])[0]
                    i += max(2, opts[i + 1] if i + 1 < len(opts) else 2)
                if not c:
                    c = Tcp(self, key, seq, wnd, dport, mss)
                    if not c.host:
                        tmp = c
                        tmp.seg(0x14, 0)               # RST: nothing listening on the host
                        return
                    self.tcp[key] = c
                    self.stats["tcp_conn"] += 1
                c.send_syn_ack()
            elif c:
                c.on_segment(flags, seq, ack, wnd, data)
                if c.closed:
                    del self.tcp[key]

    def udp_reply(self, src_ip, sport, dst_ip, dport, data):
        u = struct.pack("!HHHH", sport, dport, 8 + len(data), 0) + data
        c = l4_csum(src_ip, dst_ip, 17, u) or 0xFFFF
        u = u[:6] + struct.pack("!H", c) + u[8:]
        self.slip.send(ip_packet(src_ip, dst_ip, 17, u))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--connect", default="127.0.0.1:5555")
    ap.add_argument("--peer", default="192.168.240.1")
    ap.add_argument("--mss", type=int, default=960, help="largest TCP segment we send")
    ap.add_argument("--trace", action="store_true", help="print every TCP segment")
    ap.add_argument("--rate", type=int, default=0, help="pace the line, bytes/s (115200 baud = 11520)")
    a = ap.parse_args()
    host, port = a.connect.rsplit(":", 1)
    while True:                             # one emulator session per pass
        try:
            session(host, int(port), a)
        except OSError as e:
            print("slippeer: line dropped (%s), waiting for the emulator" % e, flush=True)
        time.sleep(1)


def session(host, port, a):
    while True:
        try:
            sock = socket.create_connection((host, port), timeout=2)
            sock.settimeout(None)
            break
        except OSError:
            time.sleep(1)
    print("slippeer: connected to %s, peer %s" % (a.connect, a.peer), flush=True)
    Tcp.MSS = a.mss
    peer = Peer(sock, a.peer, a.rate)
    last_report = time.time()
    while True:
        rl = [sock] + list(peer.udp.values()) + [c.host for c in peer.tcp.values() if c.host]
        r, _, _ = select.select(rl, [], [], 0.2)
        for s in r:
            if s is sock:
                d = sock.recv(65536)
                if not d:
                    print("slippeer: emulator closed the line", flush=True)
                    return
                for p in peer.slip.feed(d):
                    peer.handle(p)
            else:
                for key, u in list(peer.udp.items()):
                    if u is s:
                        try:
                            data, _ = u.recvfrom(65536)
                        except OSError:
                            continue
                        a_ip, a_port, d_ip, d_port = key
                        peer.udp_reply(d_ip, d_port, a_ip, a_port, data)
                for key, c in list(peer.tcp.items()):
                    if c.host is s:
                        c.on_host_readable()
        for key, c in list(peer.tcp.items()):
            c.timer()
            if c.closed:
                del peer.tcp[key]
        if time.time() - last_report > 10:
            last_report = time.time()
            print("slippeer:", peer.stats, "tx", peer.slip.tx_bytes, flush=True)


if __name__ == "__main__":
    main()
