#pragma GCC optimize("Os")
#include <string.h>
#include "Draw.h"
#include "../fx/Fx.h"

// Hot loops run from SRAM: from flash (3 wait states, no cache) a per-pixel
// loop costs ~3 us a pixel on this part. Each gets its own section so the
// unused ones are dropped at link time.
#define RAMFUNC(name) __attribute__((section(".srodata.ramfunc." #name), noinline))

static inline void plot(uint8_t *p, int x, uint8_t c) {
    if (x & 1) *p = (uint8_t)((*p & 0x0F) | (c << 4));
    else       *p = (uint8_t)((*p & 0xF0) | c);
}

// Corner insets per row for radius 1..4 (pixel-art circles, not chamfers).
static const uint8_t INSET[4][4] = { {1}, {2, 1}, {3, 1, 1}, {4, 2, 1, 1} };

void fillRound(int x, int y, int w, int h, uint8_t r, uint8_t c) {
    if (r > 4) r = 4;
    if (r * 2 > h) r = (uint8_t)(h / 2);
    const uint8_t *in = INSET[r ? r - 1 : 0];
    for (int i = 0; i < r; i++) {
        gfx_hline(x + in[i], y + i, w - 2 * in[i], c);
        gfx_hline(x + in[i], y + h - 1 - i, w - 2 * in[i], c);
    }
    gfx_fillRect(x, y + r, w, h - 2 * r, c);
}

void roundRect(int x, int y, int w, int h, uint8_t r, uint8_t c) {
    if (r > 4) r = 4;
    if (r * 2 > h) r = (uint8_t)(h / 2);
    if (!r) { gfx_rect(x, y, w, h, c); return; }
    const uint8_t *in = INSET[r - 1];
    gfx_hline(x + in[0], y, w - 2 * in[0], c);
    gfx_hline(x + in[0], y + h - 1, w - 2 * in[0], c);
    for (uint8_t i = 1; i < r; i++) {
        int len = in[i - 1] - in[i]; if (len < 1) len = 1;
        gfx_hline(x + in[i], y + i, len, c);
        gfx_hline(x + w - in[i] - len, y + i, len, c);
        gfx_hline(x + in[i], y + h - 1 - i, len, c);
        gfx_hline(x + w - in[i] - len, y + h - 1 - i, len, c);
    }
    gfx_vline(x, y + r, h - 2 * r, c);
    gfx_vline(x + w - 1, y + r, h - 2 * r, c);
}

// A run of one colour within a row: odd nibble, whole bytes, odd nibble.
static inline __attribute__((always_inline)) void fillRun(uint8_t *row, int a, int len, uint8_t c) {
    if (a < 0) { len += a; a = 0; }
    if (a + len > GFX_W) len = GFX_W - a;
    if (len <= 0) return;
    uint8_t *p = row + (a >> 1);
    if (a & 1) { *p = (uint8_t)((*p & 0x0F) | (c << 4)); p++; len--; }
    uint8_t cc = (uint8_t)(c | (c << 4));
    while (len >= 2) { *p++ = cc; len -= 2; }
    if (len) *p = (uint8_t)((*p & 0xF0) | c);
}

