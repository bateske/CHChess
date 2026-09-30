// The play screen's presentation: the camera, the pointing fingers, pieces
// that lift, glide and land, captures knocked tumbling off the board, tile
// highlights, check and mate, the HUD and the top-down strategy view.
//
// Match reports events; the stage turns them into motion and says busy()
// until it has shown them (CHBlackjack's Presenter, for chess).
#pragma once
#include <stdint.h>

namespace stage {

void begin();
void update(bool thinking);          // drain match events, advance motion (once per tick)
bool busy();
bool overShown();                    // the end of the game has been shown
// Draws the play scene; false if nothing changed since the last frame (the
// framebuffer still holds it). ui: a signature of what the caller draws on top.
bool render(uint32_t frame, uint32_t ui);
void invalidate();                   // redraw next frame

// The player's side of it (the play screen drives these).
uint8_t cursor();
void setCursor(uint8_t sq);
void moveCursor(int du, int dv);     // in view space: +u screen down-right, +v down-left
bool flipped();                      // viewing from Black's side
void select(uint8_t sq, const uint8_t *to, const uint8_t *cap, uint8_t n);
void deselect();
uint8_t selected();                  // 0xFF: none
void deny(uint8_t sq);               // the piece shakes its head
bool strategy();
void setStrategy(bool on);
void setHints(bool on);              // show the legal moves of a selected piece
void setCoords(bool on);             // file/rank letters on the rim
void setFast(bool on);               // quicker CPU turns and moves
extern const char *opponentName;     // the HUD's name for the CPU

// The board and pieces only (the title screen's backdrop), and one piece
// anywhere on screen (the promotion reel).
void renderScene(uint32_t frame);
void drawPieceAt(uint8_t piece, int x, int y);

}  // namespace stage
