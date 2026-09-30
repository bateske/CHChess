"""The piece art as one sprite sheet to edit (Photoshop or anything that
keeps an indexed PNG's colour table), and back.

    python tools/sheet.py export [SHEET]      # write tools/art/sheet.png (+ sheet_preview.png, 4x)
    python tools/sheet.py import [SHEET]      # read it back, then rebuild the game's assets
    python tools/sheet.py import --palette    # ...and take its colour table as the game palette

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
          most common colour and reports the rest). Shape edits here are
          ignored.
  SWAP    the same swap as a key: per palette colour, what it becomes on
          White and on Black. Editing a square changes the swap too (the
          piece rows win if both changed).

Palette: the swatch lists the 16 colours. FX_A and FX_B are animated in
the game (placeholders here); FELT_DK, FELT and FELT_LT follow the board
colour option. Changing the colour table itself changes the whole game's
palette, so the import only reports it unless given --palette.
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
                    load_sides, rgb, save_png, save_sides)

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
    """-> 2D palette indices (TRANSPARENT where clear) and the colour table (16 RGBs) or None."""
    im = Image.open(path)
    if im.mode == "P":
        table = im.getpalette()[:48]
        trans = im.info.get("transparency")
        rgba_trans = None
        if isinstance(trans, bytes):          # per-entry alpha
            rgba_trans = trans
        px = im.load()
        out = []
        for y in range(im.height):
            row = []
            for x in range(im.width):
                i = px[x, y]
                clear = i >= 16 or i == trans or (rgba_trans is not None and i < len(rgba_trans) and rgba_trans[i] == 0)
                row.append(TRANSPARENT if clear else i)
            out.append(row)
        return out, [tuple(table[i * 3:i * 3 + 3]) for i in range(16)]
    # Converted to RGB(A) on the way: match colours exactly (FX_B's placeholder
    # included; GOLD before FX_B).
    lut = {table_rgb(i): i for i in reversed(range(16))}
    im = im.convert("RGBA")
    out = []
    for y in range(im.height):
        row = []
        for x in range(im.width):
            r, g, b, a = im.getpixel((x, y))
            if a < 128:
                row.append(TRANSPARENT)
            elif (r, g, b) in lut:
                row.append(lut[(r, g, b)])
            else:
                raise SystemExit(f"({x},{y}): #{r:02X}{g:02X}{b:02X} is not a palette colour")
        out.append(row)
    return out, None


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
    """A piece as exported into its cell (the old art, to compare the side rows with)."""
    out = [[TRANSPARENT] * CW for _ in range(CH)]
    for j, row in enumerate(img):
        for i, v in enumerate(row):
            if v != TRANSPARENT:
                out[AY - ay + j][AX - ax + i] = v
    return out


def import_(path, take_palette):
    pix, table = read_indices(path)
    if len(pix) < H or len(pix[0]) < W:
        raise SystemExit(f"{path}: expected at least {W}x{H} (the exported layout)")
    notes = []
    old = load_sides()
    new = [list(m) for m in old]

    # The swap, from the side rows: what each MASTER colour became, compared
    # against the art as it was exported.
    from_rows = [dict(), dict()]
    for c, name in enumerate(PIECES):
        art, ax, ay = load_piece(name)
        master = place(art, ax, ay)
        for side in (0, 1):
            got = cell(pix, c, side + 1)
            for y in range(CH):
                for x in range(CW):
                    m, v = master[y][x], got[y][x]
                    if (m == TRANSPARENT) != (v == TRANSPARENT):
                        from_rows[side].setdefault("shape", set()).add(name)
                    elif m != TRANSPARENT:
                        votes = from_rows[side].setdefault(m, {})
                        votes[v] = votes.get(v, 0) + 1
    for side, sname in ((0, "WHITE"), (1, "BLACK")):
        for name in sorted(from_rows[side].pop("shape", ())):
            notes.append(f"{sname} {name}: shape differs from MASTER - ignored (edit shapes in MASTER)")
        for m, votes in from_rows[side].items():
            best = max(votes, key=votes.get)
            stray = sum(votes.values()) - votes[best]
            if stray:
                notes.append(f"{sname}: {NAMES[m]} became {NAMES[best]} in {votes[best]} pixels, "
                              f"something else in {stray} - a swap is one colour per art colour; kept {NAMES[best]}")
            if best != old[side][m]:
                new[side][m] = best
    # The key: squares changed there count unless the pieces changed that colour.
    for side in (0, 1):
        y = KEY_Y + (side + 1) * KEY_STEP + KEY_SQ // 2
        for i in range(16):
            v = pix[y][LX + i * KEY_STEP + KEY_SQ // 2]
            if v != TRANSPARENT and v != old[side][i]:
                if new[side][i] == old[side][i]:
                    new[side][i] = v
                elif new[side][i] != v:
                    notes.append(f"{('WHITE', 'BLACK')[side]}: {NAMES[i]} - pieces say {NAMES[new[side][i]]}, "
                                 f"the key says {NAMES[v]}; took the pieces")
    for side, sname in ((0, "White"), (1, "Black")):
        for i in range(16):
            if new[side][i] != old[side][i]:
                print(f"swap: {sname}'s {NAMES[i]} -> {NAMES[new[side][i]]} (was {NAMES[old[side][i]]})")
    if new != old:
        save_sides(new)

    # The art itself, from MASTER.
    for c, name in enumerate(PIECES + ["hand"]):
        got = crop(cell(pix, c, 0))
        if not got:
            notes.append(f"MASTER {name}: empty cell - left as it was")
            continue
        img, left, top = got
        if name == "hand":
            if img != load_hand():
                save_png(ART / "hand.png", img)
                print("art: hand -> tools/art/hand.png")
            continue
        ax, ay = AX - left, AY - top
        if (img, ax, ay) != load_piece(name):
            dst = ART / "pieces" / f"{name}.png"
            save_png(dst, img)
            dst.with_suffix(".anchor").write_text(f"{ax} {ay}\n")
            print(f"art: {name} -> tools/art/pieces/{name}.png (base centre {ax},{ay})")

    # The colour table.
    if table:
        changed = [(i, table[i]) for i in range(14) if table[i] != rgb(i)]
        for i, t in changed:
            c = (t[0] // 17 << 8) | (t[1] // 17 << 4) | (t[2] // 17)
            notes.append(f"colour table: {NAMES[i]} is #{t[0]:02X}{t[1]:02X}{t[2]:02X} "
                         f"(game: 0x{PALETTE[i]:03X})" + ("" if take_palette else " - not applied, use --palette"))
        if changed and take_palette:
            apply_palette(changed)

    for n in notes:
        print("note:", n)
    subprocess.run([sys.executable, str(HERE / "assets.py")], check=True)


def apply_palette(changed):
    """Write colour-table changes into the game (Palette.cpp) and the tools (assets.py)."""
    for i, t in changed:
        c = (round(t[0] / 17) << 8) | (round(t[1] / 17) << 4) | round(t[2] / 17)
        old = PALETTE[i]
        p = ROOT / "src" / "gfx" / "Palette.cpp"
        s = p.read_text()
        s = re.sub(rf"0x{old:03X},(\s*// {NAMES[i]}\b)", f"0x{c:03X},\\1", s, count=1)
        p.write_text(s, newline="\n")
        PALETTE[i] = c
        print(f"palette: {NAMES[i]} 0x{old:03X} -> 0x{c:03X}")
    p = HERE / "assets.py"
    s = p.read_text()
    s = re.sub(r"PALETTE = \[[^\]]*\]", "PALETTE = [" + ", ".join(f"0x{v:03X}" for v in PALETTE[:8]) +
               ",\n           " + ", ".join(f"0x{v:03X}" for v in PALETTE[8:]) + "]", s, count=1)
    p.write_text(s, newline="\n")


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if not args or args[0] not in ("export", "import"):
        raise SystemExit(__doc__)
    path = Path(args[1]) if len(args) > 1 else SHEET
    if args[0] == "export":
        export(path)
    else:
        import_(path, "--palette" in sys.argv)


if __name__ == "__main__":
    main()
