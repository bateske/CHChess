"""Render the chess pieces in the iso view (starting points for the art).

    python tools/pieces.py [--scale 8]    -> tools/art/gen/*.png + build/assets/pieces_sheet.png

Staunton pieces are modelled as signed-distance shapes - lathe profiles for
pawn, rook, bishop, queen and king, an extruded rounded profile for the
knight's head - and ray-marched at the camera's elevation. The board's 2:1
tiles mean the camera looks down at 30 degrees (a circle becomes an ellipse
half as tall as wide), so the pieces sit on the tiles the way the tiles
imply. Shading is quantised to three body tones, a specular glint and gold
trim, then outlined in ink.

These renders are a starting point: the palette-exact PNGs in tools/art/ are
the source of truth, and may be touched up by hand after copying from
tools/art/gen/. Colours are the WHITE set; the game remaps them for Black.
"""
import argparse
import math
from pathlib import Path

import numpy as np
from PIL import Image

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
GEN = HERE / "art" / "gen"
PREVIEW = ROOT / "build" / "assets"

# Palette (src/gfx/Palette.cpp), RGB444.
PALETTE = [0x000, 0xFFF, 0x042, 0x173, 0x4B5, 0xBBC, 0xE12, 0x702,
           0xFC2, 0x741, 0x26E, 0x125, 0xFB8, 0x6EF, 0xF0F, 0xFC2]
INK, WHITE, SILVER, BLUE, CYAN, GOLD, WOOD = 0, 1, 5, 10, 13, 8, 9


def rgb(i):
    c = PALETTE[i]
    return ((c >> 8) * 17, ((c >> 4) & 15) * 17, (c & 15) * 17)


NAVY = 11
# Body tones, dark to light, then the glint. The art stores four neutral
# tones so one sprite serves both sides through a remap:
#   (each side's colours for them: tools/art/sides.txt)
BODY = [BLUE, NAVY, SILVER, WHITE, CYAN]
TRIM = [WOOD, WOOD, GOLD, GOLD, WHITE]
THRESH = [0.38, 0.62, 0.90]

ELEV = math.radians(30.0)
D = np.array([0.0, math.cos(ELEV), -math.sin(ELEV)])     # view direction (into the screen)
R_AX = np.array([1.0, 0.0, 0.0])                          # screen right
U_AX = np.array([0.0, math.sin(ELEV), math.cos(ELEV)])    # screen up
LIGHT = np.array([-0.62, -0.55, 0.78]); LIGHT /= np.linalg.norm(LIGHT)
VIEW = -D
HALF = LIGHT + VIEW; HALF /= np.linalg.norm(HALF)


# ---------------------------------------------------------------------------
# Shapes: f(p) < 0 inside. p is (N, 3). Each returns (field, material 0/1).
# ---------------------------------------------------------------------------
def lathe(profile):
    """profile: [(z, r), ...] ascending z, linear between points."""
    zs = np.array([p[0] for p in profile], float)
    rs = np.array([p[1] for p in profile], float)

    def f(p):
        rho = np.hypot(p[:, 0], p[:, 1])
        z = p[:, 2]
        r = np.interp(z, zs, rs, left=-1.0, right=-1.0)
        d = rho - r
        # below the base / above the top: distance to the caps
        d = np.where(z < zs[0], np.maximum(d, zs[0] - z), d)
        d = np.where(z > zs[-1], np.maximum(d, z - zs[-1]), d)
        return d
    return f


def ellipsoid(cx, cy, cz, rx, ry, rz):
    def f(p):
        q = np.stack([(p[:, 0] - cx) / rx, (p[:, 1] - cy) / ry, (p[:, 2] - cz) / rz], 1)
        k = np.linalg.norm(q, axis=1)
        return (k - 1.0) * min(rx, ry, rz)
    return f


def sphere(cx, cy, cz, r):
    return ellipsoid(cx, cy, cz, r, r, r)


