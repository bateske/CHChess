"""CHChess asset pipeline.

    python tools/assets.py         # build src/assets/*, write previews to build/assets/

Sources:
  * Pieces: tools/art/pieces/<name>.png if present (hand-finished art),
    else tools/art/gen/<name>.png as rendered by tools/pieces.py. Each has a
    <name>.anchor file: the base centre, in pixels from the top-left.
    Colours are the neutral tones pieces.py documents; the game remaps them
    per side.
  * The palette swap that dresses the pieces as each side: tools/art/sides.txt.
  * The pointing hand: tools/art/hand.png if present, else palette-letter
    text in tools/art/hand.txt.
  * tools/sheet.py exports all of it as one sprite sheet to edit, and imports
    the edited sheet back into these files.

Outputs:
  src/assets/Assets.h / Assets.cpp   - generated, do not edit
  build/assets/*.png                 - previews on the real palette
"""
from pathlib import Path

from PIL import Image

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
ART = HERE / "art"
OUT_H = ROOT / "src" / "assets" / "Assets.h"
OUT_C = ROOT / "src" / "assets" / "Assets.cpp"
PREVIEW = ROOT / "build" / "assets"

# Must match src/gfx/Palette.cpp.
PALETTE = [0x000, 0xFFF, 0x042, 0x173, 0x4B5, 0xBBC, 0xE12, 0x702,
           0xFC2, 0x741, 0x26E, 0x125, 0xFB8, 0x6EF, 0xF0F, 0xFC2]
# Letters used in tools/art/*.txt. ' ' / '.' = transparent.
LETTER = {"k": 0, "w": 1, "d": 2, "f": 3, "g": 4, "s": 5, "r": 6, "m": 7,
          "y": 8, "b": 9, "u": 10, "n": 11, "p": 12, "c": 13, "x": 14, "z": 15}
TRANSPARENT = 16
PIECES = ["pawn", "knight", "bishop", "rook", "queen", "king"]
NAMES = ["INK", "WHITE", "FELT_DK", "FELT", "FELT_LT", "SILVER", "RED", "WINE",
         "GOLD", "WOOD", "BLUE", "NAVY", "SKIN", "CYAN", "FX_A", "FX_B"]
SIDES = ART / "sides.txt"
SIDES_HEADER = """# The palette swap that dresses the one set of piece art as each side.
# The art (tools/art/pieces/, else tools/art/gen/) is drawn in neutral
# tones - body BLUE, NAVY, SILVER, WHITE from dark to light, a CYAN glint,
# INK outline, GOLD and WOOD trim. One line per art colour the swap
# changes: the art colour, then White's colour, then Black's (names as in
# src/gfx/Palette.h). Colours not listed stay as drawn.
#
# tools/sheet.py import rewrites this from an edited sheet.
"""


def load_sides():
    """tools/art/sides.txt -> [white, black], each art colour -> side colour (16 entries)."""
    maps = [list(range(16)), list(range(16))]
    for ln in SIDES.read_text().splitlines():
        words = ln.split("#", 1)[0].split()
        if not words:
            continue
        a, w, b = (NAMES.index(n) for n in words)
        maps[0][a], maps[1][a] = w, b
    return maps


def save_sides(maps):
    lines = [f"{NAMES[a]:<8} {NAMES[maps[0][a]]:<8} {NAMES[maps[1][a]]}"
             for a in range(16) if maps[0][a] != a or maps[1][a] != a]
    SIDES.write_text(SIDES_HEADER + "\n".join(lines) + "\n", newline="\n")


def piece_source(name):
    """The piece's art file: hand-finished in tools/art/pieces/, else as rendered."""
    src = ART / "pieces" / f"{name}.png"
    return src if src.exists() else ART / "gen" / f"{name}.png"


def load_piece(name):
    """-> rows of palette indices, and the base centre (ax, ay)."""
    src = piece_source(name)
    ax, ay = (int(v) for v in src.with_suffix(".anchor").read_text().split())
    return load_png(src), ax, ay


def load_hand():
    png = ART / "hand.png"
    return load_png(png) if png.exists() else load_art("hand")[0]


def save_png(path, img):
    """Rows of palette indices -> a palette-exact RGBA PNG (what load_png reads)."""
    h, w = len(img), len(img[0])
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    for y in range(h):
        for x in range(w):
            if img[y][x] != TRANSPARENT:
                im.putpixel((x, y), rgb(img[y][x]) + (255,))
    path.parent.mkdir(parents=True, exist_ok=True)
    im.save(path)


def rgb(i):
    c = PALETTE[i]
    return ((c >> 8) * 17, ((c >> 4) & 15) * 17, (c & 15) * 17)


def load_png(path):
    """Palette-exact PNG -> rows of palette indices; alpha 0 is transparent."""
    lut = {rgb(i): i for i in range(15)}          # FX_B (15) shares GOLD's colour
    im = Image.open(path).convert("RGBA")
    rows = []
    for y in range(im.height):
        row = []
        for x in range(im.width):
            r, g, b, a = im.getpixel((x, y))
            if a == 0:
                row.append(TRANSPARENT)
            elif (r, g, b) in lut and a == 255:
                row.append(lut[(r, g, b)])
            else:
                raise SystemExit(f"{path.name} ({x},{y}): #{r:02X}{g:02X}{b:02X} alpha {a} is not a palette colour")
        rows.append(row)
    return rows


