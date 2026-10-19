#!/usr/bin/env python3
"""
Drawer icons for PrismNet and PrismRTG, made like the Amelinium drawer:
an emblem painted onto the system's own glow drawer icon (def_drawer.info),
so they match every other drawer on the desktop.

  PrismNet : the Prism glass prism; its spectrum lands on a small network globe
  PrismRTG : the Prism glass prism splitting a white beam into a spectrum

The classic part (DiskObject, DrawerData, planar fallback) is copied from
the base icon; only the colour IMAGs (normal and selected) get the emblem.

    make_prism_drawers.py net out.info [...]     PrismNet drawer
    make_prism_drawers.py rtg out.info [...]     PrismRTG drawer
    make_prism_drawers.py net|rtg --png preview.png
"""
import math
import os
import struct
import sys

TOOLS = os.environ.get("ICONLIB_DIR", r"C:/projects/tools")
sys.path.insert(0, TOOLS)
import make_amelinium_drawer_from_base as B   # noqa: E402  (IMAG decode/encode helpers)
from rle_os35 import u32                       # noqa: E402

OUTLINE = (16, 20, 36)
WHITE = (255, 255, 255)
GLASS = [(222, 236, 250), (176, 204, 236), (130, 166, 214), (92, 128, 190)]   # light -> dark
SPECTRUM = [(236, 64, 56), (248, 156, 40), (246, 224, 64), (80, 192, 84), (64, 132, 232), (146, 88, 216)]
OCEAN = [(150, 196, 248), (98, 150, 228), (58, 104, 190), (34, 66, 140)]
LAND = (88, 178, 96)
GRID = (206, 226, 255)


def _in_tri(x, y, a, b, c):
    def s(p, q, r):
        return (p[0] - r[0]) * (q[1] - r[1]) - (q[0] - r[0]) * (p[1] - r[1])
    d1, d2, d3 = s((x, y), a, b), s((x, y), b, c), s((x, y), c, a)
    neg = d1 < 0 or d2 < 0 or d3 < 0
    pos = d1 > 0 or d2 > 0 or d3 > 0
    return not (neg and pos)


def emblem(kind):
    """{(x, y): rgb} for the emblem, drawn back to front."""
    px = {}
    apex, bl, br = (19, 2), (9, 22), (29, 22)

    # white beam coming in from the left, hitting the left face
    for x in range(1, 15):
        for y in (14, 15):
            px[(x, y)] = WHITE

    # spectrum leaving the right face and fanning out
    x0 = 24
    x1 = 33 if kind == "net" else 44
    for x in range(x0, x1 + 1):
        t = (x - x0) / float(44 - x0)
        top, bot = 11.5 - t * 3.5, 16.5 + t * 5.5
        for y in range(int(math.floor(top)), int(math.ceil(bot)) + 1):
            f = (y - top) / max(0.01, bot - top)
            if 0.0 <= f <= 1.0:
                px[(x, y)] = SPECTRUM[min(5, int(f * 6))]

    # the glass prism: outline, shaded left (light) to right (dark), a glint
    for y in range(apex[1], bl[1] + 1):
        for x in range(bl[0], br[0] + 1):
            if not _in_tri(x + 0.5, y + 0.5, apex, bl, br):
                continue
            edge = not (_in_tri(x - 0.5, y + 0.5, apex, bl, br) and _in_tri(x + 1.5, y + 0.5, apex, bl, br)
                        and _in_tri(x + 0.5, y - 0.5, apex, bl, br) and _in_tri(x + 0.5, y + 1.5, apex, bl, br))
            if edge or y == bl[1]:
                px[(x, y)] = OUTLINE
            else:
                f = (x - bl[0]) / float(br[0] - bl[0])
                px[(x, y)] = GLASS[min(3, int(f * 4))]
    for y in range(7, 21):                       # glint down the left face
        x = int(round(apex[0] - 1 - (y - apex[1]) * (apex[0] - bl[0]) / float(bl[1] - apex[1]))) + 2
        if px.get((x, y)) in GLASS:
            px[(x, y)] = WHITE
    for x in range(13, 26):                      # a lighter band across the glass
        if px.get((x, 14)) in GLASS[1:]:
            px[(x, 14)] = GLASS[0]

    if kind == "net":
        # a small network globe where the spectrum lands
        cx, cy, r = 38, 16, 7
        L = (-0.5, -0.6, 0.62)
        n = math.sqrt(sum(c * c for c in L))
        L = tuple(c / n for c in L)
        for y in range(cy - r, cy + r + 1):
            for x in range(cx - r, cx + r + 1):
                dx, dy = x - cx, y - cy
                d2 = dx * dx + dy * dy
                if d2 > r * r:
                    continue
                if d2 >= (r - 1) ** 2:
                    px[(x, y)] = OUTLINE
                    continue
                nz = math.sqrt(max(0.0, 1 - d2 / float(r * r)))
                lit = max(0.0, min(1.0, (dx / r) * L[0] + (dy / r) * L[1] + nz * L[2]))
                c = OCEAN[min(3, int(round((1 - lit) * 3)))]
                if dy in (-3, 0, 3) or dx in (0,) or abs(dx * dx * 4 - (r - 1) ** 2 + dy * dy * 2) < 6:
                    c = GRID
                if (dx, dy) in ((-2, -1), (-3, 1), (-2, 1), (1, 2), (2, 2), (2, -3), (3, -2)):
                    c = LAND
                px[(x, y)] = c
        px[(cx - 2, cy - 3)] = WHITE             # glint
    return px