def box(cx, cy, cz, hx, hy, hz, round_=0.0):
    def f(p):
        q = np.abs(p - np.array([cx, cy, cz])) - np.array([hx, hy, hz]) + round_
        return np.linalg.norm(np.maximum(q, 0), axis=1) + np.minimum(q.max(axis=1), 0) - round_
    return f


def poly_sdf(pts, x, z):
    """Signed distance to a closed 2D polygon (negative inside)."""
    pts = np.asarray(pts, float)
    d = np.full(x.shape, 1e9)
    s = np.ones(x.shape)
    n = len(pts)
    for i in range(n):
        ax, az = pts[i]; bx, bz = pts[(i + 1) % n]
        ex, ez = bx - ax, bz - az
        wx, wz = x - ax, z - az
        t = np.clip((wx * ex + wz * ez) / (ex * ex + ez * ez), 0, 1)
        dx, dz = wx - ex * t, wz - ez * t
        d = np.minimum(d, dx * dx + dz * dz)
        c1 = z >= az; c2 = z < bz; c3 = ex * wz > ez * wx
        flip = (c1 & c2 & c3) | (~c1 & ~c2 & ~c3)
        s = np.where(flip, -s, s)
    return s * np.sqrt(d)


def extrude(pts, half, round_, yaw=0.0, ox=0.0):
    """Polygon in (x, z) extruded along y by +-half, edges rounded; turned
    by yaw (radians) about the z axis."""
    cs, sn = math.cos(yaw), math.sin(yaw)

    def f(p):
        x = cs * p[:, 0] + sn * p[:, 1] - ox
        y = -sn * p[:, 0] + cs * p[:, 1]
        d2 = poly_sdf(pts, x, p[:, 2]) + round_
        dy = np.abs(y) - half + round_
        q = np.stack([d2, dy], 1)
        return np.linalg.norm(np.maximum(q, 0), axis=1) + np.minimum(q.max(axis=1), 0) - round_
    return f


def union(*fs):
    def f(p):
        return np.minimum.reduce([g(p) for g in fs])
    return f


def subtract(a, b):
    def f(p):
        return np.maximum(a(p), -b(p))
    return f


def smooth_union(a, b, k):
    def f(p):
        da, db = a(p), b(p)
        h = np.clip(0.5 + 0.5 * (db - da) / k, 0, 1)
        return db * (1 - h) + da * h - k * h * (1 - h)
    return f


# ---------------------------------------------------------------------------
# The pieces (units: pixels; base on z = 0). Gold is chosen by a material
# function of position.
# ---------------------------------------------------------------------------
def base_profile(R):
    return [(0, R), (1.6, R), (2.6, R - 0.6), (3.4, R - 1.6), (4.2, R - 1.9), (5.0, R - 2.6)]


def pawn():
    R = 10.0
    prof = base_profile(R) + [(5.8, 7.2), (6.8, 7.4), (7.8, 6.2), (8.6, 5.0), (13.0, 3.8),
                              (13.6, 6.0), (14.8, 6.2), (15.6, 4.2), (16.4, 3.6)]
    shape = union(lathe(prof), sphere(0, 0, 21.6, 7.0))      # a big head: it reads at 13 px
    gold = lambda p: (p[:, 2] > 14.4) & (p[:, 2] < 16.8)
    return shape, gold, 28


def rook():
    R = 11.0
    prof = base_profile(R) + [(5.8, 8.2), (7.0, 8.4), (7.8, 7.4), (21.0, 6.6), (22.0, 7.8),
                              (23.0, 8.4), (24.2, 8.6), (27.2, 8.6), (27.2, 5.2), (26.6, 5.2)]
    body = lathe(prof)
    # Four merlons on the rim, turned 45 degrees so the 3/4 view shows three.
    merlons = []
    for k in range(4):
        a = math.pi / 4 + k * math.pi / 2
        cs, sn = math.cos(a), math.sin(a)

        def m(p, cs=cs, sn=sn):
            x = cs * p[:, 0] + sn * p[:, 1]
            y = -sn * p[:, 0] + cs * p[:, 1]
            q = np.stack([np.abs(x - 7.0) - 1.9, np.abs(y) - 3.0, np.abs(p[:, 2] - 28.9) - 1.9], 1)
            return np.linalg.norm(np.maximum(q, 0), axis=1) + np.minimum(q.max(axis=1), 0) - 0.3
        merlons.append(m)
    shape = union(body, *merlons)
    gold = lambda p: (p[:, 2] > 21.4) & (p[:, 2] < 24.0)
    return shape, gold, 32


