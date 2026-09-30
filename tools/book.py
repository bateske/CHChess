"""Trim ch2k's opening book to fewer plies.

    python tools/book.py            # sizes at each depth
    python tools/book.py 4          # rewrite the OPENING_BOOK array in src/engine/ch2k.hpp at depth 4

The book is a tree of moves serialised depth first, one byte a move: bits
0-5 index the move in gen_moves() order, bit 7 = children follow, bit 6 =
last sibling. Cutting at a depth keeps every line, just shorter.
"""
import re
import sys
from pathlib import Path

HPP = Path(__file__).resolve().parent.parent / "src" / "engine" / "ch2k.hpp"


def load():
    t = HPP.read_text()
    m = re.search(r"static u8 const OPENING_BOOK\[(\d+)\] PROGMEM =\s*\{(.*?)\};", t, re.S)
    return t, m, [int(v) for v in re.findall(r"\d+", m.group(2))]


def parse(data, i, depth):
    """Siblings starting at index i -> (list of (byte, children), next index)."""
    nodes = []
    while True:
        b = data[i]
        i += 1
        kids = []
        if b & 0x80:
            kids, i = parse(data, i, depth + 1)
        nodes.append((b, kids))
        if b & 0x40:
            return nodes, i


def emit(nodes, depth, maxd, out):
    for k, (b, kids) in enumerate(nodes):
        has = bool(kids) and depth + 1 < maxd
        last = k == len(nodes) - 1
        out.append((b & 0x3F) | (0x80 if has else 0) | (0x40 if last else 0))
        if has:
            emit(kids, depth + 1, maxd, out)


def main():
    t, m, data = load()
    root, end = parse(data, 0, 0)
    assert end == len(data)
    if len(sys.argv) < 2:
        for d in range(1, 8):
            out = []
            emit(root, 0, d, out)
            print(f"depth {d}: {len(out)} bytes")
        return
    d = int(sys.argv[1])
    out = []
    emit(root, 0, d, out)
    rows = ",\n".join("    " + ", ".join(f"{v:3d}" for v in out[i:i + 8]) for i in range(0, len(out), 8))
    new = f"static u8 const OPENING_BOOK[{len(out)}] PROGMEM =\n{{\n{rows},\n}};"
    HPP.write_text(t[:m.start()] + new + t[m.end():], newline="\n")
    print(f"book cut to depth {d}: {len(out)} bytes")


if __name__ == "__main__":
    main()
