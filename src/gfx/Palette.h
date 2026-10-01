// The one 16-colour palette, and the tricks it allows.
//
// CHGfx applies the palette while it converts the framebuffer for the panel,
// so recolouring an index recolours every pixel that uses it for free. All
// edits land in a staging copy (RGB444; theme, cycling and fade are worked
// out here) and pal::commit() pushes them once per frame. CHGfx 1.3 stages
// gfx_setPalette() itself and rebuilds its conversion LUT when the next flush
// starts, so a commit is safe at any time; it still costs that rebuild, so
// commit() does nothing on a frame where no staged colour or the fade was
// changed.
#pragma once
#include <stdint.h>

enum : uint8_t {
    INK = 0, WHITE, FELT_DK, FELT, FELT_LT, SILVER, RED, WINE,
    GOLD, WOOD, BLUE, NAVY, SKIN, CYAN, FX_A, FX_B,
};

namespace pal {

enum Theme : uint8_t { GREEN, BLUE_FELT, RED_FELT, PURPLE, THEME_COUNT };

void init();                                // defaults + immediate commit
void setTheme(uint8_t theme);
uint8_t theme();
void setFade(uint8_t level);                // 0 = black .. 16 = full colour
uint8_t fade();
void setFx(uint8_t index, uint16_t rgb444); // FX_A/FX_B manual control
void setCycling(bool on);                   // rainbow FX_A + pulse FX_B
// What FX_A/FX_B cycle through. CASINO: CHBlackjack's rainbow and gold/white
// pulse (banners, titles, the cursor). TARGETS: while a piece is picked up,
// FX_A shimmers cyan/white (squares it can move to) and FX_B pulses
// red/gold (pieces it can take). HOVER: as CASINO, but FX_A fades black ->
// white -> black over a second (the outline of the piece under the glove).
enum Mode : uint8_t { CASINO, TARGETS, HOVER };
void setMode(uint8_t m);
void tick();                                // once per frame, before commit
void resetClock();                          // debug: restart the FX_A/FX_B cycle
void commit();                              // once per frame, if anything changed
uint16_t rgb444(uint8_t index);             // current staged colour

}  // namespace pal
