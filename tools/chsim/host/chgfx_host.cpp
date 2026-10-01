// chsim - the transport half of CHGfx on a PC.
//
// The library's portable files (every src/*.cpp but CHGfx.cpp: drawing,
// extras, text effects, the staged palette and fade) are compiled unmodified
// from the installed library; this file replaces CHGfx.cpp, which talks to
// SPI and DMA. Flushes take the time the real panel link takes and convert
// their rows a chunk at a time, as the DMA interrupt does, so
// gfx_flushRow() / gfx_waitRow() report real progress. Two classic bugs are
// caught: drawing into rows an async flush has not converted yet, and
// writing gfx_chunkScratch() while a flush is still using it. Palette
// changes are staged by the library, so they are legal at any time.
//
// Virtual time only moves at loop(), delay() and the flush calls, so a
// change is only reported when it is certain: a row that differs while
// the flush has still not converted it.
#include <CHGfx.h>
#include "sim.h"

bool gfx__paletteTake(uint16_t out[16]);   // CHGfx_palette.cpp (CHGfx_internal.h)

uint8_t  gfx_fb[GFX_FB_BYTES];
CHGfx Gfx;

static uint8_t  s_mode = GFX_16BPP, s_div = GFX_DIV2;
static uint8_t  s_scratch[2 * GFX_CHUNK_BYTES];
static const uint8_t CANARY = 0xA5;

// The flush in flight: rows [y, y + h) convert in chunks of rowsPer, the
// first two before the DMA starts, then one more every chunkUs.
static struct {
    bool     active, tore, scratchBug;
    int      y, h, rowsPer;
    uint32_t t0, end;
    double   chunkUs;
    uint8_t  snap[GFX_FB_BYTES];
} s_job;

static uint32_t flushMicros(int w, int h) {
    // Measured: full frame 8.37 ms at 12 bpp, 11.1 ms at 16 bpp.
    uint32_t px = (uint32_t)w * (uint32_t)h;
    return s_mode == GFX_12BPP ? 40 + px * 511 / 1000 : 40 + px * 677 / 1000;
}

static int convertedRows(uint32_t now) {
    if (now >= s_job.end) return s_job.h;
    int chunks = 2 + (now > s_job.t0 ? (int)((now - s_job.t0) / s_job.chunkUs) : 0);
    return chunks * s_job.rowsPer < s_job.h ? chunks * s_job.rowsPer : s_job.h;
}

static void progress() {
    if (!s_job.active) return;
    uint32_t now = sim_now();
    if (now >= s_job.end) { s_job.active = false; return; }
    for (int i = convertedRows(now); i < s_job.h && !s_job.tore; i++) {
        int r = s_job.y + i;
        if (memcmp(gfx_fb + r * GFX_FB_STRIDE, s_job.snap + r * GFX_FB_STRIDE, GFX_FB_STRIDE)) {
            s_job.tore = true;
            sim_bug("framebuffer changed while an async flush was converting it");
        }
    }
    for (size_t i = 0; i < sizeof s_scratch && !s_job.scratchBug; i++)
        if (s_scratch[i] != CANARY) {
            s_job.scratchBug = true;
            sim_bug("gfx_chunkScratch() written while an async flush was using it");
        }
}

void gfx_begin(uint8_t spiDiv, uint8_t colorMode) {
    s_div = spiDiv; s_mode = colorMode;
    static const uint16_t def[16] = {
        0x0000, 0xFFFF, 0xF800, 0x07E0, 0x001F, 0xFFE0, 0x07FF, 0xF81F,
        0x8410, 0xC618, 0x4208, 0xFD20, 0x8000, 0x0400, 0x0010, 0x8010 };
    gfx_setPalette(def, 16);
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

void gfx_flushRectAsync(int x, int y, int w, int h) {
    gfx_wait();
    uint16_t out[16];
    gfx__paletteTake(out);                  // a staged palette applies from here
    sim_present();
    uint32_t us = flushMicros(w, h);
    if (y < 0) { h += y; y = 0; }
    if (y + h > GFX_H) h = GFX_H - y;
    if (h < 1) h = 0;
    int bpr = (s_mode == GFX_12BPP ? 3 : 4) * (w > 0 ? w : 1) / 2;
    s_job.rowsPer = (int)(GFX_CHUNK_BYTES / bpr);
    if (s_job.rowsPer < 1) s_job.rowsPer = 1;
    s_job.y = y; s_job.h = h;
    s_job.t0 = sim_now();
    s_job.end = s_job.t0 + us;
    s_job.chunkUs = h ? (double)us * s_job.rowsPer / h : 1.0;
    s_job.tore = s_job.scratchBug = false;
    memcpy(s_job.snap, gfx_fb, sizeof gfx_fb);
    memset(s_scratch, CANARY, sizeof s_scratch);
    s_job.active = true;
}
void gfx_flushRect(int x, int y, int w, int h) {
    gfx_flushRectAsync(x, y, w, h);
    gfx_wait();
}
void gfx_flush(void) { gfx_flushRect(0, 0, GFX_W, GFX_H); }
void gfx_flushAsync(void) { gfx_flushRectAsync(0, 0, GFX_W, GFX_H); }
bool gfx_busy(void) { progress(); return s_job.active; }
void gfx_wait(void) {
    progress();
    if (s_job.active) sim_advance(s_job.end - sim_now());
    s_job.active = false;
}
int gfx_flushRow(void) {
    progress();
    return s_job.active ? s_job.y + convertedRows(sim_now()) : GFX_H;
}
void gfx_waitRow(int y) {
    while (gfx_flushRow() < y) sim_advance(10);
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
