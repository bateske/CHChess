// A small sound sequencer for the CHGame piezo (from CHBlackjack).
//
// Effects play on TIM1 channel 2 (PB10), stepped by the core's 1 kHz
// SysTick hook (osSystickHandler), so no other timer is used. One pin plays
// one note at a time; a higher-priority effect is never cut off by a lower
// one. tools/audio/preview.py renders every effect to WAV from this file.
#pragma GCC optimize("Os")
#include <Arduino.h>
#include "Audio.h"

#ifdef CHSIM
// The simulator is silent: same interface, no hardware. It remembers the
// last effect so scripts and tests can check what would have sounded.
namespace audio {
static bool simOn = true;
uint8_t simLast = 0xFF;
bool begin(bool on) { simOn = on; return true; }
void setOn(bool on) { simOn = on; }
void sfx(Sfx s) { if (simOn) simLast = (uint8_t)s; }
bool playing() { return false; }
void update() {}
void led(Led) {}
}
#else

struct Step { uint16_t hz, endHz, ms; };        // effect step; hz 0 = rest

#define S(hz, end, ms) { (uint16_t)(hz), (uint16_t)(end), (uint16_t)(ms) }
#define REST(ms)       { 0, 0, (uint16_t)(ms) }

static const Step CURSOR[]  = { S(2100, 0, 10) };
static const Step SELECT[]  = { S(1700, 0, 18), S(2600, 0, 30) };
static const Step DENY[]    = { S(900, 650, 70) };
// A wooden piece set down: a knock and a higher tick.
static const Step LAND[]    = { S(2400, 1100, 14), REST(8), S(3300, 0, 16) };
static const Step HOP[]     = { S(1500, 3300, 80) };
// A smash - tones alternating high and low read as noise on a piezo, the
// lows lengthening as it lands - then the piece spinning away: falling
// swoops, about as long as it takes to fly off the screen.
static const Step CAPTURE[] = {
    S(3800, 0, 6), S(700, 0, 8), S(3200, 0, 6), S(600, 0, 8), S(2800, 0, 6), S(520, 0, 10),
    S(2400, 0, 6), S(480, 0, 12),
    S(2600, 2200, 70), S(2400, 2000, 70), S(2200, 1800, 70), S(2000, 1600, 70), S(1800, 1400, 70),
    S(1600, 1200, 80), S(1400, 900, 110) };
static const Step COIN[]    = { S(2800, 0, 10), S(3700, 0, 28) };
static const Step CHECK[]   = { S(2637, 0, 70), S(1976, 0, 70), S(2637, 0, 70), S(1976, 0, 120) };
static const Step CASTLE[]  = { S(2400, 1100, 14), REST(40), S(1400, 3200, 70), REST(30), S(2400, 1100, 14), REST(8), S(3300, 0, 16) };
static const Step PROMOTE[] = {
    S(1568, 0, 45), S(2093, 0, 45), S(2637, 0, 45), S(3136, 0, 45), S(4186, 0, 60),
    S(3136, 0, 30), S(4186, 0, 30), S(3136, 0, 30), S(4186, 0, 120) };
static const Step WHOOSH[]  = { S(1200, 3800, 90) };
static const Step FLIP[]    = { S(2300, 0, 8), REST(5), S(3300, 0, 12) };
// CHBlackjack's BLACKJACK fanfare: the signature win.
static const Step MATE[]    = {
    S(1568, 0, 50), S(2093, 0, 50), S(2637, 0, 50), S(3136, 0, 90),
    S(2093, 0, 40), S(2637, 0, 40), S(2093, 0, 40), S(2637, 0, 40),
    S(3136, 0, 40), S(4186, 0, 40), S(3136, 0, 40), S(4186, 0, 40),
    S(2000, 4200, 220) };
// Victory: a short tune (was a Playtune score in CHBlackjack).
static const Step WIN[]     = {
    S(2093, 0, 110), S(2637, 0, 110), S(3136, 0, 110), S(4186, 0, 220), REST(60),
    S(3520, 0, 110), S(4186, 0, 330) };
static const Step LOSE[]    = { S(1568, 1480, 260), S(1480, 1397, 260), S(1397, 1319, 260), S(1319, 1100, 700) };
static const Step DRAW[]    = { S(1760, 0, 70), REST(40), S(1760, 0, 70), REST(40), S(1319, 0, 160) };
static const Step TURN[]    = { S(2637, 0, 40), S(3520, 0, 90) };
static const Step TITLE[]   = {
    S(1568, 0, 90), S(2093, 0, 90), S(2637, 0, 90), S(3136, 0, 180), REST(40),
    S(2637, 0, 90), S(3136, 0, 360) };
// The CPU's clock while it thinks: the faintest clicks (played soft).
static const Step TICK[]    = { S(1100, 0, 3) };
static const Step TOCK[]    = { S(850, 0, 3) };

struct SfxDef { const Step *steps; uint8_t n, prio; };
#define DEF(a, p) { a, (uint8_t)(sizeof(a) / sizeof(a[0])), p }
static const SfxDef DEFS[(int)Sfx::COUNT] = {
    DEF(CURSOR, 0), DEF(SELECT, 1), DEF(DENY, 1), DEF(LAND, 1), DEF(HOP, 1), DEF(CAPTURE, 2),
    DEF(COIN, 1), DEF(CHECK, 3), DEF(CASTLE, 2), DEF(PROMOTE, 3), DEF(WHOOSH, 1), DEF(FLIP, 1),
    DEF(MATE, 4), DEF(WIN, 4), DEF(LOSE, 4), DEF(DRAW, 4), DEF(TURN, 1), DEF(TITLE, 2),
    DEF(TICK, 0), DEF(TOCK, 0),
};

