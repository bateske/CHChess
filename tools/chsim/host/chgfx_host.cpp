// chsim - the transport half of CHGfx on a PC.
//
// The drawing code (CHGfx_draw.cpp) is compiled unmodified from the installed
// library; this file replaces CHGfx.cpp, which talks to SPI and DMA. Flushes
// take the time the real panel link takes, and two classic bugs are caught:
// drawing into the framebuffer while an async flush is still converting it,
// and rebuilding the palette LUT during a flush.
#include <CHGfx.h>
#include "sim.h"

uint8_t  gfx_fb[GFX_FB_BYTES];
uint16_t gfx_pal[16];
CHGfx Gfx;

static uint8_t  s_mode = GFX_16BPP, s_div = GFX_DIV2;
static uint8_t  s_scratch[2 * GFX_CHUNK_BYTES];
static uint32_t s_busyUntil = 0;       // virtual micros when the flush finishes
static uint32_t s_fbHash = 0;
static bool     s_checking = false;

static uint32_t hashFb() {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < GFX_FB_BYTES; i++) { h ^= gfx_fb[i]; h *= 16777619u; }
    return h;
}

static uint32_t flushMicros(int w, int h) {
    // Measured: full frame 8.37 ms at 12 bpp, 11.1 ms at 16 bpp.
    uint32_t px = (uint32_t)w * (uint32_t)h;
    return s_mode == GFX_12BPP ? 40 + px * 511 / 1000 : 40 + px * 677 / 1000;
}

static void checkTear() {
    if (s_checking && sim_now() < s_busyUntil && hashFb() != s_fbHash)
        sim_bug("framebuffer changed while an async flush was converting it");
}

void gfx_begin(uint8_t spiDiv, uint8_t colorMode) {
    s_div = spiDiv; s_mode = colorMode;
    static const uint16_t def[16] = {
        0x0000, 0xFFFF, 0xF800, 0x07E0, 0x001F, 0xFFE0, 0x07FF, 0xF81F,
        0x8410, 0xC618, 0x4208, 0xFD20, 0x8000, 0x0400, 0x0010, 0x8010 };
    memcpy(gfx_pal, def, sizeof def);
    memset(gfx_fb, 0, sizeof gfx_fb);
}
void gfx_setSpiDiv(uint8_t d) { s_div = d; }
void gfx_setColorMode(uint8_t m) { s_mode = m; }
uint8_t gfx_colorMode(void) { return s_mode; }
uint8_t gfx_spiDiv(void) { return s_div; }
uint32_t gfx_spiHz(void) { return 48000000u / s_div; }
void gfx_setPanelOffsets(uint8_t, uint8_t, uint8_t) {}
void gfx_setInverted(bool) {}
void gfx_setPanelFrameRate(uint8_t, uint8_t, uint8_t) {}
uint8_t *gfx_chunkScratch(void) { return s_scratch; }
uint32_t gfx_frameBytes(void) { return s_mode == GFX_12BPP ? 24576 : 32768; }
uint8_t gfx_bytesPerPixel(void) { return s_mode == GFX_18BPP ? 3 : 2; }

void gfx_setPalette(const uint16_t *rgb565, uint8_t count) {
    if (sim_now() < s_busyUntil) sim_bug("palette LUT rebuilt during an async flush");
    for (uint8_t i = 0; i < count && i < 16; i++) gfx_pal[i] = rgb565[i];
}
void gfx_setPaletteEntry(uint8_t index, uint16_t rgb565) {
    if (sim_now() < s_busyUntil) sim_bug("palette LUT rebuilt during an async flush");
    if (index < 16) gfx_pal[index] = rgb565;
}
uint8_t gfx_nearest(uint16_t c) {
    uint8_t best = 0; uint32_t bd = ~0u;
    for (uint8_t i = 0; i < 16; i++) {
        int dr = (int)(c >> 11) - (int)(gfx_pal[i] >> 11);
        int dg = (int)((c >> 5) & 63) - (int)((gfx_pal[i] >> 5) & 63);
        int db = (int)(c & 31) - (int)(gfx_pal[i] & 31);
        uint32_t d = (uint32_t)(dr * dr * 4 + dg * dg + db * db * 4);
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

void gfx_flushRect(int x, int y, int w, int h) {
    gfx_wait();
    sim_present();
    sim_advance(flushMicros(w, h));
}
void gfx_flushRectAsync(int x, int y, int w, int h) {
    gfx_wait();
    sim_present();
    s_fbHash = hashFb();
    s_checking = true;
    s_busyUntil = sim_now() + flushMicros(w, h);
}
void gfx_flush(void) { gfx_flushRect(0, 0, GFX_W, GFX_H); }
void gfx_flushAsync(void) { gfx_flushRectAsync(0, 0, GFX_W, GFX_H); }
bool gfx_busy(void) { checkTear(); return sim_now() < s_busyUntil; }
void gfx_wait(void) {
    checkTear();
    if (sim_now() < s_busyUntil) sim_advance(s_busyUntil - sim_now());
    s_checking = false;
}

void gfx_stream(gfx_streamFn, void *) {}
void gfx_select(void) {}
void gfx_deselect(void) {}
void gfx_setWindow(uint8_t, uint8_t, uint8_t, uint8_t) {}
void gfx_cmd(uint8_t) {}
void gfx_data8(uint8_t) {}
void gfx_writeColorLut(void) {}
void gfx_setWriteColorLut(bool) {}
void gfx_directFillRect(uint8_t, uint8_t, uint8_t, uint8_t, uint16_t) {}
void gfx_directBlit(const void *, uint32_t, bool) {}
void gfx_blockingWrite(const uint8_t *, uint32_t) {}