def bishop():
    R = 10.8
    prof = base_profile(R) + [(5.8, 7.6), (6.8, 7.8), (7.6, 6.6), (8.6, 5.4), (21.0, 3.6),
                              (21.6, 6.2), (22.8, 6.4), (23.6, 4.2), (24.4, 3.6)]
    mitre = ellipsoid(0, 0, 30.2, 5.6, 5.6, 7.6)
    # The mitre's slit: a thin slab cut diagonally across the front.

    def slit(p):
        n = np.array([0.70, -0.45, 0.55]); n /= np.linalg.norm(n)
        d = p @ n - (np.array([0, 0, 31.8]) @ n)
        return np.maximum(np.abs(d) - 1.2, np.maximum(-p[:, 0] - 0.5, p[:, 2] - 36.8))
    shape = union(lathe(prof), subtract(mitre, slit), sphere(0, 0, 38.8, 2.0))
    gold = lambda p: ((p[:, 2] > 21.2) & (p[:, 2] < 23.8)) | (p[:, 2] > 36.9)
    return shape, gold, 42


def queen():
    R = 11.4
    prof = base_profile(R) + [(5.8, 8.2), (6.8, 8.4), (7.8, 7.0), (9.0, 5.6), (24.0, 3.8),
                              (24.6, 6.6), (25.8, 6.8), (26.6, 4.6), (27.4, 4.2),
                              (33.2, 7.4), (34.4, 7.0), (35.2, 5.2), (37.0, 3.0), (38.4, 1.0)]
    body = lathe(prof)
    points = [sphere(7.0 * math.cos(a), 7.0 * math.sin(a), 35.0, 1.8)
              for a in [i * 2 * math.pi / 8 + math.pi / 8 for i in range(8)]]
    shape = union(body, *points, sphere(0, 0, 39.6, 2.3))
    gold = lambda p: ((p[:, 2] > 24.2) & (p[:, 2] < 26.8)) | (p[:, 2] > 32.4)
    return shape, gold, 44


def king():
    R = 11.6
    prof = base_profile(R) + [(5.8, 8.4), (6.8, 8.6), (7.8, 7.2), (9.0, 5.8), (25.0, 4.0),
                              (25.6, 6.8), (26.8, 7.0), (27.6, 4.8), (28.4, 4.4),
                              (34.4, 7.4), (35.6, 7.6), (36.4, 6.2), (38.4, 3.4), (39.0, 2.0)]
    body = lathe(prof)
    cross = union(box(0, 0, 42.6, 1.25, 1.25, 4.4, 0.5), box(0, 0, 43.8, 3.6, 1.25, 1.2, 0.5))
    shape = union(body, cross)
    gold = lambda p: ((p[:, 2] > 25.2) & (p[:, 2] < 27.8)) | (p[:, 2] > 37.4)
    return shape, gold, 48


# Knight head profile, facing +x (screen right), in (x, z).
KNIGHT_HEAD = [
    (-6.6, 6.5), (-7.4, 12.0), (-7.2, 18.0), (-5.6, 24.0), (-3.4, 29.0), (-1.2, 32.6),
    (0.2, 36.4), (1.4, 33.8), (2.8, 35.2), (3.6, 32.2), (6.0, 30.2), (8.6, 27.0),
    (10.6, 23.6), (11.2, 21.4), (10.2, 19.6), (7.6, 19.8), (5.2, 19.2), (3.4, 17.2),
    (3.8, 13.0), (5.6, 9.6), (6.2, 6.5)]


