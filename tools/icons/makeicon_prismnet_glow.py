#!/usr/bin/env python3
"""
GlowIcons-style .info icons for the PrismNet GUIs (original artwork, GPL-3.0).

  PrismNet Prefs : a glass prism splits a white beam into a rainbow that lands
                   on a blue network globe; a small gear says "preferences".
  PrismMon       : a monitor showing a live traffic graph, with a prism badge.

Uses the GlowIcons pipeline from C:/projects/tools/makeicon_glow.py (4x
supersampled drawing, anti-aliased downsample, drop shadow, gold glow on the
selected image, dual planar + OS3.5 colour icon via iconlib).

    makeicon_prismnet_glow.py <outdir> [--png <previewdir>]
"""
import math
import os
import sys

sys.path.insert(0, os.environ.get("ICONLIB_DIR", r"C:/projects/tools"))
import iconlib            # noqa: E402
import makeicon_glow as G  # noqa: E402

W, H = G.W, G.H
hrect, hvgrad, hframe, hdisc, hline = G.hrect, G.hvgrad, G.hframe, G.hdisc, G.hline
lt, dk, lerp, V, hset = G.lt, G.dk, G.lerp, G.V, G.hset

WHITE = (252, 252, 252)
GLASS0, GLASS1 = (232, 246, 255), (130, 188, 230)
OCEAN0, OCEAN1 = (120, 186, 250), (30, 84, 176)
LAND = (96, 186, 104)
MET0, MET1 = (226, 230, 238), (118, 122, 134)
SCREEN0, SCREEN1 = (28, 40, 70), (12, 18, 36)
RAINBOW = [(236, 72, 64), (248, 168, 52), (246, 226, 70), (84, 196, 90), (70, 140, 236), (150, 96, 220)]


def htri(cv, p0, p1, p2, shade):
    """Filled triangle (virtual coords); shade(vx, vy) -> rgb."""
    xs = [p0[0], p1[0], p2[0]]
    ys = [p0[1], p1[1], p2[1]]

    def side(a, b, x, y):
        return (b[0] - a[0]) * (y - a[1]) - (b[1] - a[1]) * (x - a[0])

    for hy in range(V(min(ys)), V(max(ys)) + 1):
        for hx in range(V(min(xs)), V(max(xs)) + 1):
            x, y = hx / G.SS, hy / G.SS
            s0, s1, s2 = side(p0, p1, x, y), side(p1, p2, x, y), side(p2, p0, x, y)
            if (s0 >= 0 and s1 >= 0 and s2 >= 0) or (s0 <= 0 and s1 <= 0 and s2 <= 0):
                hset(cv, hx, hy, shade(x, y))


def prism(cv, ax, ay, size, sel, spread=0.62):
    """Glass prism with apex at (ax, ay)."""
    p0, p1, p2 = (ax, ay), (ax - size * spread, ay + size), (ax + size * spread, ay + size)

    def glass(x, y):
        t = min(1.0, max(0.0, (x - p1[0]) / (p2[0] - p1[0])))
        c = lerp(GLASS0, GLASS1, t)
        return lt(c, 0.25) if sel else c

    htri(cv, p0, p1, p2, glass)
    edge = dk(GLASS1, 0.6)
    hline(cv, p0[0], p0[1], p1[0], p1[1], edge, 1)
    hline(cv, p1[0], p1[1], p2[0], p2[1], edge, 1)
    hline(cv, p0[0], p0[1], p2[0], p2[1], lt(GLASS0, 0.6), 1)   # lit edge
    return p0, p1, p2


def globe(cv, cx, cy, r, sel):
    light = (-0.45, -0.55)

    def shade(dx, dy, d):
        n = 1.0 - d / r
        lam = max(0.0, 0.55 + 0.45 * (-(dx * light[0] + dy * light[1]) / max(r, 0.01)))
        c = lerp(OCEAN1, OCEAN0, min(1.0, lam + n * 0.3))
        # simple continents: two blobs
        for (bx, by, br) in ((-0.30, -0.20, 0.38), (0.35, 0.30, 0.30)):
            if (dx / r - bx) ** 2 + (dy / r - by) ** 2 < br ** 2:
                c = lerp(dk(LAND, 0.7), LAND, lam)
        return lt(c, 0.18) if sel else c

    hdisc(cv, cx, cy, r, shade)
    grid = lerp(OCEAN0, lt(OCEAN0, 0.45), 0.5)
    for k in (-0.45, 0.25):                          # latitudes
        yy = cy + k * r
        half = math.sqrt(max(0.0, r * r - (k * r) ** 2))
        hline(cv, cx - half + 0.5, yy, cx + half - 0.5, yy, grid, 1)
    hline(cv, cx, cy - r + 0.5, cx, cy + r - 0.5, grid, 1)   # meridian
    hrect(cv, cx - r * 0.5, cy - r * 0.62, cx - r * 0.3, cy - r * 0.48, lt(WHITE, 0.0))  # glint


def gear(cv, cx, cy, r, col):
    def shade(dx, dy, d):
        return lerp(col, dk(col, 0.62), (dy / r + 1) / 2)

    for i in range(8):                               # teeth
        a = i * math.pi / 4
        tx, ty = cx + math.cos(a) * r, cy + math.sin(a) * r
        hrect(cv, tx - 1, ty - 1, tx + 1, ty + 1, dk(col, 0.8))
    hdisc(cv, cx, cy, r * 0.82, shade)
    hdisc(cv, cx, cy, r * 0.32, lambda dx, dy, d: (60, 62, 72))


