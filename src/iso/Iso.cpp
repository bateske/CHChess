#pragma GCC optimize("Os")   // cold code: size over speed (hot span loops are RAMFUNCs)
#include <string.h>
#include <CHGfx.h>
#include "Iso.h"
#include "../gfx/Palette.h"
#include "../gfx/Draw.h"

#define RAMFUNC(name) __attribute__((section(".srodata.ramfunc." #name), noinline))

namespace iso {

Cam cam = {0, 40, false};
uint8_t zoom = 1;
bool flat;
bool coords = true;
uint8_t darkSq = FELT, lightSq = SKIN, tableCol = NAVY, tableDot = WINE, tableShadow = INK;

void setView(bool m, uint8_t z) { flat = m; zoom = z; }

void toView(uint8_t sq, bool flip, int &u, int &v) {
    int f = sq & 7, r = sq >> 3;
    if (!flip) { u = f; v = 7 - r; }
    else       { u = 7 - f; v = r; }
}

uint8_t fromView(int u, int v, bool flip) {
    int f = flip ? 7 - u : u, r = flip ? v : 7 - v;
    return (uint8_t)(r * 8 + f);
}

void worldOf(uint8_t sq, int &x, int &y) {
    int u, v;
    toView(sq, cam.flip, u, v);
    if (flat) { x = MX + u * MW + MW / 2; y = MY + v * MH + MH - 3; return; }
    x = worldX(u, v);
    y = worldTop(u, v) + hh();
}

void screenOf(uint8_t sq, int &x, int &y) {
    worldOf(sq, x, y);
    x = toScreenX(x);
    y = toScreenY(y);
}

// ---------------------------------------------------------------------------
// Squares. Iso tiles: rows 2, 6, 10, ... 2 wide - every pixel whose centre
// lies inside the diamond, so neighbours meet without gaps or overlaps and
// every edge is a clean 2:1 staircase. Map squares: rectangles.
// ---------------------------------------------------------------------------
static inline int halfWidth(int k, int th) { return k < th / 2 ? 2 * k + 1 : 2 * (th - 1 - k) + 1; }

RAMFUNC(isodiamond) static void diamond(int cx, int top, int th, uint8_t c) {
    int k0 = top < 0 ? -top : 0;
    int k1 = GFX_H - top; if (k1 > th) k1 = th;
    for (int k = k0; k < k1; k++) {
        int w = halfWidth(k, th);
        gfx_hline(cx - w, top + k, 2 * w, c);
    }
}

// The square's box on screen: iso (cx = centre x, top = top row, rows = th)
// or map (x, y = top-left). False if off screen.
static bool tileOnScreen(uint8_t sq, int &cx, int &top) {
    int u, v;
    toView(sq, cam.flip, u, v);
    if (flat) { cx = MX + u * MW; top = MY + v * MH; return true; }
    cx = worldX(u, v) - cam.x + CX;
    top = worldTop(u, v) - cam.y + CY;
    return cx + hw() > 0 && cx - hw() < GFX_W && top + 2 * hh() > 0 && top < GFX_H;
}

static inline void nib(int x, int y, uint8_t c) {
    if ((unsigned)x >= GFX_W || (unsigned)y >= GFX_H) return;
    uint8_t &b = gfx_fb[y * GFX_FB_STRIDE + (x >> 1)];
    b = (x & 1) ? (uint8_t)((b & 0x0F) | (c << 4)) : (uint8_t)((b & 0xF0) | c);
}

RAMFUNC(isotint) static void tintSpan(int x0, int x1, int y, uint8_t c) {
    for (int x = x0 + ((x0 + y) & 1); x < x1; x += 2) nib(x, y, c);
}

void tileTint(uint8_t sq, uint8_t inset, uint8_t c) {
    int cx, top;
    if (!tileOnScreen(sq, cx, top)) return;
    if (flat) {
        for (int y = top + inset; y < top + MH - inset; y++) tintSpan(cx + inset, cx + MW - inset, y, c);
        return;
    }
    int th = 2 * hh();
    for (int k = inset; k < th - inset; k++) {
        int w = halfWidth(k, th) - 2 * inset;
        tintSpan(cx - w, cx + w, top + k, c);
    }
}

void tileBorder(uint8_t sq, uint8_t inset, uint8_t c, uint8_t c2, uint8_t phase) {
    int cx, top;
    if (!tileOnScreen(sq, cx, top)) return;
    if (flat) {
        // Clockwise round the rectangle, dashes of 3.
        int x0 = cx + inset, y0 = top + inset, w = MW - 2 * inset, h = MH - 2 * inset;
        int p = 0;
        for (int i = 0; i < w; i++, p++) nib(x0 + i, y0, ((p + phase) / 3) & 1 ? c2 : c);
        for (int i = 1; i < h; i++, p++) nib(x0 + w - 1, y0 + i, ((p + phase) / 3) & 1 ? c2 : c);
        for (int i = w - 2; i >= 0; i--, p++) nib(x0 + i, y0 + h - 1, ((p + phase) / 3) & 1 ? c2 : c);
        for (int i = h - 2; i > 0; i--, p++) nib(x0, y0 + i, ((p + phase) / 3) & 1 ? c2 : c);
        return;
    }
    int th = 2 * hh();
    for (int k = inset; k < th - inset; k++) {
        int y = top + k;
        if ((unsigned)y >= GFX_H) continue;
        int w = halfWidth(k, th) - 2 * inset;
        // Perimeter position, clockwise from the top point, for the dashes.
        uint8_t cr = (((k + phase) >> 1) & 1) ? c2 : c;
        uint8_t cl = (((2 * th - 1 - k + phase) >> 1) & 1) ? c2 : c;
        if (w <= 2) { gfx_hline(cx - w, y, 2 * w, cr); continue; }
        gfx_hline(cx + w - 2, y, 2, cr);
        gfx_hline(cx - w, y, 2, cl);
    }
}

// ---------------------------------------------------------------------------
// Table and board
// ---------------------------------------------------------------------------

// A casino carpet: a lattice of dots that scrolls with the camera, so pans
// read as motion. One pattern row, copied.
void drawTable() {
    gfx_clear(tableCol);
    uint8_t row[GFX_FB_STRIDE];
    int camx = flat ? 0 : cam.x, camy = flat ? 0 : cam.y;
    for (int phase = 0; phase < 2; phase++) {
        memset(row, (tableCol << 4) | tableCol, sizeof row);
        int ox = camx + phase * 8;
        for (int x = (16 - (ox & 15)) & 15; x < GFX_W; x += 16) {
            uint8_t &b = row[x >> 1];
            b = (x & 1) ? (uint8_t)((b & 0x0F) | (tableDot << 4)) : (uint8_t)((b & 0xF0) | tableDot);
        }
        for (int y = (16 - ((camy + phase * 8) & 15)) & 15; y < GFX_H; y += 16)
            memcpy(gfx_fb + y * GFX_FB_STRIDE, row, sizeof row);
    }
}

// The two slab faces below the near edges, row by row: the face's top
// boundary is exactly the tiles' edge staircase. left: the edge from the
// left corner (xl, yl) down to the near corner; right: from the near corner
// up to the right corner.
static void leftFace(int xl, int yl, int depth, int shift, uint8_t c, uint8_t trim) {
    int xr = xl + 8 * hw();                      // x of the near corner
    for (int y = yl; y < yl + 8 * hh() + depth; y++) {
        if ((unsigned)y >= GFX_H) continue;
        int b = xl + 2 * (y - yl);               // last face pixel on this row
        int a = xl + 2 * (y - depth - yl) + 1;
        if (a < xl) a = xl;
        if (b > xr - 1) b = xr - 1;
        a += shift; b += shift;
        if (a > b) continue;
        gfx_hline(a, y, b - a + 1, c);
        if (trim != 0xFF && y < yl + 8 * hh()) gfx_hline(b - 1, y, 2, trim);
    }
}

static void rightFace(int xb, int yb, int depth, int shift, uint8_t c, uint8_t trim) {
    int xr = xb + 8 * hw();
    int yr = yb - 8 * hh();
    for (int y = yr; y < yb + depth; y++) {
        if ((unsigned)y >= GFX_H) continue;
        int a = xb + 2 * (yb - y) - 1;           // first face pixel on this row
        int b = xb + 2 * (yb - y + depth) - 2;
        if (a < xb) a = xb;
        if (b > xr - 1) b = xr - 1;
        a += shift; b += shift;
        if (a > b) continue;
        gfx_hline(a, y, b - a + 1, c);
        if (trim != 0xFF && y >= yr + 1 && y <= yb) gfx_hline(a, y, 2, trim);
    }
}

static void label(int x, int y, char ch) {
    char s[2] = {ch, 0};
    if (x > -4 && x < GFX_W && y > -6 && y < GFX_H) text35(x, y, s, GOLD);
}

void drawBoard() {
    if (flat) {
        gfx_rect(MX - 2, MY - 2, 8 * MW + 4, 8 * MH + 4, GOLD);
        gfx_rect(MX - 1, MY - 1, 8 * MW + 2, 8 * MH + 2, INK);
        for (uint8_t sq = 0; sq < 64; sq++) {
            int x, y;
            tileOnScreen(sq, x, y);
            gfx_fillRect(x, y, MW, MH, ((sq >> 3) + sq) & 1 ? lightSq : darkSq);
        }
        for (int i = 0; coords && i < 8; i++) {
            label(MX + i * MW + MW / 2 - 1, MY + 8 * MH + 3, (char)('A' + (cam.flip ? 7 - i : i)));
            label(MX - 8, MY + i * MH + MH / 2 - 2, (char)('8' - (cam.flip ? 7 - i : i)));
        }
        return;
    }
    // Corners in screen space: left (a1 from White), near, right.
    int xl = -8 * hw() - cam.x + CX, yl = 8 * hh() - cam.y + CY;
    int xb = -cam.x + CX,            yb = 16 * hh() - cam.y + CY;
    int s = slab();

    // Contact shadow on the carpet, then the slab.
    leftFace(xl, yl, s + 2 + zoom, 1 + zoom, tableShadow, 0xFF);
    rightFace(xb, yb, s + 2 + zoom, 1 + zoom, tableShadow, 0xFF);
    leftFace(xl, yl, s, 0, WINE, GOLD);
    rightFace(xb, yb, s, 0, INK, GOLD);

    // Squares (only those on screen).
    int th = 2 * hh();
    for (uint8_t sq = 0; sq < 64; sq++) {
        int cx, top;
        if (!tileOnScreen(sq, cx, top)) continue;
        diamond(cx, top, th, ((sq >> 3) + sq) & 1 ? lightSq : darkSq);
    }

    // Coordinates on the carpet below the near edges: files along the left
    // one, ranks along the right (reversed from Black's side).
    for (int i = 0; coords && i < 8; i++) {
        int dx = (2 * i + 1) * hw() / 2, dy = (2 * i + 1) * hh() / 2 + s + 2 + zoom;
        label(xl + dx - 4, yl + dy, (char)('A' + (cam.flip ? 7 - i : i)));
        label(xb + dx + 2, yb - (2 * i + 1) * hh() / 2 + s + 2 + zoom, (char)('1' + (cam.flip ? 7 - i : i)));
    }
}

}  // namespace iso
