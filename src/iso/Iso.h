// The isometric board: projection, table felt, the board's top and slab,
// tile highlights and piece placement, at the rest pose of the camera.
//
// World space is the 2:1 projection itself, in pixels, with the board's far
// corner at (0, 0). View space (u, v) names squares as the camera sees
// them: u runs down-right on screen, v down-left, and the lattice point
// (u, v) sits at world ((u - v) * HW, (u + v) * HH). From White's side a8
// is the far corner, a1 the left and h1 the near one; from Black's side the
// board is turned half round (flip).
#pragma once
#include <stdint.h>

namespace iso {

constexpr int TW = 56, TH = 28;             // tile diamond
constexpr int HW = TW / 2, HH = TH / 2;
constexpr int SLAB = 8;                     // board thickness
constexpr int CX = 64, CY = 58;             // screen point the camera looks at

// Camera: the world point shown at (CX, CY).
struct Cam { int x, y; bool flip; };
extern Cam cam;

// Square 0..63 (a1 = 0) <-> view coordinates.
void toView(uint8_t sq, bool flip, int &u, int &v);
uint8_t fromView(int u, int v, bool flip);

// World position of a square's centre (x) and its tile's top (y).
inline int worldX(int u, int v) { return (u - v) * HW; }
inline int worldTop(int u, int v) { return (u + v) * HH; }
// Screen position of a square's centre.
void screenOf(uint8_t sq, int &x, int &y);

// Tile colours: dark and light squares.
extern uint8_t darkSq, lightSq, tableCol, tableDot, tableShadow;
extern bool coords;                          // letters/numbers on the rim

void drawTable();                            // felt, scrolled with the camera
void drawBoard();                            // slab, shadow, squares, rim letters
void fillTile(uint8_t sq, uint8_t c);        // repaint one square's diamond
// Border of a square's diamond, `inset` px in from its edge. With c2 != c
// the border is dashed (runs of 4 rows) and `phase` shifts the dashes.
void tileBorder(uint8_t sq, uint8_t inset, uint8_t c, uint8_t c2, uint8_t phase);

// Half of a square's pixels (checkerboard) in c, inset rows in from its
// edge: a tint that lets the square's own colour show through.
void tileTint(uint8_t sq, uint8_t inset, uint8_t c);

// Diamond primitives in screen space (top = row of the top point).
void diamond(int cx, int top, uint8_t c);

}  // namespace iso
