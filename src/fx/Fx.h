// Motion and sparkle: easing curves, a particle pool, pop-up banners,
// floating "+$15" texts and screen shake. Integer maths only - soft-float
// trig once cost a demo on this chip 8.5 KB of flash and half its frame rate.
#pragma once
#include <stdint.h>

namespace fx {

enum Ease : uint8_t { LINEAR, OUT_CUBIC, OUT_BACK, IN_OUT, OUT_BOUNCE };
// t in 0..n -> 0..256 (OUT_BACK/OUT_BOUNCE may overshoot).
int ease(Ease e, int t, int n);
int isin(int a);                    // a in 1/256 turns -> -256..256

// Presentation-only randomness (never touches game outcomes).
uint32_t rnd();
int rndRange(int lo, int hi);
void reseed();                      // debug: restart the sequence

enum Kind : uint8_t { SPARK, CONFETTI, STAR, DUST };
void spawn(Kind k, int x, int y, int vx16, int vy16, uint8_t life, uint8_t colour);
void burst(Kind k, int x, int y, uint8_t n, int speed16, uint8_t colour);  // radial
void fountain(int x, int y, uint8_t n);                                    // confetti up
bool particlesAlive();

// Big centred lettering with an outline; pops in, holds, fades.
enum BannerStyle : uint8_t { B_RAINBOW, B_GOLD, B_RED, B_CYAN, B_WHITE };
void banner(const char *text, BannerStyle s, int cy, uint8_t frames = 70);
void holdBanner(bool on);            // keep the banner up (before it blinks out) until false
bool bannerActive();

void shake(uint8_t frames, uint8_t amplitude);

// Vertical extent of everything transient on screen (particles, banner,
// shake). Returns false if nothing is moving.
bool activeRows(int &lo, int &hi);

void clear();
void update();                      // once per frame
// dust: the size of a DUST puff (2 at the board's usual size, more zoomed in).
void drawParticles(uint8_t dust);
bool particles();                    // any still flying
void scroll(int dx, int dy);         // move them all (with the camera, so they keep their place)
extern const uint8_t RAIN[5];        // the casino rainbow: red, gold, green, cyan, blue
void drawBanner();
void applyShake(int y0, int y1);    // post-process rows y0..y1 of the framebuffer

}  // namespace fx