# ------------------------------------------------------------------ glyphs
def prismprefs(cv, sel):
    globe(cv, 36, 18, 10, sel)
    # white beam in from the left, into the prism
    hline(cv, 0, 24, 8, 21, dk(WHITE, 0.8), 3)
    hline(cv, 0, 24, 8, 21, WHITE, 2)
    prism(cv, 11, 6, 20, sel, spread=0.5)
    # rainbow fan from the prism's right face onto the globe
    for i, c in enumerate(RAINBOW):
        col = lt(c, 0.25) if sel else c
        hline(cv, 16 + i * 0.5, 16 + i * 1.4, 27, 11 + i * 2.6, col, 1.2)
    gear(cv, 40, 32, 5, (238, 196, 84) if sel else (214, 172, 66))


def prismmon(cv, sel):
    x0, y0, x1, y1 = 5, 4, 42, 29
    G.soft_box(cv, x0, y0, x1, y1, MET0, MET1)               # monitor case
    sx0, sy0, sx1, sy1 = x0 + 3, y0 + 3, x1 - 3, y1 - 5
    hvgrad(cv, sx0, sy0, sx1, sy1, SCREEN0, SCREEN1)          # screen
    hframe(cv, sx0, sy0, sx1, sy1, dk(MET1, 0.6), 1)
    # grid lines
    for gy in (sy0 + 5, sy0 + 10):
        hrect(cv, sx0 + 1, gy, sx1 - 1, gy, (40, 58, 96))
    # traffic: rx area (green) + tx line (amber)
    pts_rx = [3, 6, 5, 9, 7, 12, 10, 8, 13, 11, 14, 9] if not sel else [5, 9, 8, 12, 11, 15, 13, 12, 15, 14, 16, 13]
    n = len(pts_rx)
    step = (sx1 - sx0 - 2) / (n - 1)
    base = sy1 - 1
    for i in range(n - 1):
        xa, xb = sx0 + 1 + i * step, sx0 + 1 + (i + 1) * step
        ya, yb = base - pts_rx[i], base - pts_rx[i + 1]
        for k in range(int(V(xb) - V(xa)) + 1):
            t = k / max(1, V(xb) - V(xa))
            hx = V(xa) + k
            top = ya + (yb - ya) * t
            for hy in range(int(V(top)), V(base) + 1):
                hset(cv, hx, hy, (50, 150, 70) if not sel else (70, 190, 90))
        hline(cv, xa, ya, xb, yb, (150, 240, 150), 1)
    tx = [2, 3, 2, 4, 3, 5, 4, 3, 5, 4, 6, 5]
    for i in range(n - 1):
        xa, xb = sx0 + 1 + i * step, sx0 + 1 + (i + 1) * step
        hline(cv, xa, base - tx[i] - 1, xb, base - tx[i + 1] - 1, (250, 200, 80), 1)
    # front panel: power LED + prism badge
    led = (90, 230, 100) if sel else (60, 170, 70)
    hrect(cv, x1 - 6, y1 - 3, x1 - 5, y1 - 2, led)
    prism(cv, x0 + 6, y1 - 4, 3.2, sel)
    # stand
    hvgrad(cv, 19, y1 + 1, 28, y1 + 3, MET1, dk(MET1, 0.8))
    G.soft_box(cv, 13, y1 + 4, 34, y1 + 6, MET0, MET1)


GLYPHS = {"PrismNetPrefs": prismprefs, "PrismMon": prismmon}


def render(fn, sel):
    cv = G.hcanvas()
    fn(cv, sel)
    return G.compose(G.downsample(cv), glow=sel)


def shared_palette(rn, tn, rs, ts, colours=160):
    """Median-cut both states into ONE palette (index 0 = transparent BG).
    Gradients + rainbow + gold glow exceed 255 colours with plain rounding."""
    from PIL import Image
    pix = [tuple(int(round(c)) for c in rgb[y][x])
           for rgb, tr in ((rn, tn), (rs, ts)) for y in range(H) for x in range(W) if not tr[y][x]]
    strip = Image.new("RGB", (len(pix), 1))
    strip.putdata(pix)
    q = strip.quantize(colors=colours, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    flat = q.getpalette()[:colours * 3]
    pal = [G.BG] + [tuple(flat[i:i + 3]) for i in range(0, len(flat), 3)]
    data = list(q.get_flattened_data()) if hasattr(q, 'get_flattened_data') else list(q.getdata())
    out, k = [], 0
    for rgb, tr in ((rn, tn), (rs, ts)):
        grid = [[0] * W for _ in range(H)]
        for y in range(H):
            for x in range(W):
                if not tr[y][x]:
                    grid[y][x] = data[k] + 1
                    k += 1
        out.append(grid)
    return out[0], out[1], pal


def emit(name, fn, outdir, pngdir):
    rn, tn = render(fn, False)
    rs, ts = render(fn, True)
    cn, cs, pal = shared_palette(rn, tn, rs, ts)
    info = iconlib.build_info(cn, pal, G.planar_of(pal), W, H, 0, cidx_sel=cs)
    out = os.path.join(outdir, name + ".info")
    with open(out, "wb") as f:
        f.write(info)
    print("wrote %s (%d bytes, %d colours)" % (out, len(info), len(pal)))
    if pngdir:
        iconlib.write_png(os.path.join(pngdir, name + ".png"), cn, pal, W, H, 0)
        iconlib.write_png(os.path.join(pngdir, name + "-selected.png"), cs, pal, W, H, 0)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return
    outdir = sys.argv[1]
    pngdir = sys.argv[3] if len(sys.argv) > 3 and sys.argv[2] == "--png" else None
    for name, fn in GLYPHS.items():
        emit(name, fn, outdir, pngdir)


if __name__ == "__main__":
    main()