def knight():
    R = 10.8
    prof = base_profile(R) + [(5.8, 7.8), (6.8, 8.0), (7.6, 6.8), (8.4, 5.8)]
    head = extrude(KNIGHT_HEAD, 3.4, 1.6, yaw=math.radians(-24), ox=0.4)
    shape = smooth_union(lathe(prof), head, 1.2)
    gold = lambda p: (p[:, 2] < 8.6) & (p[:, 2] > 5.6)
    return shape, gold, 38


PIECES = {"pawn": pawn, "knight": knight, "bishop": bishop, "rook": rook, "queen": queen, "king": king}


# ---------------------------------------------------------------------------
# Rendering
# ---------------------------------------------------------------------------
def render(shape, gold, height, size=1.0, ss=8):
    """size: screen pixels per model unit (the models are built for 56 px tiles)."""
    rmax = int(math.ceil(12 * size))
    w = 2 * rmax + 3
    top = int(math.ceil((height * math.cos(ELEV) + 6) * size)) + 3
    bot = int(math.ceil(6 * size)) + 2
    h = top + bot
    # Sub-pixel sample grid (centres), screen coords relative to the anchor,
    # in model units.
    xs = ((np.arange(w * ss) + 0.5) / ss - w / 2) / size
    ys = ((np.arange(h * ss) + 0.5) / ss - top) / size          # down positive
    X, Y = np.meshgrid(xs, ys)
    X = X.ravel(); Y = Y.ravel()
    origin = X[:, None] * R_AX[None, :] - Y[:, None] * U_AX[None, :] - D[None, :] * 80.0
    t = np.zeros(len(X))
    hit = np.zeros(len(X), bool)
    alive = np.ones(len(X), bool)
    prev = np.zeros(len(X))
    for _ in range(600):
        idx = np.nonzero(alive)[0]
        if not len(idx):
            break
        p = origin[idx] + D[None, :] * t[idx, None]
        d = shape(p)
        inside = d < 0.0
        hit[idx[inside]] = True
        alive[idx[inside]] = False
        out = idx[~inside]
        prev[out] = t[out]
        t[out] += np.maximum(0.06, 0.5 * d[~inside])
        gone = t > 160
        alive &= ~gone
    # Refine each hit between the last outside and the first inside point.
    hi_t = t.copy(); lo_t = prev.copy()
    hidx = np.nonzero(hit)[0]
    for _ in range(12):
        mid = 0.5 * (lo_t[hidx] + hi_t[hidx])
        d = shape(origin[hidx] + D[None, :] * mid[:, None])
        ins = d < 0
        hi_t[hidx[ins]] = mid[ins]
        lo_t[hidx[~ins]] = mid[~ins]
    t = hi_t
    # Normals by central differences.
    p = origin + D[None, :] * t[:, None]
    e = 0.45
    n = np.zeros_like(p)
    for k in range(3):
        dv = np.zeros(3); dv[k] = e
        n[:, k] = shape(p + dv) - shape(p - dv)
    n /= np.linalg.norm(n, axis=1, keepdims=True) + 1e-9
    diff = np.clip(n @ LIGHT, 0, 1)
    rim = np.clip(1.0 - (n @ VIEW), 0, 1) ** 3
    spec = np.clip(n @ HALF, 0, 1) ** 28
    inten = 0.18 + 0.82 * diff + 0.18 * rim
    is_gold = gold(p)
    # Down-sample: coverage by majority, shading averaged over hits.
    def pool(a):
        return a.reshape(h, ss, w, ss).mean(axis=(1, 3))
    cov = pool(hit.astype(float))
    ih = pool(np.where(hit, inten, 0)) / np.maximum(cov, 1e-6)
    sh = pool(np.where(hit, spec, 0)) / np.maximum(cov, 1e-6)
    gh = pool(np.where(hit & is_gold, 1.0, 0)) / np.maximum(cov, 1e-6)
    img = np.full((h, w), -1, int)
    for y in range(h):
        for x in range(w):
            if cov[y, x] < 0.5:
                continue
            tones = TRIM if gh[y, x] >= 0.5 else BODY
            i = ih[y, x]
            k = sum(i >= th for th in THRESH)
            c = tones[k]
            if sh[y, x] > 0.72:
                c = tones[4]
            img[y, x] = c
    # Despeckle: a lone pixel between two of the same body colour takes
    # theirs (cleaner bands, and fewer runs for the RLE packer).
    for _ in range(2 if size >= 1 else 1):
        for y in range(h):
            for x in range(1, w - 1):
                a, b, c = img[y, x - 1], img[y, x], img[y, x + 1]
                if a >= 0 and a == c and b != a and b not in (INK,):
                    img[y, x] = a
        for y in range(1, h - 1):
            for x in range(w):
                a, b, c = img[y - 1, x], img[y, x], img[y + 1, x]
                if a >= 0 and a == c and b != a and b not in (INK,) and b not in TRIM:
                    img[y, x] = a
    # Ink outline around the silhouette (4-neighbour).
    out = img.copy()
    for y in range(h):
        for x in range(w):
            if img[y, x] >= 0:
                continue
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                xx, yy = x + dx, y + dy
                if 0 <= xx < w and 0 <= yy < h and img[yy, xx] >= 0:
                    out[y, x] = INK
                    break
    return crop(out, top)


