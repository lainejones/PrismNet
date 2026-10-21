"""classicimg.py IN.info OUT.info [PREVIEW.png] - give an OS 3.5 colour icon a real
classic (planar, 4-colour) image for Workbench 3.0/3.1, made from its own colour
picture. Everything else in the file (type, DrawerData, tooltypes, the FORM ICON
colour data) is kept byte for byte; only the classic image(s) are replaced.

Classic palette: the OS 3.x Workbench default - 0 grey, 1 black, 2 white,
3 blue. Transparent pixels become 0 (the window background)."""
import struct, sys

WB = [(0xAA, 0xAA, 0xAA), (0x00, 0x00, 0x00), (0xFF, 0xFF, 0xFF), (0x66, 0x88, 0xBB)]


def bits(data, nbits):
    acc = n = 0
    for b in data:
        acc = (acc << 8) | b
        n += 8
        while n >= nbits:
            n -= nbits
            yield (acc >> n) & ((1 << nbits) - 1)


def unrle(data, depth, count):
    out = []
    # RLE on bit stream: control byte (8 bits) then values of `depth` bits
    src = bits(data, 1)
    def take(nb):
        v = 0
        for _ in range(nb):
            v = (v << 1) | next(src)
        return v
    while len(out) < count:
        c = take(8)
        if c > 128:
            v = take(depth)
            out += [v] * (257 - c)
        elif c < 128:
            out += [take(depth) for _ in range(c + 1)]
    return out[:count]


def read_form(d):
    i = d.find(b'FORM')
    form = d[i:]
    size = struct.unpack('>I', form[4:8])[0]
    assert form[8:12] == b'ICON'
    off, end = 12, 8 + size
    face, imags = None, []
    while off < end:
        cid, cl = form[off:off + 4], struct.unpack('>I', form[off + 4:off + 8])[0]
        body = form[off + 8:off + 8 + cl]
        if cid == b'FACE':
            face = (body[0] + 1, body[1] + 1)
        elif cid == b'IMAG':
            imags.append(body)
        off += 8 + cl + (cl & 1)
    return face, imags


def decode_imag(body, w, h, pal=None):
    trans, ncol, flags, ifmt, pfmt, depth = body[0], body[1] + 1, body[2], body[3], body[4], body[5]
    isize, psize = struct.unpack('>HH', body[6:10])
    p = body[10:]
    idata, pdata = p[:isize + 1], p[isize + 1:isize + 1 + psize + 1]
    pix = unrle(idata, depth, w * h) if ifmt else list(idata[:w * h])
    if flags & 2:
        raw = unrle(pdata, 8, ncol * 3) if pfmt else list(pdata[:ncol * 3])
        pal = [tuple(raw[k * 3:k * 3 + 3]) for k in range(ncol)]
    t = trans if flags & 1 else None
    return pix, pal, t


def nearest(c):
    """Flat 4-colour mapping in the classic Workbench style: dark -> black
    (outlines, shadows), light -> white, mid blues -> blue, mid greys ->
    the background grey. A dither looked noisy at icon size."""
    r, g, b = c
    lum = 0.3 * r + 0.59 * g + 0.11 * b
    blueish = b >= r and b >= g - 10
    if lum < 55 or (lum < 95 and not blueish) or (lum < 95 and b < 150):
        return 1
    if lum > 190:
        return 2
    return 3 if blueish else (0 if lum < 170 else 2)


OVER = {}       # palette index -> classic colour, per icon (OVERRIDES=7:3,1:0,...)


def classic(pix, pal, t, w, h):
    idx = [0 if (t is not None and v == t) else OVER.get(v, nearest(pal[v])) for v in pix]
    bpr = (w + 15) // 16 * 2
    planes = bytearray(2 * h * bpr)
    for y in range(h):
        for x in range(w):
            v = idx[y * w + x]
            for pl in range(2):
                if v >> pl & 1:
                    planes[pl * h * bpr + y * bpr + x // 8] |= 0x80 >> (x & 7)
    return idx, bytes(planes)


def image_struct(w, h, planes):
    return struct.pack('>hhHHHIBBI', 0, 0, w, h, 2, 1, 3, 0, 0) + planes


def main():
    args = [x for x in sys.argv[1:] if not x.startswith('OVERRIDES=')]
    for x in sys.argv[1:]:
        if x.startswith('OVERRIDES='):
            OVER.update({int(k): int(v) for k, v in (kv.split(':') for kv in x[10:].split(','))})
    sys.argv = [sys.argv[0]] + args
    src, dst = sys.argv[1], sys.argv[2]
    d = open(src, 'rb').read()
    g = d[4:48]
    width, height, flags = struct.unpack('>HHH', g[8:14])
    render, select = struct.unpack('>II', g[18:26])
    userdata = struct.unpack('>I', g[40:44])[0]
    typ = d[48]
    deftool, tooltypes, _, _, drawerdata, toolwin = struct.unpack('>IIIIII', d[50:74])
    off = 78
    dd = b''
    if drawerdata:
        dd = d[off:off + 56]
        off += 56
    def skip_image(o):
        w_, h_, dep = struct.unpack('>HHH', d[o + 4:o + 10])
        return o + 20 + dep * h_ * ((w_ + 15) // 16 * 2)
    if render:
        off = skip_image(off)
    if select:
        off = skip_image(off)
    rest = d[off:]                       # strings, DrawerData2, FORM ICON - kept as they are
    (fw, fh), imags = read_form(d)
    pix, pal, t = decode_imag(imags[0], fw, fh)
    idx, planes = classic(pix, pal, t, fw, fh)
    sel = None
    if len(imags) > 1:
        p2, pal2, t2 = decode_imag(imags[1], fw, fh, pal)
        _, sel = classic(p2, pal2, t2, fw, fh)
    # gadget: the image's size; highlight = the second image, or complement
    gflags = (flags & ~0x0003) | (0x0002 if sel else 0x0000)
    g2 = bytearray(g)
    struct.pack_into(">HHH", g2, 8, fw, fh, gflags)
    struct.pack_into('>II', g2, 18, 1, 1 if sel else 0)
    out = d[:4] + bytes(g2) + d[48:78] + dd + image_struct(fw, fh, planes)
    if sel:
        out += image_struct(fw, fh, sel)
    out += rest
    open(dst, 'wb').write(out)
    print('%s: classic image %dx%d, 2 planes (was %dx%d)%s' % (dst, fw, fh, width, height,
                                                                ', with a selected image' if sel else ''))
    if len(sys.argv) > 3:
        from PIL import Image
        im = Image.new('RGB', (fw, fh))
        im.putdata([WB[v] for v in idx])
        im.resize((fw * 4, fh * 4), Image.NEAREST).save(sys.argv[3])


if __name__ == '__main__':
    main()