RAMFUNC(sprite4) void sprite4(const uint8_t *d, int x, int y, const uint8_t *remap, uint8_t flags) {
    uint8_t w = d[0], h = d[1];
    d += 2;
    bool mirror = flags & SPR_MIRROR, ghost = flags & SPR_GHOST;
    int s = (flags & SPR_2X) ? 2 : 1;           // doubled: every pixel a 2x2 block
    for (int j = 0; j < h; j++) {
        uint8_t n = *d++;
        const uint8_t *runs = d;
        d += n;
        for (int rep = 0; rep < s; rep++, y++) {
            if ((unsigned)y >= GFX_H) continue;
            uint8_t *row = gfx_fb + y * GFX_FB_STRIDE;
            // Walk the runs with a running x; mirrored, backwards from the
            // row's right end (trailing transparency is implicit).
            int q = x, step = 1;
            const uint8_t *r = runs;
            if (mirror) {
                int used = 0;
                for (uint8_t i = 0; i < n; i++) used += (runs[i] >> 4) + 1;
                q += (w - used) * s;
                r += n - 1; step = -1;
            }
            for (uint8_t i = 0; i < n; i++, r += step) {
                uint8_t b = *r;
                int len = ((b >> 4) + 1) * s;
                uint8_t c = b & 15;
                if (c != 15) {
                    c = remap[c];
                    if (!ghost) fillRun(row, q, len, c);
                    else {
                        for (int k = q + ((q + y) & 1); k < q + len; k += 2)
                            if ((unsigned)k < GFX_W) plot(row + (k >> 1), k, c);
                    }
                }
                q += len;
            }
        }
    }
}

RAMFUNC(spriterot) static void rotSpan(const uint8_t *src, int sw, int sh, int x0, int x1, int y,
                                       int32_t u, int32_t v, int32_t du, int32_t dv, const uint8_t *remap) {
    uint8_t *row = gfx_fb + y * GFX_FB_STRIDE;
    for (int x = x0; x < x1; x++, u += du, v += dv) {
        int sx = u >> 16, sy = v >> 16;
        if ((unsigned)sx >= (unsigned)sw || (unsigned)sy >= (unsigned)sh) continue;
        uint8_t b = src[sy * ((sw + 1) >> 1) + (sx >> 1)];
        uint8_t c = (sx & 1) ? (uint8_t)(b >> 4) : (uint8_t)(b & 15);
        if (c != 15) plot(row + (x >> 1), x, remap[c]);
    }
}

void spriteRot(const uint8_t *d, int ax, int ay, int px, int py, uint8_t angle, int scale,
               const uint8_t *remap, bool mirror) {
    int w = d[0], h = d[1], stride = (w + 1) >> 1;
    if (stride * h > 1024 || scale <= 0) return;
    // Decode to raw 4 bpp (15 = transparent), mirrored if asked.
    uint8_t *buf = gfx_chunkScratch();
    for (int i = 0; i < stride * h; i++) buf[i] = 0xFF;
    const uint8_t *p = d + 2;
    for (int j = 0; j < h; j++) {
        uint8_t n = *p++;
        int x = 0;
        while (n--) {
            uint8_t b = *p++;
            int len = (b >> 4) + 1;
            uint8_t c = b & 15;
            for (int k = 0; k < len; k++, x++) {
                int xx = mirror ? w - 1 - x : x;
                uint8_t &q = buf[j * stride + (xx >> 1)];
                q = (xx & 1) ? (uint8_t)((q & 0x0F) | (c << 4)) : (uint8_t)((q & 0xF0) | c);
            }
        }
    }
    if (mirror) ax = w - 1 - ax;
    // Inverse map: screen offset (dx, dy) from the pivot -> source pixel.
    int cs = fx::isin(angle + 64), sn = fx::isin(angle);            // Q8
    int32_t ic = (int32_t)cs * 256 / scale, is = (int32_t)sn * 256 / scale;   // Q8, divided by scale
    // Bounding box: the pivot's farthest corner distance, scaled.
    int r = 0;
    int cx[4] = {-ax, w - ax, -ax, w - ax}, cy[4] = {-ay, -ay, h - ay, h - ay};
    for (int k = 0; k < 4; k++) { int m = (cx[k] < 0 ? -cx[k] : cx[k]) + (cy[k] < 0 ? -cy[k] : cy[k]); if (m > r) r = m; }
    r = r * scale / 256 + 1;
    int y0 = py - r, y1 = py + r, x0 = px - r, x1 = px + r;
    if (y0 < 0) y0 = 0;
    if (y1 > GFX_H) y1 = GFX_H;
    if (x0 < 0) x0 = 0;
    if (x1 > GFX_W) x1 = GFX_W;
    for (int y = y0; y < y1; y++) {
        int dy = y - py, dx = x0 - px;
        // src = R(-angle) * (dx, dy) / scale + pivot, in 16.16
        int32_t u = ((ic * dx + is * dy) << 8) + ((int32_t)ax << 16) + 0x8000;
        int32_t v = ((-is * dx + ic * dy) << 8) + ((int32_t)ay << 16) + 0x8000;
        rotSpan(buf, w, h, x0, x1, y, u, v, ic << 8, -is << 8, remap);
    }
}

