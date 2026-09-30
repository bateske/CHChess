"""The piece art as one sprite sheet to edit (Photoshop or anything that
keeps an indexed PNG's colour table), and back.

    python tools/sheet.py export [SHEET]      # write tools/art/sheet.png (+ sheet_preview.png, 4x)
    python tools/sheet.py import [SHEET]      # read it back, then rebuild the game's assets

The sheet is an indexed PNG on the game's 16-colour palette, index 16
transparent. Paint only with the colour table's colours.

  MASTER  the art itself: the six pieces and the pointing glove, in the
          neutral tones both sides are dressed from (body BLUE, NAVY,
          SILVER, WHITE dark to light, CYAN glint, INK outline, GOLD and
          WOOD trim). Change shapes and shading here. Each piece stands
          with its base centre on the same point of its cell (9, 26 in the
          cell); keep it there, the glove's fingertip too.
  WHITE,  the pieces as each side shows them: MASTER through the palette
  BLACK   swap. Recolour them to change the swap - a colour of MASTER must
          become one colour on a side (the import takes each MASTER colour's
          most common colour and reports the rest). Or redraw them outright
          (clear MASTER's pieces, or change their shapes here): the import
          then rebuilds the art from these two rows - each White/Black pair
          of colours becomes one tone of the shared art - so both sides'
          shapes must match, and at most 16 pairs.
  SWAP    the same swap as a key: per palette colour, what it becomes on
          White and on Black. Editing a square changes the swap too (the
          piece rows win if both changed).

Palette: the swatch lists the 16 colours. FX_A and FX_B are animated in
the game (placeholders here); FELT_DK, FELT and FELT_LT follow the board
colour option. Colours are read by value (editors may reorder the colour
table); a colour that isn't the game's is an error - changing the palette
itself is a code change.
"""
import re
import subprocess
import sys
from pathlib import Path

from PIL import Image

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import assets  # noqa: E402
from assets import (ART, NAMES, PALETTE, PIECES, TRANSPARENT, load_hand, load_piece,  # noqa: E402
                    load_sides, save_png, save_sides)

INK = 0

ROOT = HERE.parent
SHEET = ART / "sheet.png"

# Layout, in sheet pixels.
CW, CH = 18, 32                 # a cell
AX, AY = 9, 26                  # where a piece's base centre sits in its cell
LX = 30                         # left of the cells (labels before it)
ROW_Y = [12, 12 + CH, 12 + 2 * CH]         # MASTER, WHITE, BLACK
KEY_Y = 12 + 3 * CH + 10                   # SWAP key: three rows of squares
KEY_STEP, KEY_SQ = 7, 6
SW_X = LX + 7 * CW + 8                     # palette swatch
SW_Y, SW_STEP = 12, 8
W, H = SW_X + 48, max(KEY_Y + 3 * KEY_STEP + 4, SW_Y + 16 * SW_STEP + 2)
# FX_A/FX_B are animated in the game; distinct placeholders keep the colour
# table free of duplicates.
FX_SHOW = {14: 0xF0F, 15: 0xFE8}


def table_rgb(i):
    c = FX_SHOW.get(i, PALETTE[i])
    return ((c >> 8) * 17, ((c >> 4) & 15) * 17, (c & 15) * 17)


# ---------------------------------------------------------------------------
# The game's 3x5 font (src/gfx/Draw.cpp) for the labels
# ---------------------------------------------------------------------------
def font35():
    src = re.sub(r"//[^\n]*", "", (ROOT / "src" / "gfx" / "Draw.cpp").read_text())
    body = src[src.index("FONT35[][3] = {"):]
    body = body[:body.index("};")]
    glyphs = [tuple(int(v, 16) for v in g) for g in re.findall(r"\{(0x[0-9A-Fa-f]+),(0x[0-9A-Fa-f]+),(0x[0-9A-Fa-f]+)\}", body)]
    idx = src[src.index("IDX35[91] = {"):]
    idx = [int(v) for v in re.findall(r"-?\d+", idx[idx.index("{") + 1:idx.index("};")])]
    return glyphs, idx


