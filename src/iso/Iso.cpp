#pragma GCC optimize("Os")   // cold code: size over speed (hot span loops are RAMFUNCs)
#include <string.h>
#include <CHGfx.h>
#include "Iso.h"
#include "../gfx/Palette.h"
#include "../gfx/Draw.h"

#define RAMFUNC(name) __attribute__((section(".srodata.ramfunc." #name), noinline))

namespace iso {

Cam cam = {0, 112, false};
bool coords = true;
uint8_t darkSq = FELT, lightSq = SKIN, tableCol = NAVY, tableDot = WINE, tableShadow = INK;

void toView(uint8_t sq, bool flip, int &u, int &v) {
    int f = sq & 7, r = sq >> 3;
    if (!flip) { u = f; v = 7 - r; }
    else       { u = 7 - f; v = r; }
}

uint8_t fromView(int u, int v, bool flip) {
    int f = flip ? 7 - u : u, r = flip ? v : 7 - v;
    return (uint8_t)(r * 8 + f);
}

void screenOf(uint8_t sq, int &x, int &y) {
    int u, v;
    toView(sq, cam.flip, u, v);
    x = worldX(u, v) - cam.x + CX;
    y = worldTop(u, v) + HH - cam.y + CY;
}

// Tile rows: widths 2, 6, ... 54, 54, ... 6, 2 - every pixel whose centre
// lies inside the diamond, so neighbours meet without gaps or overlaps and
// every edge is a clean 2:1 staircase.
static inline int halfWidth(int k) { return k < HH ? 2 * k + 1 : 2 * (TH - 1 - k) + 1; }

RAMFUNC(isodiamond) void diamond(int cx, int top, uint8_t c) {
    int k0 = top < 0 ? -top : 0;
    int k1 = GFX_H - top; if (k1 > TH) k1 = TH;
    for (int k = k0; k < k1; k++) {
        int hw = halfWidth(k);
        gfx_hline(cx - hw, top + k, 2 * hw, c);
    }
}

RAMFUNC(isotint) static void tintRows(int cx, int top, int inset, uint8_t c) {
    for (int k = inset; k < TH - inset; k++) {
        int y = top + k;
        if ((unsigned)y >= GFX_H) continue;
        int hw = halfWidth(k) - 2 * inset;
        uint8_t *row = gfx_fb + y * GFX_FB_STRIDE;
        for (int x = cx - hw + ((cx - hw + y) & 1); x < cx + hw; x += 2) {
            if ((unsigned)x >= GFX_W) continue;
            uint8_t &b = row[x >> 1];
            b = (x & 1) ? (uint8_t)((b & 0x0F) | (c << 4)) : (uint8_t)((b & 0xF0) | c);
        }
    }
}

static bool tileOnScreen(uint8_t sq, int &cx, int &top);

void tileTint(uint8_t sq, uint8_t inset, uint8_t c) {
    int cx, top;
    if (tileOnScreen(sq, cx, top)) tintRows(cx, top, inset, c);
}

static bool tileOnScreen(uint8_t sq, int &cx, int &top) {
    int u, v;
    toView(sq, cam.flip, u, v);
    cx = worldX(u, v) - cam.x + CX;
    top = worldTop(u, v) - cam.y + CY;
    return cx + HW > 0 && cx - HW < GFX_W && top + TH > 0 && top < GFX_H;
}

void fillTile(uint8_t sq, uint8_t c) {
    int cx, top;
    if (tileOnScreen(sq, cx, top)) diamond(cx, top, c);
}

static inline void px(int x, int y, uint8_t c) {
    if ((unsigned)x < GFX_W && (unsigned)y < GFX_H) gfx_pixel(x, y, c);
}

void tileBorder(uint8_t sq, uint8_t inset, uint8_t c, uint8_t c2, uint8_t phase) {
    int cx, top;
    if (!tileOnScreen(sq, cx, top)) return;
    for (int k = inset; k < TH - inset; k++) {
        int y = top + k;
        if ((unsigned)y >= GFX_H) continue;
        int hw = halfWidth(k) - 2 * inset;
        // Perimeter position, clockwise from the top point, for the dashes.
        uint8_t cr = (((k + phase) >> 2) & 1) ? c2 : c;
        uint8_t cl = (((2 * TH - 1 - k + phase) >> 2) & 1) ? c2 : c;
        if (hw <= 2) { gfx_hline(cx - hw, y, 2 * hw, cr); continue; }
        gfx_hline(cx + hw - 2, y, 2, cr);
        gfx_hline(cx - hw, y, 2, cl);
    }
}

// ---------------------------------------------------------------------------
// Table and board
// ---------------------------------------------------------------------------

// Felt with a printed lattice of dots that scrolls with the camera, so pans
// read as motion even over bare table. One pattern row, copied.
void drawTable() {
    gfx_clear(tableCol);
    uint8_t row[GFX_FB_STRIDE];
    for (int phase = 0; phase < 2; phase++) {
        memset(row, (tableCol << 4) | tableCol, sizeof row);
        int ox = cam.x + phase * 8;
        for (int x = (16 - (ox & 15)) & 15; x < GFX_W; x += 16) {
            uint8_t &b = row[x >> 1];
            b = (x & 1) ? (uint8_t)((b & 0x0F) | (tableDot << 4)) : (uint8_t)((b & 0xF0) | tableDot);
        }
        for (int y = (16 - ((cam.y + phase * 8) & 15)) & 15; y < GFX_H; y += 16)
            memcpy(gfx_fb + y * GFX_FB_STRIDE, row, sizeof row);
    }
}

// The two slab faces below the near edges, row by row (see the derivation
// in the notes: the face's top boundary is exactly the tiles' edge
// staircase). left: the edge from the left corner (xl, yl) down to the
// near corner; right: from the near corner up to the right corner.
static void leftFace(int xl, int yl, int depth, int shift, uint8_t c, uint8_t trim) {
    int xr = xl + 8 * HW;                        // x of the near corner
    for (int y = yl; y < yl + 8 * HH + depth; y++) {
        if ((unsigned)y >= GFX_H) continue;
        int b = xl + 2 * (y - yl);               // last face pixel on this row
        int a = xl + 2 * (y - depth - yl) + 1;
        if (a < xl) a = xl;
        if (b > xr - 1) b = xr - 1;
        a += shift; b += shift;
        if (a > b) continue;
        gfx_hline(a, y, b - a + 1, c);
        if (trim != 0xFF && y < yl + 8 * HH) gfx_hline(b - 1, y, 2, trim);
    }
}

static void rightFace(int xb, int yb, int depth, int shift, uint8_t c, uint8_t trim) {
    int xr = xb + 8 * HW;
    int yr = yb - 8 * HH;
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

void drawBoard() {
    // Corners in screen space: left (a1 from White), near, right.
    int xl = -8 * HW - cam.x + CX, yl = 8 * HH - cam.y + CY;
    int xb = -cam.x + CX,          yb = 16 * HH - cam.y + CY;

    // Contact shadow on the felt, then the slab.
    leftFace(xl, yl, SLAB + 5, 3, tableShadow, 0xFF);
    rightFace(xb, yb, SLAB + 5, 3, tableShadow, 0xFF);
    leftFace(xl, yl, SLAB, 0, WINE, GOLD);
    rightFace(xb, yb, SLAB, 0, INK, GOLD);

    // Squares (only those on screen).
    for (uint8_t sq = 0; sq < 64; sq++) {
        int cx, top;
        if (!tileOnScreen(sq, cx, top)) continue;
        diamond(cx, top, ((sq >> 3) + sq) & 1 ? lightSq : darkSq);
    }

    // Coordinates on the rim: files along the left face, ranks along the
    // right one (reversed from Black's side).
    for (int i = 0; coords && i < 8; i++) {
        char s[2] = {0, 0};
        int x = xl + (2 * i + 1) * HW / 2 - 1, y = yl + (2 * i + 1) * HH / 2 + 1;
        if (x > -4 && x < GFX_W && y > -6 && y < GFX_H) {
            s[0] = (char)('A' + (cam.flip ? 7 - i : i));
            text35(x, y, s, GOLD);
        }
        x = xb + (2 * i + 1) * HW / 2 - 1; y = yb - (2 * i + 1) * HH / 2 + 1;
        if (x > -4 && x < GFX_W && y > -6 && y < GFX_H) {
            s[0] = (char)('1' + (cam.flip ? 7 - i : i));
            text35(x, y, s, GOLD);
        }
    }
}

}  // namespace iso