// ---------------------------------------------------------------------------
// Shapes and effects
// ---------------------------------------------------------------------------
static int isqrt(int v) {
    if (v <= 0) return 0;
    int r = 0, bit = 1 << 14;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return r;
}

// Half-widths per row for the few ellipse sizes in use (bet circle, chips),
// computed once: the integer square roots cost ~10 us a row from flash.
struct EllipseRows { uint8_t rx, ry, dx[16]; };
static EllipseRows ecache[4];

static const uint8_t *ellipseRows(int rx, int ry) {
    for (auto &e : ecache) if (e.rx == rx && e.ry == ry) return e.dx;
    static uint8_t next = 0;
    EllipseRows &e = ecache[next++ & 3];
    e.rx = (uint8_t)rx; e.ry = (uint8_t)ry;
    for (int dy = 0; dy <= ry && dy < 16; dy++) {
        int t = ry * ry - dy * dy + ry / 2;
        e.dx[dy] = (uint8_t)(ry > 0 ? isqrt(rx * rx * t / (ry * ry)) : rx);
    }
    return e.dx;
}

void fillEllipse(int cx, int cy, int rx, int ry, uint8_t c) {
    const uint8_t *dx = ellipseRows(rx, ry);
    for (int dy = -ry; dy <= ry; dy++) {
        int d = dx[dy < 0 ? -dy : dy];
        gfx_hline(cx - d, cy + dy, 2 * d + 1, c);
    }
}

void ellipse(int cx, int cy, int rx, int ry, uint8_t c) {
    const uint8_t *dx = ellipseRows(rx, ry);
    for (int dy = -ry; dy <= ry; dy++) {
        int a = dy < 0 ? -dy : dy;
        int d = dx[a];
        int inner = (a == ry) ? -d - 1 : dx[a + 1];
        int len = d - inner; if (len < 1) len = 1;
        gfx_hline(cx + d - len + 1, cy + dy, len, c);
        gfx_hline(cx - d, cy + dy, len, c);
    }
}

void dither(int x, int y, int w, int h, uint8_t c, uint8_t phase) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > GFX_W) w = GFX_W - x;
    if (y + h > GFX_H) h = GFX_H - y;
    if (w <= 0 || h <= 0) return;
    uint8_t cc = (uint8_t)(c | (c << 4));
    for (int j = 0; j < h; j++) {
        int yy = y + j;
        uint8_t *row = gfx_fb + yy * GFX_FB_STRIDE;
        // Pixels where (px + yy + phase) is even get the colour.
        uint8_t m = ((yy + phase) & 1) ? 0xF0 : 0x0F;
        int i = x;
        if (i & 1) { if (m == 0xF0) row[i >> 1] = (uint8_t)((row[i >> 1] & 0x0F) | (c << 4)); i++; }
        for (; i + 1 < x + w; i += 2) row[i >> 1] = (uint8_t)((row[i >> 1] & ~m) | (cc & m));
        if (i < x + w && m == 0x0F) row[i >> 1] = (uint8_t)((row[i >> 1] & 0xF0) | c);
    }
}

