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

## On CHGfx 1.3.0: shapes, sprites and row operations

CHGfx 1.3.0 took up suggestions 2 to 5. Each of its primitives was swapped
in on its own and the simulator screenshots were compared with 1.2.0's.
The sizes below are the final release build (`opt=oslto`, Upload only,
48,628 B flash and 17,872 B SRAM) with that one item switched. Where
CHGfx's version did the same job in fewer bytes the game's copy went;
where it cost more, the game kept its own:

| Primitive | CHGfx's version instead of the game's | Drawn by |
|---|---|---|
| Filled ellipse (piece shadows) | -84 B flash, -80 B SRAM (no row cache) | `gfx_fillEllipse` |
| Pattern row into the board (`copyRow`) | +36 B flash, -16 B SRAM; one more call into flash per board row, about 0.2 ms more per board redraw (estimated, ~99 rows) | the game |
| Rounded rects | +124 B flash (it works out the corner insets with a square root on every call; the game reads a 16 B table) | the game |
| Dither | +8 B flash, +176 B SRAM (its span loop runs from SRAM) | the game |
| `sprite4` | +244 B flash, +160 B SRAM (1:1 and scaled loops and `gfx__span`, all in SRAM) | the game |
| `spriteRot` | +220 B flash, -16 B SRAM (its own Q14 sine table and `isqrt`) | the game |
| Screen shake (`gfx_scroll`) | +164 B flash | the game |

All of these draw the same pixels as the game's, except
`gfx_sprite4Rot` (a Q14 sine: the toppled king and the tumbling captured
piece move by a pixel here and there) and `gfx_scroll` (rows and columns
the shake uncovers keep their old pixels). Taking the six the game kept
would cost 808 B of flash and 320 B of SRAM (49,436 / 18,192 B, above the
18,072 B of SRAM the unchanged game used on 1.3). Each of them is bigger
than the game's own; of the six, only the dither would run faster from
CHGfx, and that costs 176 B of SRAM, the scarcer budget here. The
dithered square highlight now uses the game's dither too; it used to
call a flash helper per pixel from SRAM.

The game's own versions also got faster while they were being compared,
with the same pixels:

* `dither` paints a word (eight pixels) at a time between ragged ends;
  the title screen dithers about 5 KB a frame.
* `spriteRot` visits only the box round the rotated sprite, not a square
  of side twice its farthest corner: the fallen king at full zoom goes
  from 13,924 visited pixels a frame to 1,705.
* `maskDraw` grows each row of a banner once, not twice (the shadow and
  the outline share it), and the title's mask is as wide as its
  lettering, not 124 px.
* `sprite4` doubles the 15 remapped colours once a call instead of once
  a run; the board's pattern rows (`iso::patSpan`) are written a byte at
  a time instead of a pixel at a time; the mask and rotation buffers are
  cleared with newlib's `memset` (word stores).

Together with a few small trims (the map border plots with `gfx_pixel`,
already linked; `pal::init` uses `memcpy`), they made the release build
60 B smaller and 16 B lighter in SRAM. Against 1.2.0, the simulator's
render cost (a host proxy, not a board measurement) is 10% lower on the
showcase script, 12% on the title screen and 19% on the checkmate
screen.

On the board (debug builds, 1.2.0 against this one): a full redraw
(`tools/scripts/device_render.txt`) went from 6.4 to 6.0 ms in the normal
view, 6.1 to 5.6 ms on the map and 8.6 to 7.8 ms zoomed in on a move;
the whip-zoom move's frames from 6.6 to 6.1 ms on average (9.5 to 8.6 ms
at worst); and GRANDMASTER, drawing its bursts and bobs while it thinks
(`device_think.txt`), from 9.5-9.7 s to 9.2-9.3 s a move, mostly the
cheaper 1.3 flush.

## On CHGfx 1.3.0: text, banners and the palette

CHGfx 1.3.0 also took up suggestions 6 and 7. `CHGfx_Tiny3x5` is the
game's 3x5 font, pixel for pixel: all 80 of the game's glyphs, capitals
and lower case, are the same, and the 14 the library added (`"` `&` `;`
`@` `[` `\` `]` `^` `_` `` ` `` `{` `|` `}` `~`) are characters the game
never draws. With `gfx_text(x, y + 5)`, `gfx_textScaled(x, y + 10, ..., 2)`
and `gfx_textFx` at the baseline (`y + 5 * scale`, the same ramp and wave
offsets), every frame of the eight test scripts and of the title, setup and
options screens came out identical. It does not fit, though (release build,
flash / SRAM against the game's own text code, measured before the
speed-ups above):

| Text drawn by CHGfx | Flash | SRAM |
|---|---:|---:|
| HUD, plates and menus (`gfx_text`, `gfx_textScaled`) | +2,000 B | -64 B |
| Banners and titles (`gfx_textFx`) | +2,144 B | +72 B |
| All of it (the game's font and `Mask.cpp` deleted) | +2,260 B | -8 B |

Each of these overflows the 50,944-byte application region, so the game
keeps its font, `text35`, `text35x2` and `Mask.cpp`. CHGfx's text costs
4.3 KB where the game's costs 1.9 KB. Three things make the difference:

* The built-in 5x7 font (475 B) and its renderer (264 B, in SRAM) are
  linked even though the game would never draw with them, because
  `gfx_textScaled` and `gfx_textFx` pick the font at run time. A build
  switch that leaves the built-in font out once a GFXfont is in use would
  save those 739 B.
* `CHGfx_Tiny3x5` takes 952 B, not the 857 B its header says: a
  `GFXglyph` is padded to 8 bytes on this core, so the glyph table is
  760 B. The game's `FONT35` and its index take 331 B.
* `gfx_textFx` is 1,014 B of flash plus 462 B of SRAM functions, where
  `Mask.cpp` is 502 B plus 408 B. The scaled GFXfont path also fills a
  rectangle per run from flash: menu screens rendered about 7% slower in
  the simulator.

The palette module stays too (themes, cycling, fades and the flashes are
the game's), but CHGfx 1.3 stages `gfx_setPalette` and rebuilds its
conversion table when the next flush starts, so a commit no longer has to
follow `gfx_wait()`. The module used to keep a copy of the last palette it
sent, to skip commits that changed nothing. It now notices in `pal::tick()`
whether a cycling colour really moved: the same palettes reach the panel
on the same frames, for 44 B less flash and 32 B less SRAM. A late frame
that runs several ticks can move a cycling colour and back; it now
commits that palette unchanged, which costs CHGfx one extra conversion
table rebuild (0 to 10 times per test script). `gfx_setFade` in place of the game's own fade would cost about
150 B more, as it brings in the fade arithmetic that LTO otherwise drops,
so the game still fades its RGB444 colours itself.
