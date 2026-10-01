#pragma GCC optimize("Os")
#include <string.h>
#include <CHGfx.h>
#include "Mask.h"

#include "Draw.h"
#include "../RamFunc.h"

Mask maskBegin(int w, int h) {
    Mask m;
    m.w = (uint8_t)w; m.h = (uint8_t)h;
    m.stride = (uint8_t)((w + 2 + 7) >> 3);
    m.bits = gfx_chunkScratch();
    memset(m.bits, 0, (uint32_t)m.stride * (uint32_t)(h + 2));   // newlib's: word stores
    return m;
}

int text35WidthScaled(const char *s, uint8_t scale) { return text35Width(s) * scale; }

// Each font row, scaled, is one bit pattern (3 * scale bits) ORed into
// `scale` mask rows: a few byte ORs a row rather than a call per pixel.
void maskText35(Mask &m, int x, int y, const char *s, uint8_t scale, const int8_t *dy) {
    uint32_t one = (1u << scale) - 1;
    for (int k = 0; s[k]; k++, x += 4 * scale) {
        if (s[k] == '~') { x -= 2 * scale; continue; }
        int g = glyph35(s[k]);
        if (g < 0) continue;
        int top = y + (dy ? dy[k] : 0) + 1, bx = x + 1;     // + the margin
        if (bx < 0) continue;
        for (int row = 0; row < 6; row++) {
            uint32_t pat = 0;
            for (int col = 0; col < 3; col++) pat = (pat << scale) | (((FONT35[g][col] >> row) & 1) ? one : 0);
            if (!pat) continue;
            pat <<= 32 - 3 * scale - (bx & 7);
            for (int j = 0; j < scale; j++) {
                int r = top + row * scale + j;
                if ((unsigned)r >= (unsigned)(m.h + 2)) continue;
                uint8_t *p = m.bits + r * m.stride;
                for (int b = 0; b < 4 && (bx >> 3) + b < m.stride; b++) p[(bx >> 3) + b] |= (uint8_t)(pat >> (24 - 8 * b));
            }
        }
    }
}

// Paint the set bits of one mask row (stride bytes, MSB-first) at screen
// row y, bit 0 at screen column x. A mask byte is eight pixels, four
// framebuffer bytes, so the bits go two at a time (at an odd x shifted one
// along first). From SRAM: this loop is most of a banner's cost.
RAMFUNC(maskruns) static void runs(const uint8_t *row, uint8_t stride, int x, int y, uint8_t c) {
    if ((unsigned)y >= GFX_H) return;
    uint8_t *fb = gfx_fb + y * GFX_FB_STRIDE;
    uint8_t cc = (uint8_t)(c | (c << 4));
    int odd = x & 1, px = x - odd;
    uint8_t prev = 0;
    for (int i = 0; i <= stride; i++, px += 8) {
        uint8_t cur = i < stride ? row[i] : 0;
        uint8_t b = odd ? (uint8_t)((prev << 7) | (cur >> 1)) : cur;
        prev = cur;
        for (int xx = px; b; xx += 2, b = (uint8_t)(b << 2)) {
            if ((unsigned)xx >= GFX_W) continue;
            uint8_t &q = fb[xx >> 1];
            switch (b & 0xC0) {
                case 0xC0: q = cc; break;
                case 0x80: q = (uint8_t)((q & 0xF0) | c); break;          // left pixel
                case 0x40: q = (uint8_t)((q & 0x0F) | (c << 4)); break;   // right pixel
            }
        }
    }
}

// Grow row r by one pixel in all 8 directions into out. From SRAM too: it
// runs for every row (the outline and the shadow share it).
RAMFUNC(maskdilate) static void dilateRow(const Mask &m, int r, uint8_t *out) {
    int rows = m.h + 2;
    for (int b = 0; b < m.stride; b++) {
        uint8_t v = m.bits[r * m.stride + b];
        if (r > 0) v |= m.bits[(r - 1) * m.stride + b];
        if (r + 1 < rows) v |= m.bits[(r + 1) * m.stride + b];
        out[b] = v;
    }
    uint8_t carryL = 0;
    uint8_t tmp[32];
    for (int b = 0; b < m.stride; b++) tmp[b] = out[b];
    for (int b = m.stride - 1; b >= 0; b--) {           // shift left (x-1)
        uint8_t nc = (uint8_t)(tmp[b] >> 7);
        out[b] |= (uint8_t)((tmp[b] << 1) | carryL);
        carryL = nc;
    }
    uint8_t carryR = 0;
    for (int b = 0; b < m.stride; b++) {                  // shift right (x+1)
        uint8_t nc = (uint8_t)(tmp[b] << 7);
        out[b] |= (uint8_t)((tmp[b] >> 1) | carryR);
        carryR = nc;
    }
}

void maskDraw(const Mask &m, int x, int y, uint8_t outline, uint8_t shadow, const uint8_t *ramp) {
    int rows = m.h + 2;
    int ox = x - 1, oy = y - 1;                          // undo the margin
    uint8_t d[32];
    // Each grown row is painted twice: as the shadow, a row down and a pixel
    // right, then as the outline. Screen row Y still gets its shadow before
    // its outline, as if all the shadow went down first.
    for (int r = 0; r < rows; r++) {
        dilateRow(m, r, d);
        runs(d, m.stride, ox + 1, oy + r + 1, shadow);
        runs(d, m.stride, ox, oy + r, outline);
    }
    for (int r = 1; r < rows - 1; r++) runs(m.bits + r * m.stride, m.stride, ox, oy + r, ramp[r - 1]);
}
