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

// Pixels [a, b) of a pattern row into screen row y: ragged nibble ends,
// word copies between (both rows are word aligned).
RAMFUNC(isocopy) static void copyRow(int y, const uint8_t *src, int a, int b) {
    if (a < 0) a = 0;
    if (b > GFX_W) b = GFX_W;
    if ((unsigned)y >= GFX_H || a >= b) return;
    uint8_t *dst = gfx_fb + y * GFX_FB_STRIDE;
    if (a & 1) { dst[a >> 1] = (uint8_t)((dst[a >> 1] & 0x0F) | (src[a >> 1] & 0xF0)); a++; }
    if (b & 1) { b--; dst[b >> 1] = (uint8_t)((dst[b >> 1] & 0xF0) | (src[b >> 1] & 0x0F)); }
    int i = a >> 1, e = b >> 1;
    while (i < e && (i & 3)) { dst[i] = src[i]; i++; }
    for (; i + 4 <= e; i += 4) *(uint32_t *)(dst + i) = *(const uint32_t *)(src + i);
    for (; i < e; i++) dst[i] = src[i];
}

// A span of colour c in a pattern row, clipped to the screen.
RAMFUNC(isopatspan) static void patSpan(uint8_t *row, int a, int b, uint8_t c) {
    if (a < 0) a = 0;
    if (b > GFX_W) b = GFX_W;
    for (; a < b; a++) {
        uint8_t &p = row[a >> 1];
        p = (a & 1) ? (uint8_t)((p & 0x0F) | (c << 4)) : (uint8_t)((p & 0xF0) | c);
    }
}

// The iso squares. The board is one big diamond (top corner x0, y0), and
// each of its rows is a periodic pattern: a light span centred every tile
// width, dark between, as wide as the tile row it cuts. The pattern only
// depends on the row within a tile, so each of the th patterns is built
// once and copied into the eight board rows it appears in - a few word
// copies a row instead of a span per square.
RAMFUNC(isosquares) static void isoSquares(int x0, int y0, int hw, int th, uint8_t light, uint8_t dark) {
    uint32_t pat[GFX_FB_STRIDE / 4];
    uint8_t *pb = (uint8_t *)pat;
    int rows = 8 * th;
    for (int r = 0; r < th; r++) {
        bool built = false;
        for (int ry = r; ry < rows; ry += th) {
            int y = y0 + ry;
            if ((unsigned)y >= GFX_H) continue;
            if (!built) {
                built = true;
                for (int i = 0; i < GFX_FB_STRIDE / 4; i++) pat[i] = dark * 0x11111111u;
                int wl = halfWidth(r, th), c = x0;
                while (c - wl > 0) c -= 2 * hw;
                for (; c - wl < GFX_W; c += 2 * hw) patSpan(pb, c - wl, c + wl, light);
            }
            int w = halfWidth(ry, rows);
            copyRow(y, pb, x0 - w, x0 + w);
        }
    }
}

// The map's squares: a pattern row per rank parity, copied into its rows.
RAMFUNC(mapsquares) static void mapSquares(uint8_t light, uint8_t dark) {
    uint32_t pat[GFX_FB_STRIDE / 4];
    uint8_t *pb = (uint8_t *)pat;
    for (int v = 0; v < 8; v++) {
        for (int u = 0; u < 8; u++)
            patSpan(pb, MX + u * MW, MX + (u + 1) * MW, (u + v) & 1 ? dark : light);
        for (int k = 0; k < MH; k++) copyRow(MY + v * MH + k, pb, MX, MX + 8 * MW);
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
    int rows = flat ? MH : 2 * hh();
    for (int k = inset; k < rows - inset; k++) {
        if (flat) tintSpan(cx + inset, cx + MW - inset, top + k, c);
        else {
            int w = halfWidth(k, rows) - 2 * inset;
            tintSpan(cx - w, cx + w, top + k, c);
        }
    }
}

void tileBorder(uint8_t sq, uint8_t inset, uint8_t c, uint8_t c2, uint8_t phase) {
    int cx, top;
    if (!tileOnScreen(sq, cx, top)) return;
    if (flat) {
        // Clockwise round the rectangle from its top-left corner, dashes of 3.
        int x = cx + inset, y = top + inset, p = phase;
        for (int side = 0; side < 4; side++) {
            int n = (side & 1) ? MH - 2 * inset - 1 : MW - 2 * inset - 1;
            for (int i = 0; i < n; i++, p++) {
                nib(x, y, (p / 3) & 1 ? c2 : c);
                if (side & 1) y += side == 1 ? 1 : -1;
                else          x += side == 0 ? 1 : -1;
            }
        }
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
    text35(x, y, s, GOLD);                      // clips per pixel
}

void drawBoard() {
    if (flat) {
        gfx_rect(MX - 2, MY - 2, 8 * MW + 4, 8 * MH + 4, GOLD);
        gfx_rect(MX - 1, MY - 1, 8 * MW + 2, 8 * MH + 2, INK);
        mapSquares(lightSq, darkSq);
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

    isoSquares(xb, yl - 8 * hh(), hw(), 2 * hh(), lightSq, darkSq);

    // Coordinates on the carpet below the near edges: files along the left
    // one, ranks along the right (reversed from Black's side).
    for (int i = 0; coords && i < 8; i++) {
        int dx = (2 * i + 1) * hw() / 2, dy = (2 * i + 1) * hh() / 2 + s + 2 + zoom;
        label(xl + dx - 4, yl + dy, (char)('A' + (cam.flip ? 7 - i : i)));
        label(xb + dx + 2, yb - (2 * i + 1) * hh() / 2 + s + 2 + zoom, (char)('1' + (cam.flip ? 7 - i : i)));
    }
}

}  // namespace iso
