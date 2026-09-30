// The board on screen: the isometric view (zoomable) and the flat map
// (strategy view), with the table, the board's slab, square highlights and
// where pieces stand.
//
// Iso world space is the 2:1 projection itself, in pixels at the current
// zoom, with the board's far corner at (0, 0). View space (u, v) names
// squares as the camera sees them: u runs down-right on screen, v down-left,
// and the lattice point (u, v) sits at world ((u - v) * hw, (u + v) * hh).
// From White's side a8 is the far corner, a1 the left and h1 the near one;
// from Black's side the board is turned half round (flip). The map uses the
// same (u, v): u across, v down.
#pragma once
#include <stdint.h>

namespace iso {

constexpr int CX = 64, CY = 69;             // screen point the camera looks at (below the HUD)

// Current view: tileH is half a tile's height, 5 (20x10 tiles) up to 10
// (40x20, pieces drawn doubled) - the camera zooms through the steps between.
// flat = the board seen from above (the map).
extern uint8_t tileH;
extern bool flat;
void setView(bool flat);
inline int hw() { return 2 * tileH; }       // half a tile's width
inline int hh() { return tileH; }           // half a tile's height
inline int slab() { return (3 * tileH + 2) / 5; }   // board thickness
inline int zscale() { return tileH * 256 / 5; }     // art scale, Q8 (256 = 1:1)
inline int zoomed(int px) { return (px * zscale()) >> 8; }

// Map layout: squares MW x MH from (MX, MY).
constexpr int MX = 10, MY = 26, MW = 14, MH = 11;

// Camera: the world point shown at (CX, CY) (iso only).
struct Cam { int x, y; bool flip; };
extern Cam cam;

// Square 0..63 (a1 = 0) <-> view coordinates.
void toView(uint8_t sq, bool flip, int &u, int &v);
uint8_t fromView(int u, int v, bool flip);

// World position of a lattice point (iso).
inline int worldX(int u, int v) { return (u - v) * hw(); }
inline int worldTop(int u, int v) { return (u + v) * hh(); }
// Where a square's piece stands on screen (the centre of its base).
void screenOf(uint8_t sq, int &x, int &y);
// World position of that point (iso; the map's world is the screen).
void worldOf(uint8_t sq, int &x, int &y);
inline int toScreenX(int wx) { return flat ? wx : wx - cam.x + CX; }
inline int toScreenY(int wy) { return flat ? wy : wy - cam.y + CY; }

// Colours: squares, table, shadow.
extern uint8_t darkSq, lightSq, tableCol, tableDot, tableShadow;
extern bool coords;                          // letters/numbers round the board

void drawTable();                            // carpet, scrolled with the camera
void drawBoard();                            // slab or frame, squares, coordinates
// Border of a square, `inset` px in from its edge. With c2 != c the border
// is dashed and `phase` marches the dashes round.
void tileBorder(uint8_t sq, uint8_t inset, uint8_t c, uint8_t c2, uint8_t phase);
// Half of a square's pixels (checkerboard) in c, inset in from its edge.
void tileTint(uint8_t sq, uint8_t inset, uint8_t c);

}  // namespace iso
