#pragma GCC optimize("Os")   // cold code: size over speed (hot pixel loops live in Draw/Mask)
#include <CHGfx.h>
#include "Palette.h"

namespace pal {

// Authored on the RGB444 grid so 12 bpp output, fades and the simulator agree.
static const uint16_t BASE[16] = {
    0x000,  // INK
    0xFFF,  // WHITE
    0x042,  // FELT_DK
    0x173,  // FELT
    0x4B5,  // FELT_LT
    0xBBC,  // SILVER
    0xE12,  // RED
    0x702,  // WINE
    0xFC2,  // GOLD
    0x741,  // WOOD
    0x26E,  // BLUE
    0x125,  // NAVY
    0xFB8,  // SKIN
    0x6EF,  // CYAN
    0xF0F,  // FX_A
    0xFC2,  // FX_B
};

static const uint16_t THEMES[THEME_COUNT][3] = {
    {0x042, 0x173, 0x4B5},   // classic green
    {0x024, 0x149, 0x48D},   // blue
    {0x401, 0x812, 0xC44},   // red
    {0x203, 0x517, 0x95B},   // purple
};

static const uint16_t RAINBOW[12] = {
    0xF22, 0xF82, 0xFE2, 0x8F2, 0x2F4, 0x2FC, 0x2EF, 0x28F, 0x42F, 0xA2F, 0xF2E, 0xF28,
};

// TARGETS: FX_A shimmers cyan -> white, FX_B pulses red -> gold (and back).
static const uint16_t SHIMMER[8] = {0x6EF, 0x7EF, 0x9EF, 0xAFF, 0xBFF, 0xCFF, 0xEFF, 0xFFF};
static const uint16_t PULSE[8]   = {0xE12, 0xE32, 0xE52, 0xF72, 0xF82, 0xF92, 0xFB2, 0xFC2};

static uint16_t staged[16];
static uint16_t committed[16];
static bool     dirty = true;
static uint8_t  themeIdx = 0, fadeLevel = 16;
static bool     cycling = true;
static uint32_t ticks = 0;
static uint8_t  mode = CASINO;

void init() {
    for (uint8_t i = 0; i < 16; i++) staged[i] = BASE[i];
    dirty = true;
    commit();
}

void setTheme(uint8_t t) {
    if (t >= THEME_COUNT) t = 0;
    themeIdx = t;
    for (uint8_t i = 0; i < 3; i++) staged[FELT_DK + i] = THEMES[t][i];
    dirty = true;
}
uint8_t theme() { return themeIdx; }

void setFade(uint8_t level)      { if (level > 16) level = 16; if (level != fadeLevel) { fadeLevel = level; dirty = true; } }
uint8_t fade()                   { return fadeLevel; }
void setFx(uint8_t i, uint16_t c){ staged[i] = c; dirty = true; }
void setCycling(bool on)         { cycling = on; }
void setMode(uint8_t m)          { mode = m; }
uint16_t rgb444(uint8_t i)       { return staged[i]; }

// Triangle wave 0..15..0 over 32 frames.
static uint8_t tri(uint32_t t) { t &= 31; return (uint8_t)(t > 15 ? 31 - t : t); }

void tick() {
    ticks++;
    if (!cycling) return;
    if (mode == TARGETS) {
        staged[FX_A] = SHIMMER[tri(ticks * 2) >> 1];
        staged[FX_B] = PULSE[tri(ticks * 2 + 16) >> 1];
    } else {
        staged[FX_A] = mode == HOVER ? (uint16_t)(tri(ticks >> 1) * 0x111) : RAINBOW[(ticks / 3) % 12];
        // FX_B: triangle wave GOLD <-> WHITE over 32 frames.
        uint8_t t = tri(ticks);
        uint8_t g = (uint8_t)(12 + (t * 3) / 15), b = (uint8_t)(2 + (t * 13) / 15);
        staged[FX_B] = (uint16_t)(0xF00 | (g << 4) | b);
    }
    dirty = true;
}

void resetClock() { ticks = 0; }

void commit() {
    if (!dirty) return;
    dirty = false;
    uint16_t out[16];
    for (uint8_t i = 0; i < 16; i++) {
        uint16_t c = staged[i];
        uint16_t r = (uint16_t)((((c >> 8) & 15) * fadeLevel) >> 4);
        uint16_t g = (uint16_t)((((c >> 4) & 15) * fadeLevel) >> 4);
        uint16_t b = (uint16_t)(((c & 15) * fadeLevel) >> 4);
        out[i] = (uint16_t)((((r << 1) | (r >> 3)) << 11) | (((g << 2) | (g >> 2)) << 5) | ((b << 1) | (b >> 3)));
    }
    bool same = true;
    for (uint8_t i = 0; i < 16; i++) if (out[i] != committed[i]) { same = false; break; }
    if (same) return;
    for (uint8_t i = 0; i < 16; i++) committed[i] = out[i];
    gfx_setPalette(out, 16);
}

}  // namespace pal
