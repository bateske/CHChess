#pragma GCC optimize("Os")
#include <CHGfx.h>
#include "Mask.h"

#include "Draw.h"

Mask maskBegin(int w, int h) {
    Mask m;
    m.w = (uint8_t)w; m.h = (uint8_t)h;
    m.stride = (uint8_t)((w + 2 + 7) >> 3);
    m.bits = gfx_chunkScratch();
    uint32_t n = (uint32_t)m.stride * (uint32_t)(h + 2);
    for (uint32_t i = 0; i < n; i++) m.bits[i] = 0;
    return m;
}

void Mask::set(int x, int y) {
    x += 1; y += 1;                                   // margin
    if ((unsigned)x >= (unsigned)(w + 2) || (unsigned)y >= (unsigned)(h + 2)) return;
    bits[y * stride + (x >> 3)] |= (uint8_t)(0x80 >> (x & 7));
}

static void plot(Mask &m, int x, int y, uint8_t scale) {
    for (int j = 0; j < scale; j++)
        for (int i = 0; i < scale; i++) m.set(x + i, y + j);
}

int text35WidthScaled(const char *s, uint8_t scale) { return text35Width(s) * scale; }

void maskText35(Mask &m, int x, int y, const char *s, uint8_t scale, const int8_t *dy) {
    for (int k = 0; s[k]; k++) {
        if (s[k] == '~') { x += 2 * scale; continue; }
        int g = glyph35(s[k]);
        int oy = y + (dy ? dy[k] : 0);
        if (g >= 0)
            for (int col = 0; col < 3; col++)
                for (int row = 0; row < 6; row++)
                    if (FONT35[g][col] & (1u << row)) plot(m, x + col * scale, oy + row * scale, scale);
        x += 4 * scale;
    }
}

void maskBlit1(Mask &m, const uint8_t *bits, int x, int y, uint8_t w, uint8_t h, uint8_t scale) {
    uint8_t stride = (uint8_t)((w + 7) >> 3);
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            if (bits[j * stride + (i >> 3)] & (0x80 >> (i & 7))) plot(m, x + i * scale, y + j * scale, scale);
}

// Paint the set runs of one mask row (stride bytes, MSB-first) at screen
// row y, where bit 0 of the row is screen column x. From SRAM, skipping
// empty and full bytes whole: this loop is most of a banner's cost.
__attribute__((section(".srodata.ramfunc.maskruns"), noinline))
static void runs(const uint8_t *row, uint8_t stride, int x, int y, uint8_t c) {
    if ((unsigned)y >= GFX_H) return;
    int n = stride * 8, i = 0, start = -1;
    while (i < n) {
        uint8_t b = row[i >> 3];
        if ((i & 7) == 0) {
            if (b == 0x00) { if (start >= 0) { gfx_hline(x + start, y, i - start, c); start = -1; } i += 8; continue; }
            if (b == 0xFF) { if (start < 0) start = i; i += 8; continue; }
        }
        if (b & (0x80 >> (i & 7))) { if (start < 0) start = i; }
        else if (start >= 0) { gfx_hline(x + start, y, i - start, c); start = -1; }
        i++;
    }
    if (start >= 0) gfx_hline(x + start, y, n - start, c);
}

// Grow row r by one pixel in all 8 directions into out.
static void dilateRow(const Mask &m, int r, uint8_t *out) {
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

void maskDraw(const Mask &m, int x, int y, uint8_t fill, int outline, int shadow, const uint8_t *ramp) {
    int rows = m.h + 2;
    int ox = x - 1, oy = y - 1;                          // undo the margin
    uint8_t d[32];
    if (shadow >= 0) {
        for (int r = 0; r < rows; r++) {
            if (outline >= 0) { dilateRow(m, r, d); runs(d, m.stride, ox + 1, oy + r + 1, (uint8_t)shadow); }
            else runs(m.bits + r * m.stride, m.stride, ox + 1, oy + r + 1, (uint8_t)shadow);
        }
    }
    if (outline >= 0)
        for (int r = 0; r < rows; r++) { dilateRow(m, r, d); runs(d, m.stride, ox, oy + r, (uint8_t)outline); }
    for (int r = 1; r < rows - 1; r++)
        runs(m.bits + r * m.stride, m.stride, ox, oy + r, ramp ? ramp[r - 1] : fill);
}
