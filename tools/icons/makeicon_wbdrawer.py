#!/usr/bin/env python3
"""
PrismNet drawer icon in the classic Workbench style: a grey metal cabinet with
one drawer pulled out and a handle, bevelled with white highlights and black
shadows. Original artwork (GPL-3.0), so it can ship in the release archive.

    makeicon_wbdrawer.py C.info Libs.info ...   # write WBDRAWER icon(s)
    makeicon_wbdrawer.py --png preview.png      # colour preview

Uses C:/projects/tools/iconlib.py (planar 4-colour image + OS 3.5 colour form).
"""
import os
import sys

sys.path.insert(0, os.environ.get("ICONLIB_DIR", r"C:/projects/tools"))
import iconlib  # noqa: E402

W, H = 46, 30

PALETTE = [
    (149, 149, 149),  # 0 transparent / WB background
    (0, 0, 0),        # 1 black outline / shadow
    (255, 255, 255),  # 2 white highlight
    (186, 186, 196),  # 3 cabinet face, light
    (150, 152, 166),  # 4 cabinet face, mid
    (108, 110, 128),  # 5 cabinet face, dark / side
    (102, 136, 187),  # 6 blue label / accent
    (60, 64, 80),     # 7 handle slot
]
TRANSPARENT = 0
#              0  1  2  3  4  5  6  7
PLANAR_MAP = [0, 1, 2, 2, 0, 1, 3, 1]   # -> grey / black / white / blue


def new():
    return [[TRANSPARENT] * W for _ in range(H)]


def put(g, x, y, c):
    if 0 <= x < W and 0 <= y < H:
        g[y][x] = c


def hline(g, x0, x1, y, c):
    for x in range(x0, x1 + 1):
        put(g, x, y, c)


def vline(g, x, y0, y1, c):
    for y in range(y0, y1 + 1):
        put(g, x, y, c)


def rect(g, x0, y0, x1, y1, c):
    for y in range(y0, y1 + 1):
        hline(g, x0, x1, y, c)


def bevel(g, x0, y0, x1, y1, face):
    """Filled box: white top/left, black bottom/right (Workbench 3D look)."""
    rect(g, x0, y0, x1, y1, face)
    hline(g, x0, x1, y0, 2)
    vline(g, x0, y0, y1, 2)
    hline(g, x0, x1, y1, 1)
    vline(g, x1, y0, y1, 1)


def draw(g):
    # cabinet: top in perspective, right side, front
    D = 4
    CX0, CY0, CX1, CY1 = 4, 7, 37, 27
    for i in range(D + 1):                       # top
        hline(g, CX0 + i, CX1 + i, CY0 - i, 3)
        put(g, CX0 + i, CY0 - i, 2)
    hline(g, CX0 + D, CX1 + D, CY0 - D, 2)
    for i in range(D + 1):                       # right side
        vline(g, CX1 + i, CY0 - i, CY1 - i, 5)
        put(g, CX1 + i, CY1 - i, 1)
    vline(g, CX1 + D, CY0 - D, CY1 - D, 1)
    bevel(g, CX0, CY0, CX1, CY1, 4)              # front

    # lower drawer (closed): groove + handle
    hline(g, CX0 + 2, CX1 - 2, 21, 5)
    hline(g, CX0 + 2, CX1 - 2, 22, 2)
    rect(g, 17, 24, 24, 25, 7)
    hline(g, 17, 24, 26, 2)

    # upper drawer, pulled out towards the viewer (down-left)
    PX0, PY0, PX1, PY1 = 1, 10, 34, 19
    rect(g, PX0 + 2, PY0 - 2, PX1 + 2, PY0 - 1, 5)       # visible drawer box top
    hline(g, PX0 + 2, PX1 + 2, PY0 - 2, 1)
    bevel(g, PX0, PY0, PX1, PY1, 3)
    rect(g, PX0 + 4, PY0 + 2, PX0 + 13, PY0 + 4, 6)       # blue label card
    hline(g, PX0 + 4, PX0 + 13, PY0 + 5, 2)
    rect(g, 19, PY0 + 5, 27, PY0 + 6, 7)                  # handle
    hline(g, 19, 27, PY0 + 7, 2)


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return
    g = new()
    draw(g)
    if args[0] == "--png":
        iconlib.write_png(args[1], g, PALETTE, W, H, TRANSPARENT)
        print("wrote", args[1])
        return
    data = iconlib.build_info(g, PALETTE, PLANAR_MAP, W, H, TRANSPARENT, drawer=True)
    for path in args:
        with open(path, "wb") as f:
            f.write(data)
        print("wrote", path, "(%d bytes, WBDRAWER)" % len(data))


if __name__ == "__main__":
    main()
