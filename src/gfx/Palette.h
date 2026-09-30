// The one 16-colour palette, and the tricks it allows.
//
// CHGfx applies the palette while it converts the framebuffer for the panel,
// so recolouring an index recolours every pixel that uses it for free. The
// catch: gfx_setPalette() rebuilds the conversion LUT, so it must never run
// while an async flush is converting. All edits land in a staging copy and
// pal::commit() pushes them once per frame, right after gfx_wait().
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
// red/gold (pieces it can take).
enum Mode : uint8_t { CASINO, TARGETS };
void setMode(uint8_t m);
void tick();                                // once per frame, before commit
void resetClock();                          // debug: restart the FX_A/FX_B cycle
void commit();                              // only after gfx_wait()
uint16_t rgb444(uint8_t index);             // current staged colour

}  // namespace pal