def text(px, x, y, s, c):
    glyphs, idx = FONT
    for ch in s:
        g = idx[ord(ch) - 32] if 32 <= ord(ch) <= 122 else -1
        if g >= 0:
            for col in range(3):
                for row in range(6):
                    if glyphs[g][col] >> row & 1:
                        px[x + col, y + row] = c
        x += 4


FONT = font35()


# ---------------------------------------------------------------------------
# Export
# ---------------------------------------------------------------------------
def blit(px, img, x, y):
    for j, row in enumerate(img):
        for i, c in enumerate(row):
            if c != TRANSPARENT:
                px[x + i, y + j] = c


def remapped(img, m):
    return [[c if c == TRANSPARENT else m[c] for c in row] for row in img]


def fits(img, ax, ay, name):
    if AX - ax < 0 or AY - ay < 0 or AX - ax + len(img[0]) > CW or AY - ay + len(img) > CH:
        raise SystemExit(f"{name}: {len(img[0])}x{len(img)} with its base at {ax},{ay} does not fit a {CW}x{CH} cell")


def export(path):
    im = Image.new("P", (W, H), TRANSPARENT)
    pal = []
    for i in range(16):
        pal += list(table_rgb(i))
    pal += [128, 128, 128]                   # 16: transparent
    im.putpalette(pal + [0] * (768 - len(pal)))
    px = im.load()
    maps = load_sides()
    for r, label in enumerate(["MASTER", "WHITE", "BLACK"]):
        text(px, 2, ROW_Y[r] + 12, label, 0)
    for c, name in enumerate(PIECES):
        img, ax, ay = load_piece(name)
        fits(img, ax, ay, name)
        text(px, LX + c * CW + 7, 4, "PNBRQK"[c], 0)
        blit(px, img, LX + c * CW + AX - ax, ROW_Y[0] + AY - ay)
        for r in (1, 2):
            blit(px, remapped(img, maps[r - 1]), LX + c * CW + AX - ax, ROW_Y[r] + AY - ay)
    hand = load_hand()
    text(px, LX + 6 * CW + 1, 4, "HAND", 0)
    blit(px, hand, LX + 6 * CW + AX - len(hand[0]) // 2, ROW_Y[0] + AY - len(hand) + 1)
    # The swap key.
    text(px, 2, KEY_Y - 8, "SWAP", 0)
    for r, label in enumerate(["ART", "WHITE", "BLACK"]):
        text(px, 8 if r == 0 else 2, KEY_Y + r * KEY_STEP, label, 0)
        for i in range(16):
            c = i if r == 0 else maps[r - 1][i]
            for dy in range(KEY_SQ):
                for dx in range(KEY_SQ):
                    px[LX + i * KEY_STEP + dx, KEY_Y + r * KEY_STEP + dy] = c
    # The palette swatch.
    for i in range(16):
        y = SW_Y + i * SW_STEP
        for dy in range(6):
            for dx in range(6):
                px[SW_X + dx, y + dy] = i
        text(px, SW_X + 8, y, f"{i:>2} {NAMES[i].replace('_', ' ')}", 0)
    im.info["transparency"] = TRANSPARENT
    im.save(path, transparency=TRANSPARENT)
    # A 4x preview on a mid grey, to look at.
    pv = im.convert("RGBA")
    bg = Image.new("RGBA", pv.size, (96, 96, 104, 255))
    bg.alpha_composite(pv)
    bg.convert("RGB").resize((W * 4, H * 4), Image.NEAREST).save(path.with_name(path.stem + "_preview.png"))
    print(f"exported {path} ({W}x{H}) and its 4x preview")


# ---------------------------------------------------------------------------
# Import
# ---------------------------------------------------------------------------
def read_indices(path):
    """-> 2D game palette indices, TRANSPARENT where clear. Matched by colour:
    editors may save the colour table in their own order."""
    lut = {table_rgb(i): i for i in reversed(range(16))}     # GOLD before FX_B
    im = Image.open(path).convert("RGBA")
    out, bad = [], {}
    for y in range(im.height):
        row = []
        for x in range(im.width):
            r, g, b, a = im.getpixel((x, y))
            if a < 128:
                row.append(TRANSPARENT)
            elif (r, g, b) in lut:
                row.append(lut[(r, g, b)])
            else:
                bad.setdefault((r, g, b), (x, y))
                row.append(TRANSPARENT)
        out.append(row)
    if bad:
        raise SystemExit("not palette colours: " + ", ".join(
            f"#{r:02X}{g:02X}{b:02X} at {xy}" for (r, g, b), xy in bad.items()) +
            " - paint with the sheet's colours (changing the palette itself is a code change)")
    return out


def cell(pix, c, r):
    x0, y0 = LX + c * CW, ROW_Y[r]
    return [row[x0:x0 + CW] for row in pix[y0:y0 + CH]]


def crop(img):
    """Trim to the opaque pixels -> (rows, left, top), or None if empty."""
    ys = [y for y, row in enumerate(img) if any(v != TRANSPARENT for v in row)]
    xs = [x for x in range(len(img[0])) if any(row[x] != TRANSPARENT for row in img)]
    if not ys:
        return None
    return [row[xs[0]:xs[-1] + 1] for row in img[ys[0]:ys[-1] + 1]], xs[0], ys[0]


def place(img, ax, ay):
    """A piece as exported into its cell."""
    out = [[TRANSPARENT] * CW for _ in range(CH)]
    for j, row in enumerate(img):
        for i, v in enumerate(row):
            if v != TRANSPARENT:
                out[AY - ay + j][AX - ax + i] = v
    return out


def opaque(img):
    return [[v != TRANSPARENT for v in row] for row in img]


def write_piece(name, img, left, top):
    ax, ay = AX - left, AY - top
    if (img, ax, ay) != load_piece(name):
        dst = ART / "pieces" / f"{name}.png"
        save_png(dst, img)
        dst.with_suffix(".anchor").write_text(f"{ax} {ay}\n")
        print(f"art: {name} -> tools/art/pieces/{name}.png (base centre {ax},{ay})")


def import_(path):
    pix = read_indices(path)
    if len(pix) < H or len(pix[0]) < W:
        raise SystemExit(f"{path}: expected at least {W}x{H} (the exported layout)")
    notes = []
    old = load_sides()
    cells = [[cell(pix, c, r) for r in range(3)] for c in range(len(PIECES))]

    # Which rows are the art? MASTER, if every piece is there and both side
    # rows keep its shapes; else the WHITE and BLACK rows themselves.
    from_sides = any(not crop(m) or opaque(w) != opaque(m) or opaque(b) != opaque(m) for m, w, b in cells)
    if from_sides:
        new = rebuild_from_sides(cells, notes)
    else:
        new = [list(m) for m in old]
        swap_from_rows(cells, old, new, notes)
        swap_from_key(pix, old, new, notes)
        for c, name in enumerate(PIECES):
            img, left, top = crop(cells[c][0])
            write_piece(name, img, left, top)

    for side, sname in ((0, "White"), (1, "Black")):
        for i in range(16):
            if new[side][i] != old[side][i]:
                print(f"swap: {sname}'s {NAMES[i]} -> {NAMES[new[side][i]]} (was {NAMES[old[side][i]]})")
    if new != old:
        save_sides(new)

    # The glove.
    got = crop(cell(pix, 6, 0))
    if not got:
        notes.append("MASTER hand: empty cell - left as it was")
    elif got[0] != load_hand():
        save_png(ART / "hand.png", got[0])
        print("art: hand -> tools/art/hand.png")

    for n in notes:
        print("note:", n)
    subprocess.run([sys.executable, str(HERE / "assets.py")], check=True)


def swap_from_rows(cells, old, new, notes):
    """MASTER is the art: what each of its colours became on each side (the
    most common colour; the rest reported)."""
    for side, sname in ((0, "WHITE"), (1, "BLACK")):
        votes = {}
        for c in range(len(PIECES)):
            m, got = cells[c][0], cells[c][side + 1]
            for y in range(CH):
                for x in range(CW):
                    if m[y][x] != TRANSPARENT:
                        v = votes.setdefault(m[y][x], {})
                        v[got[y][x]] = v.get(got[y][x], 0) + 1
        for m, v in votes.items():
            best = max(v, key=v.get)
            stray = sum(v.values()) - v[best]
            if stray:
                notes.append(f"{sname}: {NAMES[m]} became {NAMES[best]} in {v[best]} pixels, something else "
                             f"in {stray} - one colour per art colour on a side; kept {NAMES[best]}")
            new[side][m] = best


def swap_from_key(pix, old, new, notes):
    """The SWAP key's squares count where the pieces left a colour as it was."""
    for side in (0, 1):
        y = KEY_Y + (side + 1) * KEY_STEP + KEY_SQ // 2
        for i in range(16):
            v = pix[y][LX + i * KEY_STEP + KEY_SQ // 2]
            if v == TRANSPARENT or v == old[side][i]:
                continue
            if new[side][i] == old[side][i]:
                new[side][i] = v
            elif new[side][i] != v:
                notes.append(f"{('WHITE', 'BLACK')[side]}: {NAMES[i]} - pieces say {NAMES[new[side][i]]}, "
                             f"the key says {NAMES[v]}; took the pieces")


# Free slots for art tones White and Black both already use, most fitting
# first (CYAN was the glint, NAVY a body tone); felt and FX colours last.
SPARE = (13, 11, 5, 10, 1, 12, 6, 7, 8, 9, 2, 3, 4, 14, 15, 0)


def rebuild_from_sides(cells, notes):
    """The WHITE and BLACK rows are the art: every pixel's pair (White's
    colour, Black's) becomes one colour of the shared art, and the swap
    sends it to each. The pair drawn in INK on White keeps INK (the game
    recolours INK for its outline highlights); the others take White's
    colour, else Black's, else a free palette slot (never drawn as itself -
    the art is always drawn through the swap)."""
    pairs = {}
    for c, name in enumerate(PIECES):
        _, w, b = cells[c]
        if opaque(w) != opaque(b):
            n = sum(1 for y in range(CH) for x in range(CW) if (w[y][x] == TRANSPARENT) != (b[y][x] == TRANSPARENT))
            raise SystemExit(f"{name}: WHITE and BLACK differ in shape at {n} pixels - the two sides share one "
                             f"set of art, so their outlines must match")
        for y in range(CH):
            for x in range(CW):
                if w[y][x] != TRANSPARENT:
                    p = (w[y][x], b[y][x])
                    pairs[p] = pairs.get(p, 0) + 1
    if len(pairs) > 16:
        raise SystemExit(f"{len(pairs)} different White/Black colour pairs - the art has room for 16")
    key, taken = {}, set()
    ink = [p for p in pairs if p[0] == INK]
    for p in sorted(ink, key=lambda p: -pairs[p])[:1]:
        key[p] = INK
        taken.add(INK)
    for p in sorted(pairs, key=lambda p: -pairs[p]):
        if p in key:
            continue
        for k in (p[0], p[1]) + SPARE:
            if k not in taken:
                key[p] = k
                taken.add(k)
                break
    new = [list(range(16)), list(range(16))]
    for (w, b), k in key.items():
        new[0][k], new[1][k] = w, b
    for (w, b), n in sorted(pairs.items(), key=lambda t: -t[1]):
        print(f"tone {NAMES[key[(w, b)]]:<8} White {NAMES[w]:<8} Black {NAMES[b]:<8} ({n} px)")
    for c, name in enumerate(PIECES):
        w, b = cells[c][1], cells[c][2]
        art = [[key[(w[y][x], b[y][x])] if w[y][x] != TRANSPARENT else TRANSPARENT for x in range(CW)]
               for y in range(CH)]
        img, left, top = crop(art)
        write_piece(name, img, left, top)
    notes.append("the art was rebuilt from the WHITE and BLACK rows (MASTER's pieces were missing or "
                 "the side rows had new shapes); re-export to see it as MASTER")
    return new


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if not args or args[0] not in ("export", "import"):
        raise SystemExit(__doc__)
    path = Path(args[1]) if len(args) > 1 else SHEET
    if args[0] == "export":
        export(path)
    else:
        import_(path)


if __name__ == "__main__":
    main()
