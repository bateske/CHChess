// 1 bpp scratch masks for text and logo effects.
//
// Outlined, shadowed or gradient text drawn the naive way (the glyphs nine
// times over, pixel by pixel from flash) cost ~5 ms a line on this chip.
// Instead: render the shape once into a bit mask, grow it by one pixel with
// word-wide ORs for the outline, and paint every row as horizontal runs with
// gfx_hline's word stores.
//
// The mask lives in CHGfx's chunk buffers (gfx_chunkScratch, 1 KB), which
// are idle between gfx_wait() and the next flush - i.e. during render.
// Never keep a Mask across frames or use one outside render.
#pragma once
#include <stdint.h>

struct Mask {
    uint8_t *bits;       // MSB-first rows, 1 px margin on every side
    uint8_t stride;      // bytes per row
    uint8_t w, h;        // usable size (excluding the margin)
    void set(int x, int y);
};

Mask maskBegin(int w, int h);                   // cleared; w*h <= ~7000 px

// PPOT's 3x5 font at an integer scale (4*scale px advance). dy, if given,
// offsets each character vertically (wavy banners).
void maskText35(Mask &m, int x, int y, const char *s, uint8_t scale = 1, const int8_t *dy = nullptr);
void maskBlit1(Mask &m, const uint8_t *bits, int x, int y, uint8_t w, uint8_t h, uint8_t scale = 1);
int  text35WidthScaled(const char *s, uint8_t scale);

// Paint the mask with its top-left at (x, y). outline/shadow < 0 = none.
// ramp, if given, is a fill colour per mask row (gradient lettering).
void maskDraw(const Mask &m, int x, int y, uint8_t fill, int outline = -1, int shadow = -1,
              const uint8_t *ramp = nullptr);