void remapRect(int x, int y, int w, int h, const uint8_t *m) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > GFX_W) w = GFX_W - x;
    if (y + h > GFX_H) h = GFX_H - y;
    if (w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++) {
        uint8_t *row = gfx_fb + (y + j) * GFX_FB_STRIDE;
        int i = x;
        if (i & 1) { uint8_t b = row[i >> 1]; row[i >> 1] = (uint8_t)((b & 0x0F) | (m[b >> 4] << 4)); i++; }
        for (; i + 1 < x + w; i += 2) { uint8_t b = row[i >> 1]; row[i >> 1] = (uint8_t)(m[b & 15] | (m[b >> 4] << 4)); }
        if (i < x + w) { uint8_t b = row[i >> 1]; row[i >> 1] = (uint8_t)((b & 0xF0) | m[b & 15]); }
    }
}

// ---------------------------------------------------------------------------
// PPOT Font3x5 (Press Play On Tape, Apache-2.0). Column bytes, bit 0 = top,
// bit 5 = descender. Extended here with $ , / ' * ( ) < > = % #.
// ---------------------------------------------------------------------------
const uint8_t FONT35[][3] = {
    {0x1F,0x05,0x1F},{0x1F,0x15,0x1B},{0x1F,0x11,0x11},{0x1F,0x11,0x0E},{0x1F,0x15,0x11},  // A-E
    {0x1F,0x05,0x01},{0x1F,0x11,0x1D},{0x1F,0x04,0x1F},{0x00,0x1F,0x00},{0x10,0x10,0x1F},  // F-J
    {0x1F,0x04,0x1B},{0x1F,0x10,0x10},{0x1F,0x06,0x1F},{0x1F,0x01,0x1F},{0x1F,0x11,0x1F},  // K-O
    {0x1F,0x05,0x07},{0x1F,0x31,0x1F},{0x1F,0x05,0x1B},{0x17,0x15,0x1D},{0x01,0x1F,0x01},  // P-T
    {0x1F,0x10,0x1F},{0x0F,0x10,0x0F},{0x1F,0x0C,0x1F},{0x1B,0x04,0x1B},{0x07,0x1C,0x07},  // U-Y
    {0x19,0x15,0x13},                                                                       // Z
    {0x0C,0x12,0x1E},{0x1F,0x12,0x0C},{0x1E,0x12,0x12},{0x0C,0x12,0x1F},{0x0C,0x1A,0x14},  // a-e
    {0x04,0x1F,0x05},{0x2E,0x2A,0x1E},{0x1F,0x02,0x1C},{0x00,0x1D,0x00},{0x20,0x1D,0x00},  // f-j
    {0x1F,0x04,0x1A},{0x01,0x1F,0x00},{0x1E,0x04,0x1E},{0x1E,0x02,0x1E},{0x1E,0x12,0x1E},  // k-o
    {0x3E,0x12,0x0C},{0x0C,0x12,0x3E},{0x1E,0x02,0x06},{0x14,0x12,0x0A},{0x02,0x0F,0x12},  // p-t
    {0x1E,0x10,0x1E},{0x0E,0x10,0x0E},{0x1E,0x08,0x1E},{0x1A,0x04,0x1A},{0x2E,0x28,0x1E},  // u-y
    {0x1A,0x12,0x16},                                                                       // z
    {0x1F,0x11,0x1F},{0x12,0x1F,0x10},{0x1D,0x15,0x17},{0x11,0x15,0x1F},{0x07,0x04,0x1F},  // 0-4
    {0x17,0x15,0x1D},{0x1F,0x15,0x1D},{0x01,0x01,0x1F},{0x1F,0x15,0x1F},{0x17,0x15,0x1F},  // 5-9
    {0x00,0x17,0x00},{0x00,0x10,0x00},{0x04,0x04,0x04},{0x04,0x0E,0x04},{0x02,0x29,0x06},  // ! . - + ?
    {0x0A,0x00,0x00},                                                                       // :
    {0x12,0x1F,0x09},{0x20,0x10,0x00},{0x18,0x06,0x01},{0x00,0x03,0x00},{0x0A,0x04,0x0A},  // $ , / ' *
    {0x00,0x0E,0x11},{0x11,0x0E,0x00},{0x04,0x0A,0x11},{0x11,0x0A,0x04},{0x0A,0x0A,0x0A},  // ( ) < > =
    {0x19,0x04,0x13},{0x1F,0x0A,0x1F},                                                      // % #
};