def _composite(kind, pix, pal, w, h):
    pal = list(pal)

    def gi(rgb):
        if rgb in pal:
            return pal.index(rgb)
        pal.append(rgb)
        return len(pal) - 1

    em = emblem(kind)
    black = gi((0, 0, 0))
    for (x, y) in list(em):                       # dark halo: separates it from the drawer
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                q = (x + dx, y + dy)
                if q not in em and 0 <= q[0] < w and 0 <= q[1] < h:
                    pix[q[1] * w + q[0]] = black
    for (x, y), rgb in em.items():
        if 0 <= x < w and 0 <= y < h:
            pix[y * w + x] = gi(rgb)
    return pix, pal


def build(kind):
    data = open(B.BASE, "rb").read()
    p = data.find(b"FORM")
    classic = data[:p]
    w = data[p + 20] + 1
    h = data[p + 21] + 1
    o, end = p + 12, p + 8 + u32(data, p + 4)
    imags = []
    while o < end:
        cid, clen = data[o:o + 4], u32(data, o + 4)
        if cid == b"IMAG":
            imags.append(data[o + 8:o + 8 + clen])
        o += 8 + clen + (clen & 1)
    out, maxpal = [], 0
    for body in imags:
        tc, pix, pal = B._decode_imag(body, w, h)
        pix, pal = _composite(kind, pix, pal, w, h)
        enc, pl = B._encode_imag(tc, pix, pal)
        out.append(enc)
        maxpal = max(maxpal, pl)
    form = b"ICON" + B._iff_chunk(b"FACE", struct.pack(">BBBBH", w - 1, h - 1, 0, 0, maxpal - 1))
    for enc in out:
        form += B._iff_chunk(b"IMAG", enc)
    return classic + b"FORM" + struct.pack(">I", len(form)) + form


def write_png(kind, path, scale=6):
    import zlib
    data = open(B.BASE, "rb").read()
    p = data.find(b"FORM")
    w, h = data[p + 20] + 1, data[p + 21] + 1
    o = p + 12
    while data[o:o + 4] != b"IMAG":
        o += 8 + u32(data, o + 4) + (u32(data, o + 4) & 1)
    tc, pix, pal = B._decode_imag(data[o + 8:o + 8 + u32(data, o + 4)], w, h)
    pix, pal = _composite(kind, pix, pal, w, h)
    raw = bytearray()
    for y in range(h * scale):
        raw.append(0)
        for x in range(w * scale):
            i = pix[(y // scale) * w + x // scale]
            raw += bytes((176, 176, 176) if i == tc else pal[i])

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w * scale, h * scale, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b"")
    open(path, "wb").write(png)
    print("wrote", path)


def main():
    a = sys.argv[1:]
    if len(a) < 2 or a[0] not in ("net", "rtg"):
        print(__doc__)
        return 1
    if a[1] == "--png":
        write_png(a[0], a[2])
        return 0
    data = build(a[0])
    for path in a[1:]:
        open(path, "wb").write(data)
        print("wrote", path, "(%d bytes)" % len(data))
    return 0


if __name__ == "__main__":
    sys.exit(main())