def crop(img, top):
    ys, xs = np.nonzero(img >= 0)
    y0, y1, x0, x1 = ys.min(), ys.max() + 1, xs.min(), xs.max() + 1
    anchor = (img.shape[1] // 2 - x0, top - y0)      # base centre within the crop
    return img[y0:y1, x0:x1], anchor


def to_png(img):
    h, w = img.shape
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    for y in range(h):
        for x in range(w):
            if img[y, x] >= 0:
                im.putpixel((x, y), rgb(img[y, x]) + (255,))
    return im


# Each side's colours: tools/art/sides.txt (read through assets.py).
from assets import load_sides  # noqa: E402
_SIDES = load_sides()
WHITE_REMAP = {a: c for a, c in enumerate(_SIDES[0]) if a != c}
BLACK_REMAP = {a: c for a, c in enumerate(_SIDES[1]) if a != c}


def remap(img, table):
    out = img.copy()
    for a, b in table.items():
        out[img == a] = b
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--size", type=float, default=0.5, help="pixels per model unit (1.0 = 56 px tiles)")
    ap.add_argument("--scale", type=int, default=8, help="preview magnification")
    a = ap.parse_args()
    GEN.mkdir(parents=True, exist_ok=True)
    PREVIEW.mkdir(parents=True, exist_ok=True)
    tiles = []
    for name, fn in PIECES.items():
        shape, gold, height = fn()
        img, anchor = render(shape, gold, height, a.size)
        to_png(img).save(GEN / f"{name}.png")
        (GEN / f"{name}.anchor").write_text(f"{anchor[0]} {anchor[1]}\n")
        print(f"{name}: {img.shape[1]}x{img.shape[0]} anchor {anchor}")
        tiles.append((remap(img, WHITE_REMAP), remap(img, BLACK_REMAP)))
    # Preview sheet: white row over a light square, black row over a dark one.
    pad = 4
    W = sum(t[0].shape[1] + pad for t in tiles) + pad
    H = max(t[0].shape[0] for t in tiles) * 2 + pad * 3
    sheet = Image.new("RGB", (W, H), rgb(3))
    x = pad
    for white, black in tiles:
        for row, im in enumerate((white, black)):
            y = pad + row * (H // 2)
            sheet.paste(to_png(im), (x, y), to_png(im))
        x += white.shape[1] + pad
    sheet = sheet.resize((W * a.scale, H * a.scale), Image.NEAREST)
    sheet.save(PREVIEW / "pieces_sheet.png")
    print(f"sheet: {PREVIEW / 'pieces_sheet.png'}")


if __name__ == "__main__":
    main()
