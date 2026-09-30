# CHGfx: notes from CHChess

CHChess draws an isometric board, up to 32 scaled sprites, particles,
outlined gradient lettering and a HUD every frame, on CHGfx 1.2.0 (4 bpp
framebuffer, 16-colour palette, async DMA flush). The library held up
well. These are the places where the game had to build its own tools,
with suggestions for what could move into the library. They are roughly
in order of value.

## What worked and should stay

* **The 4 bpp framebuffer with a palette converted on every flush.**
  Every frame is re-sent, so palette animation is free. CHChess leans on
  this constantly: shimmering move targets, the fading hover outline,
  the gold-to-white pulse, fades to black. It also skips redrawing a
  frame whose inputs have not changed and just flushes the old buffer
  again, so an idle board costs a flush and almost no drawing, while the
  palette keeps moving. That pattern deserves a paragraph in the README.
* **`gfx_chunkScratch()`**: 1 KB free between `gfx_wait()` and the next
  flush. CHChess uses it to decode a sprite for rotation and to build a
  flash page for saving. Worth keeping documented exactly as it is.
* **`gfx_flushAsync()` / `gfx_wait()`**, plus the host build in the
  simulator that flags drawing during a flush. That check caught real
  bugs.

## Suggestions

### 1. Document the flush's CPU cost, and let drawing overlap it

The performance notes call the pixel conversion "free" against the SPI
budget. That holds for the wire, but the chunk conversion runs on the
CPU, in the DMA interrupt. Measured on the board, an async full flush
takes about **5 ms of CPU time** per frame, out of 16.7 ms. CHChess's CPU
opponent noticed: it now draws at most one frame every 133 ms while it
searches, because 60 fps flushing alone took about a third of the search.

Two things would help:

* **Say so in the README**, with the figure, so games budget for it.
* **Expose flush progress** (`gfx_flushedRows()` or a row callback).
  Rows that have already been converted and sent are free to draw into.
  A game could start drawing the next frame's top while the bottom is
  still going out ("racing the beam"), instead of waiting in
  `gfx_wait()`. Today all drawing waits for the whole flush.

### 2. Palette-swapped span sprites (`sprite4`)

Both CHBlackjack and CHChess carry the same kind of sprite routine
(CHChess: `src/gfx/Draw.cpp`, packer in `tools/assets.py`):

* the art is stored as runs per row (`(len-1) << 4 | colour`, colour 15
  transparent). That is compact, and fast because a run is a fill, not a
  pixel loop;
* it is drawn through a **16-entry remap table**, so one image serves many
  looks. CHChess keeps a single set of pieces, and the table turns it into
  White or Black, a red flash, a white hit flash or a coloured outline.
  The glove works the same way, white for you and red for the CPU;
* it is **scaled** (Q8, nearest neighbour), for the camera zoom.

`gfx_blit()` has neither remapping nor scaling. A `gfx_sprite4(data, x, y,
remap, scale)` with its packer script would be the most useful addition:
palette swapping is the natural way to get variety out of 16 colours.
Keep the separate 1:1 fast path. On this core, folding scaling into the
same loop measurably slowed unscaled drawing (register pressure).

### 3. A clip rectangle

There is no clip region; everything clips to the screen. CHChess draws
the board under a 10-pixel HUD, shakes only rows 10-127, and keeps
lettering inside panels, each by hand. `gfx_setClip(x, y, w, h)` honoured
by the fills, lines, text and blits (they already clip to the screen, so
it is a change of bounds) would remove a class of off-by-one bugs.

### 4. Fast row operations in SRAM

newlib-nano's `memmove` and `memcpy` are byte loops running from flash
(3 wait states). A screen shake that moved framebuffer rows with
`memmove` cost about 10 ms a frame, and CHChess replaced it with a
word-copy routine in SRAM. The library is the natural home for:

* `gfx_scrollRows(y0, y1, dy)` / horizontal nibble shifts (shake, scrolling
  backgrounds);
* `gfx_fillRows(y0, y1, c)` and `gfx_copyRow(dst, src)`, the building
  blocks CHChess uses to fill the board a row at a time.

### 5. Primitives both games rewrote

* **Rounded rectangles** (filled and outlined, radius up to 4): every
  panel, plate and menu highlight.
* **Filled ellipse**: shadows under pieces.
* **50% dither fill** (`dither(x, y, w, h, c, phase)`): darkened
  backdrops behind menus and the felt borders.
* **Rotated/scaled sprite** (`spriteRot`), using the chunk scratch: the
  tumbling captured piece and the toppling king.
* **Outlined, gradient-filled big text** (CHBlackjack's `Mask`: render
  glyphs to a 1 bpp mask, dilate it for the outline, fill with a per-row
  colour ramp): the title and the CHECK! and CHECKMATE! banners.

A small `CHGfxExtras.h` with these would spare the next game from
writing them again.

### 6. A 3x5 font

Both games use Press Play On Tape's 3x5 font (`text35`, with a doubled
variant) for HUDs and plates. At 128x128 it is the most legible size that
still fits a sentence on one line. Shipping a tiny 3x5 font (with credit)
alongside the GFXfont support would suit this screen better than the
larger defaults.

### 7. Palette helpers

CHChess's palette module (`src/gfx/Palette.cpp`) stages the 16 colours and
commits them after `gfx_wait()`. On top of that it does fades to black,
theme swaps (felt colours) and two "animated slots" whose colour is
recomputed each frame (a rainbow cycle, a grey pulse, a shimmer). Useful
library pieces would be a staged `gfx_setPalette` that applies at the
next flush (so it can be called any time without tearing), and a
`gfx_setFade(level)` applied while the conversion table is built.

### 8. Ship the simulator

`tools/chsim` (the host CHGfx with flush timing, the drawing-during-flush
check, a cost model calibrated against `benchmark-results.txt`, and the
`chdrive.py` script runner with screenshots and GIFs) has been copied
from CHBlackjack to CHChess. It would be better maintained once, in
`CHGfx/extras/sim`, and it is the fastest way to develop for the board.
