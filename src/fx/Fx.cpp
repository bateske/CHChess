#pragma GCC optimize("Os")   // cold code: size over speed (hot pixel loops live in Draw/Mask)
#include <CHGfx.h>
#include <string.h>
#include "Fx.h"
#include "../gfx/Draw.h"
#include "../gfx/Mask.h"
#include "../gfx/Palette.h"

namespace fx {

// ---------------------------------------------------------------------------
// Curves: 17-point Q8 tables, linearly interpolated.
// ---------------------------------------------------------------------------
static const int16_t CURVES[5][17] = {
    {0, 16, 32, 48, 64, 80, 96, 112, 128, 144, 160, 176, 192, 208, 224, 240, 256},
    {0, 45, 84, 119, 148, 173, 194, 210, 224, 235, 242, 248, 252, 254, 256, 256, 256},
    {0, 69, 126, 173, 209, 237, 257, 271, 278, 281, 281, 277, 272, 267, 261, 258, 256},
    {0, 3, 11, 24, 40, 59, 81, 104, 128, 152, 175, 197, 216, 232, 245, 253, 256},
    {0, 8, 30, 68, 121, 189, 248, 215, 196, 193, 204, 231, 249, 240, 246, 253, 256},
};

int ease(Ease e, int t, int n) {
    if (n <= 0 || t >= n) return 256;
    if (t <= 0) return 0;
    int p = (t << 8) / n;                    // 0..255
    int i = p >> 4, f = p & 15;
    const int16_t *c = CURVES[e];
    return c[i] + (((c[i + 1] - c[i]) * f) >> 4);
}

static const uint8_t SIN[65] = {
    0, 6, 13, 19, 25, 31, 38, 44, 50, 56, 62, 68, 74, 80, 86, 92, 98, 104, 109, 115, 121, 126,
    132, 137, 142, 147, 152, 157, 162, 167, 172, 177, 181, 185, 190, 194, 198, 202, 206, 209,
    213, 216, 220, 223, 226, 229, 231, 234, 237, 239, 241, 243, 245, 247, 248, 250, 251, 252,
    253, 254, 255, 255, 255, 255, 255};

int isin(int a) {
    a &= 255;
    int q = a >> 6, i = a & 63;
    int v;
    switch (q) {
        case 0: v = SIN[i]; break;
        case 1: v = SIN[64 - i]; break;
        case 2: v = -SIN[i]; break;
        default: v = -SIN[64 - i]; break;
    }
    return v;
}

static uint32_t seed = 0x1234567u;
uint32_t rnd() { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; }
void reseed() { seed = 0x1234567u; }
int rndRange(int lo, int hi) { return hi > lo ? lo + (int)(rnd() % (uint32_t)(hi - lo)) : lo; }

// ---------------------------------------------------------------------------
// Particles
// ---------------------------------------------------------------------------
struct Particle { int16_t x, y; int8_t vx, vy; uint8_t life, colour, kind, age; };
static Particle parts[48];

void spawn(Kind k, int x, int y, int vx, int vy, uint8_t life, uint8_t colour) {
    Particle *slot = nullptr;
    for (auto &p : parts) if (!p.life) { slot = &p; break; }
    if (!slot) slot = &parts[rnd() % 48];                 // steal one
    slot->x = (int16_t)(x << 4); slot->y = (int16_t)(y << 4);
    slot->vx = (int8_t)(vx < -127 ? -127 : vx > 127 ? 127 : vx);
    slot->vy = (int8_t)(vy < -127 ? -127 : vy > 127 ? 127 : vy);
    slot->life = life; slot->colour = colour; slot->kind = k; slot->age = 0;
}

void burst(Kind k, int x, int y, uint8_t n, int speed, uint8_t colour) {
    for (uint8_t i = 0; i < n; i++) {
        int a = (int)(i * 256 / n) + rndRange(0, 12);
        int sp = speed / 2 + rndRange(0, speed / 2 + 1);
        spawn(k, x, y, (isin(a + 64) * sp) >> 8, (isin(a) * sp) >> 8, (uint8_t)rndRange(20, 40), colour);
    }
}

void fountain(int x, int y, uint8_t n) {
    static const uint8_t CONF[6] = {RED, GOLD, FELT_LT, CYAN, BLUE, WHITE};
    for (uint8_t i = 0; i < n; i++)
        spawn(CONFETTI, x + rndRange(-4, 5), y, rndRange(-28, 29), rndRange(-60, -30), (uint8_t)rndRange(40, 70),
              CONF[rnd() % 6]);
}

bool particlesAlive() {
    for (auto &p : parts) if (p.life) return true;
    return false;
}

static void updateParticles() {
    for (auto &p : parts) {
        if (!p.life) continue;
        p.life--; p.age++;
        p.x += p.vx; p.y += p.vy;
        switch (p.kind) {
            case CONFETTI: if (p.age & 1) p.vy += 2; if (p.vy > 24) p.vy = 24;
                           p.vx = (int8_t)(p.vx * 15 / 16); break;
            case DUST:     p.vx = (int8_t)(p.vx * 7 / 8); p.vy = (int8_t)(p.vy * 7 / 8); break;
            default:       p.vy += (p.age & 3) == 0; break;
        }
    }
}

void drawParticles() {
    for (auto &p : parts) {
        if (!p.life) continue;
        int x = p.x >> 4, y = p.y >> 4;
        switch (p.kind) {
            case SPARK:
                gfx_pixel(x, y, p.colour);
                if (p.age < 8) { gfx_pixel(x - 1, y, p.colour); gfx_pixel(x + 1, y, p.colour);
                                 gfx_pixel(x, y - 1, p.colour); gfx_pixel(x, y + 1, p.colour); }
                break;
            case CONFETTI:
                if ((p.age >> 2) & 1) gfx_hline(x, y, 2, p.colour);
                else gfx_vline(x, y, 2, p.colour);
                break;
            case STAR:
                gfx_hline(x - 1, y, 3, p.colour); gfx_vline(x, y - 1, 3, p.colour);
                break;
            case DUST: if (p.life > 4 || (p.life & 1)) gfx_pixel(x, y, p.colour); break;
        }
    }
}

// ---------------------------------------------------------------------------
// Banner
// ---------------------------------------------------------------------------
static char bannerText[14];
static uint8_t bannerStyle, bannerLen, bannerT, bannerFrames;
static int bannerCy;

void banner(const char *text, BannerStyle s, int cy, uint8_t frames) {
    strncpy(bannerText, text, sizeof bannerText - 1);
    bannerText[sizeof bannerText - 1] = 0;
    bannerStyle = s; bannerCy = cy; bannerT = 0; bannerFrames = frames;
    bannerLen = frames;
}

bool bannerActive() { return bannerFrames != 0; }

void drawBanner() {
    if (!bannerFrames) return;
    int t = bannerT;
    uint8_t scale = t < 3 ? 2 : (t < 7 ? 4 : 3);
    int w = text35WidthScaled(bannerText, scale);
    while (w > 124 && scale > 2) w = text35WidthScaled(bannerText, --scale);
    int h = 6 * scale;
    int8_t dy[14];
    int n = (int)strlen(bannerText);
    for (int k = 0; k < n && k < 14; k++) dy[k] = (int8_t)((isin(t * 10 + k * 36) * 2) >> 8) + 2;
    Mask m = maskBegin(w + 1, h + 5);
    maskText35(m, 0, 0, bannerText, scale, dy);
    // Last few frames: blink out.
    if (bannerFrames < 10 && (bannerFrames & 2)) return;
    uint8_t ramp[32];
    static const uint8_t RAIN[5] = {RED, GOLD, FELT_LT, CYAN, BLUE};
    for (int r = 0; r < h + 5 && r < 32; r++) {
        switch (bannerStyle) {
            case B_RAINBOW: ramp[r] = RAIN[((r / 2) + t / 3) % 5]; break;
            case B_GOLD:    ramp[r] = r < 3 ? FX_B : (r < h / 2 + 6 ? GOLD : WOOD); break;
            case B_RED:     ramp[r] = r < 3 ? WHITE : RED; break;
            case B_CYAN:    ramp[r] = r < 3 ? WHITE : CYAN; break;
            default:        ramp[r] = WHITE; break;
        }
    }
    uint8_t outline = bannerStyle == B_RAINBOW ? FX_A : INK;
    maskDraw(m, 64 - w / 2, bannerCy - h / 2 - 2, WHITE, outline, bannerStyle == B_RAINBOW ? INK : WINE, ramp);
}

// ---------------------------------------------------------------------------
// Floating text and shake
// ---------------------------------------------------------------------------
struct Float { int16_t x, y; uint8_t t, colour; char text[8]; };
static Float floats[4];

void floatText(const char *text, int x, int y, uint8_t colour) {
    Float *f = &floats[0];
    for (auto &q : floats) if (!q.t) { f = &q; break; }
    f->x = (int16_t)x; f->y = (int16_t)y; f->t = 50; f->colour = colour;
    strncpy(f->text, text, 7); f->text[7] = 0;
}

void drawFloats() {
    for (auto &f : floats) {
        if (!f.t) continue;
        int y = f.y - (50 - f.t) / 2;
        int x = f.x - text35Width(f.text) / 2;
        if (f.t < 8 && (f.t & 1)) continue;
        text35(x + 1, y + 1, f.text, INK);
        text35(x, y, f.text, f.colour);
    }
}

static uint8_t shakeT, shakeAmp;

void shake(uint8_t frames, uint8_t amp) { shakeT = frames; shakeAmp = amp; }

void applyShake(int y0, int y1) {
    if (!shakeT) return;
    int a = (shakeAmp * shakeT + 9) / 10;
    if (a < 1) a = 1;
    int dy = (shakeT & 1) ? a : -a;
    int dxBytes = (shakeT & 2) ? 1 : -1;                    // 2 px sideways
    int rows = y1 - y0 + 1;
    uint8_t *base = gfx_fb + y0 * GFX_FB_STRIDE;
    if (dy > 0) memmove(base + dy * GFX_FB_STRIDE, base, (size_t)(rows - dy) * GFX_FB_STRIDE);
    else memmove(base, base - dy * GFX_FB_STRIDE, (size_t)(rows + dy) * GFX_FB_STRIDE);
    for (int y = 0; y < rows; y++) {
        uint8_t *row = base + y * GFX_FB_STRIDE;
        if (dxBytes > 0) memmove(row + 1, row, GFX_FB_STRIDE - 1);
        else memmove(row, row + 1, GFX_FB_STRIDE - 1);
    }
}

bool activeRows(int &lo, int &hi) {
    lo = 999; hi = -1;
    if (shakeT) { lo = 0; hi = 127; return true; }
    for (auto &p : parts) if (p.life) { int y = p.y >> 4; if (y - 2 < lo) lo = y - 2; if (y + 3 > hi) hi = y + 3; }
    for (auto &f : floats) if (f.t) { int y = f.y - (50 - f.t) / 2; if (y - 1 < lo) lo = y - 1; if (y + 7 > hi) hi = y + 7; }
    if (bannerFrames) { if (bannerCy - 18 < lo) lo = bannerCy - 18; if (bannerCy + 18 > hi) hi = bannerCy + 18; }
    return hi >= lo;
}

void clear() {
    memset(parts, 0, sizeof parts);
    memset(floats, 0, sizeof floats);
    bannerFrames = 0;
    shakeT = 0;
}

void update() {
    updateParticles();
    if (bannerFrames) { bannerFrames--; bannerT++; }
    for (auto &f : floats) if (f.t) f.t--;
    if (shakeT) shakeT--;
}

}  // namespace fx