def load_art(name):
    """tools/art/<name>.txt: palette letters, one row per line; '#' starts a comment line.
    Several images may follow each other, separated by a blank line."""
    imgs, cur = [], []
    for ln in (ART / f"{name}.txt").read_text().splitlines():
        if ln.startswith("#"):
            continue
        if not ln.strip():
            if cur:
                imgs.append(cur)
                cur = []
            continue
        cur.append(ln.rstrip())
    if cur:
        imgs.append(cur)
    out = []
    for rows in imgs:
        w = max(len(r) for r in rows)
        out.append([[TRANSPARENT if ch in " ." else LETTER[ch] for ch in r.ljust(w)] for r in rows])
    return out


def pack_span4(img, trans=TRANSPARENT):
    """Colour image -> w, h, then per row: n, then n bytes of (len-1)<<4 | colour
    (colour 15 = skip). Trailing transparency is implicit."""
    h, w = len(img), len(img[0])
    out = [w, h]
    for row in img:
        runs, x = [], 0
        while x < w:
            c = row[x]
            s = x
            while x < w and row[x] == c and x - s < 16:
                x += 1
            runs.append((x - s, c))
        while runs and runs[-1][1] == trans:
            runs.pop()
        out.append(len(runs))
        for n, c in runs:
            out.append(((n - 1) << 4) | (15 if c == trans else c))
    return out


def pack_rows1(img):
    """1-bit rows, MSB first (any opaque pixel = 1)."""
    h, w = len(img), len(img[0])
    out = []
    for row in img:
        for x0 in range(0, w, 8):
            b = 0
            for i in range(8):
                x = x0 + i
                if x < w and row[x] != TRANSPARENT:
                    b |= 0x80 >> i
            out.append(b)
    return out


def preview(name, img, scale=6, bg=3):
    h, w = len(img), len(img[0])
    im = Image.new("RGB", (w, h), rgb(bg))
    for y in range(h):
        for x in range(w):
            if img[y][x] != TRANSPARENT:
                im.putpixel((x, y), rgb(img[y][x]))
    im.resize((w * scale, h * scale), Image.NEAREST).save(PREVIEW / f"{name}.png")


def c_array(name, data, per_line=16):
    lines = [f"const uint8_t {name}[{len(data)}] = {{"]
    for i in range(0, len(data), per_line):
        lines.append("    " + ", ".join(str(v) for v in data[i:i + per_line]) + ",")
    lines.append("};")
    return "\n".join(lines)


def main():
    PREVIEW.mkdir(parents=True, exist_ok=True)
    decls, defs = [], []
    total = 0

    # Pieces.
    table = []
    for name in PIECES:
        img, ax, ay = load_piece(name)
        data = pack_span4(img)
        defs.append(c_array(f"PIECE_{name.upper()}", data))
        table.append((f"PIECE_{name.upper()}", ax, ay))
        total += len(data)
        preview(f"piece_{name}", img)
    defs.append("const PieceArt PIECE_ART[6] = {\n" +
                "".join(f"    {{{n}, {ax}, {ay}}},\n" for n, ax, ay in table) + "};")
    decls.append("struct PieceArt { const uint8_t *data; int8_t ax, ay; };   // span4 art, base centre\n"
                 "extern const PieceArt PIECE_ART[6];                          // pawn, knight, bishop, rook, queen, king")
    total += 6 * 8

    # Each side's colours for the art.
    maps = load_sides()
    defs.append("const uint8_t SIDE_REMAP[2][16] = {\n" +
                "".join("    {" + ", ".join(str(v) for v in m) + "},\n" for m in maps) + "};")
    decls.append("extern const uint8_t SIDE_REMAP[2][16];                      // art colour -> White's, Black's (tools/art/sides.txt)")
    total += 32

    # The pointing hand (span4).
    hand = load_hand()
    data = pack_span4(hand)
    defs.append(c_array("HAND", data))
    decls.append("extern const uint8_t HAND[];                                 // span4, fingertip at bottom centre")
    total += len(data)
    preview("hand", hand, 8)

    OUT_H.parent.mkdir(parents=True, exist_ok=True)
    OUT_H.write_text("// Generated by tools/assets.py - do not edit.\n#pragma once\n#include <stdint.h>\n\n"
                     + "\n".join(decls) + "\n", newline="\n")
    OUT_C.write_text("// Generated by tools/assets.py - do not edit.\n"
                     "// Piece art rendered by tools/pieces.py (and finished by hand where\n"
                     "// tools/art/pieces/ has a file); hand-drawn cursor and icons in tools/art/.\n"
                     "#include \"Assets.h\"\n\n" + "\n\n".join(defs) + "\n", newline="\n")
    print(f"assets: {total} bytes of data")


if __name__ == "__main__":
    main()
