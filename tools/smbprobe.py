"""smbprobe.py host... - send one SMB2 NEGOTIATE (dialects 2.0.2..3.0.2) to each host
and print the dialect, signing mode and size limits it picks. No session is
set up and nothing is logged in to. Used for docs/SMBCLIENT.md."""
import socket, struct, sys, uuid

DIALECTS = [0x0202, 0x0210, 0x0300, 0x0302]

def negotiate(host):
    hdr = b'\xfeSMB' + struct.pack('<HHIHHIIQIIQ16s', 64, 0, 0, 0, 1, 0, 0, 0, 0xfeff, 0, 0, b'\0' * 16)
    body = struct.pack('<HHHHI16sQ', 36, len(DIALECTS), 1, 0, 0x7f, uuid.uuid4().bytes, 0)
    body += b''.join(struct.pack('<H', d) for d in DIALECTS)
    msg = hdr + body
    s = socket.create_connection((host, 445), timeout=5)
    s.sendall(struct.pack('>I', len(msg)) + msg)
    n = struct.unpack('>I', s.recv(4))[0] & 0xffffff
    data = b''
    while len(data) < n:
        data += s.recv(n - len(data))
    s.close()
    status = struct.unpack('<I', data[8:12])[0]
    b = data[64:]
    (size, secmode, dialect, _ctx, guid, caps, maxtrans, maxread, maxwrite) = \
        struct.unpack('<HHHH16sIIII', b[:40])
    return status, secmode, dialect, caps, maxtrans, maxread, maxwrite

for host in sys.argv[1:]:
    try:
        st, sm, d, caps, mt, mr, mw = negotiate(host)
        print(f'{host:14} status {st:#x} dialect {d:#06x} signing '
              f'{"REQUIRED" if sm & 2 else "enabled" if sm & 1 else "off"} '
              f'caps {caps:#x} maxread {mr} maxwrite {mw} maxtrans {mt}')
    except OSError as e:
        print(f'{host:14} {e}')