// --- Sequencer state (shared with the 1 kHz interrupt) ----------------------
static volatile const Step *fxSteps = nullptr;
static volatile uint8_t fxN = 0, fxI = 0, fxPrio = 0;
static volatile uint16_t fxT = 0;

static bool started = false, running = false;
static uint16_t lastHz = 0;
static volatile bool soft;           // the clock: a narrow pulse, quieter than any effect
static uint8_t ledPattern = 0;
static uint16_t ledT = 0;

extern "C" volatile uint32_t CFGHR_tmpB;        // GPIOB CFGHR is write-only: go through the shadow

static void pb10(uint32_t nibble) {
    uint32_t v = (CFGHR_tmpB & ~(15u << 8)) | (nibble << 8);
    CFGHR_tmpB = v;
    GPIOB->CFGHR = v;
}

static void hwInit() {
    RCC->APB2PCENR |= RCC_APB2Periph_AFIO | RCC_APB2Periph_GPIOB | RCC_APB2Periph_TIM1;
    AFIO->PCFR1 = (AFIO->PCFR1 & ~(7u << 15)) | (1u << 15);   // TIM1 partial remap: CH2 on PB10
    GPIOB->BCR = 1u << 10;
    pb10(11u);                                                // alternate-function push-pull
    TIM1->CTLR1 = 0; TIM1->CTLR2 = 0; TIM1->SMCFGR = 0; TIM1->DMAINTENR = 0;
    TIM1->CCER = 0;
    TIM1->CHCTLR1 = 0x6800;                                   // CH2 PWM1 + preload
    TIM1->CHCTLR2 = 0;
    TIM1->PSC = 47;                                           // 1 MHz
    TIM1->RPTCR = 0;
    TIM1->ATRLR = 999;
    TIM1->CH2CVR = 0;
    TIM1->CNT = 0;
    TIM1->BDTR = 0x8000;                                      // MOE
    TIM1->CCER = 0x10;
    TIM1->SWEVGR = 1;
    TIM1->INTFR = 0;
    running = false; lastHz = 0;
}

static void tone(uint16_t hz) {
    if (hz == lastHz) return;
    lastHz = hz;
    if (!hz) {
        TIM1->CH2CVR = 0; TIM1->SWEVGR = 1; TIM1->CTLR1 = 0; TIM1->INTFR = 0;
        running = false;
        return;
    }
    uint32_t period = (1000000u + hz / 2u) / hz;
    if (period < 2) period = 2;
    running = true;
    TIM1->CTLR1 = 0;
    TIM1->ATRLR = period - 1;
    TIM1->CH2CVR = soft ? period / 8 : period / 2;
    TIM1->SWEVGR = 1;
    TIM1->INTFR = 0;
    TIM1->CTLR1 = 0x81;
}

extern "C" void osSystickHandler(void) {
    if (!started) return;
    const Step *s = (const Step *)fxSteps;
    if (!s) { tone(0); return; }
    const Step &st = s[fxI];
    uint16_t hz = st.hz;
    if (hz && st.endHz) hz = (uint16_t)(st.hz + ((int32_t)st.endHz - st.hz) * fxT / st.ms);
    if (++fxT >= st.ms) {
        fxT = 0;
        if (++fxI >= fxN) { fxSteps = nullptr; fxPrio = 0; }
    }
    tone(hz);
}

namespace audio {

bool begin(bool on) {
    RCC->APB2PCENR |= RCC_APB2Periph_GPIOB;
    uint32_t v = (CFGHR_tmpB & ~(15u << 4)) | (3u << 4);          // PB9 LED: push-pull output
    CFGHR_tmpB = v;
    GPIOB->CFGHR = v;
    GPIOB->BCR = 1u << 9;
    if (!on) { started = false; lastHz = 1; tone(0); return true; }
    if (!started) hwInit();
    started = true;
    return true;
}

void setOn(bool on) { begin(on); }

void sfx(Sfx s) {
    const SfxDef &d = DEFS[(int)s];
    if (!started) return;
    if (fxSteps && d.prio < fxPrio) return;
    __disable_irq();
    fxSteps = d.steps; fxN = d.n; fxI = 0; fxT = 0; fxPrio = d.prio;
    soft = s >= Sfx::Tick;
    __enable_irq();
}

bool playing() { return fxSteps != nullptr; }

void led(Led p) { ledPattern = p; ledT = 0; }

void update() {
    if (!ledPattern) return;
    ledT++;
    bool on = false;
    switch (ledPattern) {
        case LED_BLINK:  on = ledT < 12; if (ledT > 12) ledPattern = 0; break;
        case LED_TRIPLE: on = (ledT % 16) < 8; if (ledT > 48) ledPattern = 0; break;
        case LED_PARTY:  on = (ledT % 8) < 4; if (ledT > 240) ledPattern = 0; break;
    }
    if (on && ledPattern) GPIOB->BSHR = 1u << 9;
    else GPIOB->BCR = 1u << 9;
}

}  // namespace audio
#endif  // CHSIM