// ASCII 32..122 -> FONT35 index, -1 = no glyph.
static const int8_t IDX35[91] = {
    -1, 62, -1, 79, 68, 78, -1, 71, 73, 74, 72, 65, 69, 64, 63, 70,   // space ! " # $ % & ' ( ) * + , - . /
    52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 67, -1, 75, 77, 76, 66,   // 0-9 : ; < = > ?
    -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,            // @ A-O
    15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1, -1,  // P-Z and five symbols
    -1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,  // backtick a-o
    41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51,                       // p-z
};

int glyph35(char ch) { return (ch >= 32 && ch <= 122) ? IDX35[ch - 32] : -1; }

// Column-major glyph, bit 0 = top row: the layout of both PPOT's font and
// CHGfx's built-in one.
RAMFUNC(glyph) void glyph(int x, int y, const uint8_t *cols, uint8_t ncols, uint8_t c) {
    c &= 0x0F;
    for (uint8_t i = 0; i < ncols; i++, x++) {
        uint8_t bits = cols[i];
        if (!bits || (unsigned)x >= GFX_W) continue;
        int yy = y;
        uint8_t *p = gfx_fb + yy * GFX_FB_STRIDE + (x >> 1);
        for (; bits; bits >>= 1, yy++, p += GFX_FB_STRIDE)
            if ((bits & 1) && (unsigned)yy < GFX_H) plot(p, x, c);
    }
}

RAMFUNC(text35) int text35(int x, int y, const char *str, uint8_t c) {
    int x0 = x;
    for (; *str; str++) {
        char ch = *str;
        if (ch == '\n') { x = x0; y += 7; continue; }
        if (ch == '~') { x += 2; continue; }
        int g = (ch >= 32 && ch <= 122) ? IDX35[ch - 32] : -1;
        if (g >= 0) glyph(x, y, FONT35[g], 3, c);
        x += 4;
    }
    return x - x0;
}

// Each font pixel as a 2x2 block: two rows of one byte (even x) or two
// nibble pairs (odd x).
RAMFUNC(text35x2) void text35x2(int x, int y, const char *str, uint8_t c) {
    c &= 0x0F;
    int x0 = x;
    for (; *str; str++) {
        char ch = *str;
        if (ch == '\n') { x = x0; y += 14; continue; }
        if (ch == '~') { x += 4; continue; }
        int g = (ch >= 32 && ch <= 122) ? IDX35[ch - 32] : -1;
        if (g >= 0) {
            for (int col = 0; col < 3; col++) {
                uint8_t bits = FONT35[g][col];
                int px = x + col * 2;
                if ((unsigned)px > GFX_W - 2) continue;
                for (int row = 0; bits; row++, bits >>= 1) {
                    if (!(bits & 1)) continue;
                    int py = y + row * 2;
                    if ((unsigned)py > GFX_H - 2) continue;
                    uint8_t *p = gfx_fb + py * GFX_FB_STRIDE + (px >> 1);
                    for (int k = 0; k < 2; k++, p += GFX_FB_STRIDE) {
                        if (!(px & 1)) *p = (uint8_t)(c | (c << 4));
                        else { p[0] = (uint8_t)((p[0] & 0x0F) | (c << 4)); p[1] = (uint8_t)((p[1] & 0xF0) | c); }
                    }
                }
            }
        }
        x += 8;
    }
}

int text35Width(const char *str) {
    int w = 0, best = 0;
    for (; *str; str++) {
        if (*str == '\n') { if (w > best) best = w; w = 0; }
        else w += (*str == '~') ? 2 : 4;
    }
    if (w > best) best = w;
    return best ? best - 1 : 0;
}
