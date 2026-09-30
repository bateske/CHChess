#pragma GCC optimize("Os")
#include "Debug.h"
#if CHCH_DEBUG
#include <Arduino.h>
#include <CHGfx.h>
#include "../CHGame.h"
#include "../gfx/Fmt.h"
#include <string.h>

#ifndef CHSIM
extern "C" {
uint8_t CDC_write_nb(char c);
void CDC_flush(void);
uint8_t CDC_enumerated(void);
uint8_t CDC_dtr(void);
void chgame_enter_bootloader(void);
}
#else
void sim_out(const uint8_t *p, uint32_t n);
void sim_waitInput();
uint64_t sim_hostNanos();
// Render cost in the simulator: host nanoseconds (tools/chsim/perf.py turns
// them into device milliseconds with a calibration against the CHGfx
// benchmark's measured primitives).
static uint64_t pcT0, pcSum, pcMax;
#endif

namespace dbg {

// Stack high-water mark: the stack is painted at boot, and P reports how
// deep anything has reached since (the search plus a frame drawn from its
// poll hook is the deepest path).
#ifndef CHSIM
extern "C" uint32_t _susrstack[], _eusrstack[];
uint32_t *frameStackLo, *frameStackHi;      // src/Frame.cpp's stack for frames drawn mid-search
static const uint32_t PAINT = 0xA5A5A5A5u;
void paintStack() {
    uint32_t here;
    for (uint32_t *p = _susrstack; p < &here - 16; p++) *p = PAINT;
    for (uint32_t *p = frameStackLo; p < frameStackHi; p++) *p = PAINT;
}
static uint32_t stackUsed(uint32_t *lo, uint32_t *hi) {
    uint32_t *p = lo;
    while (p < hi && *p == PAINT) p++;
    return (uint32_t)((uint8_t *)hi - (uint8_t *)p);
}
#else
void paintStack() {}
static uint32_t stackUsed(uint32_t *, uint32_t *) { return 0; }
static uint32_t *_susrstack, *_eusrstack;
uint32_t *frameStackLo, *frameStackHi;
#endif

bool (*hook)(char cmd, const char *args) = nullptr;
bool (*holdGame)() = nullptr;
static char held[100];               // a game command waiting for the search to end

static char line[100];
static uint8_t len = 0;
static bool ackPending = false;
static uint32_t tRnd;
static uint32_t sumRnd, maxRnd, frames, late;
static uint32_t lastFrameStart;
#if CHCH_PROFILE
static uint32_t profT, profSum[12], profFrames;

void profStart() { profT = micros(); profFrames++; }
void prof(uint8_t slot) {
    uint32_t now = micros();
    if (slot < 12) profSum[slot] += now - profT;
    profT = now;
}
#endif

#ifndef CHSIM
// Bulk writes go straight to the CDC endpoint in 64-byte packets: the core's
// Serial.write() flushes after every byte.
static void out(const uint8_t *p, uint32_t n) {
    if (!CDC_enumerated() || !CDC_dtr()) return;
    while (n) {
        uint32_t t = millis();
        while (!CDC_write_nb((char)*p)) {
            if (!CDC_enumerated() || millis() - t > 25) return;
        }
        p++; n--;
    }
    CDC_flush();
}
#else
static void out(const uint8_t *p, uint32_t n) { ::sim_out(p, n); }
#endif

void print(const char *s) {
    uint32_t n = 0;
    while (s[n]) n++;
    out((const uint8_t *)s, n);
}

uint32_t parseNum(const char *&p, uint8_t base) {
    uint32_t v = 0;
    while (*p == ' ' || *p == ',') p++;
    for (;; p++) {
        char c = *p;
        uint8_t d;
        if (c >= '0' && c <= '9') d = (uint8_t)(c - '0');
        else if (base == 16 && c >= 'a' && c <= 'f') d = (uint8_t)(c - 'a' + 10);
        else if (base == 16 && c >= 'A' && c <= 'F') d = (uint8_t)(c - 'A' + 10);
        else break;
        v = v * base + d;
    }
    return v;
}

// "KEY=value" pairs, one line.
static char *kv(char *p, const char *key, uint32_t v) {
    p = fmtStr(p, key);
    return fmtInt(p, (int32_t)v);
}

static void execute() {
    line[len] = 0;
    char cmd = line[0];
    const char *args = line + 1;
    while (*args == ' ') args++;
    char buf[112];
    char *p = buf;
    switch (cmd) {
        case '?':
            print("CHCS " CHCH_VERSION "\n");
            break;
        case 'S':
            gfx_wait();
            p = kv(p, "FB ", arduboy.frameCount);
            fmtStr(p, " 8224\n");
            print(buf);
            out(gfx_fb, GFX_FB_BYTES);
            out((const uint8_t *)gfx_pal, 32);
            break;
        case 'K':
            arduboy.injected = (uint8_t)parseNum(args, 16);
            print("OK\n");
            break;
        case 'L':
            arduboy.lockstep = (*args == '1') ? 0 : -1;
            print("OK\n");
            break;
        case 'N':
            if (arduboy.lockstep < 0) arduboy.lockstep = 0;
            arduboy.lockstep += (int32_t)parseNum(args, 10);
            ackPending = true;
            break;
        case 'P': {
            uint32_t f = frames ? frames : 1;
            p = kv(p, "PERF rnd=", sumRnd / f);
            p = kv(p, " max=", maxRnd);
            p = kv(p, " late=", late);
            p = kv(p, " frames=", frames);
            p = kv(p, " stk=", stackUsed(_susrstack, _eusrstack));
            p = kv(p, " fstk=", stackUsed(frameStackLo, frameStackHi));
#ifdef CHSIM
            p = kv(p, " pcrnd=", (uint32_t)(pcSum / f));
            p = kv(p, " pcmax=", (uint32_t)pcMax);
            pcSum = pcMax = 0;
#endif
            fmtStr(p, "\n");
            print(buf);
            sumRnd = maxRnd = frames = late = 0;
            break;
        }
#if CHCH_PROFILE
        case 'T': {
            uint32_t f = profFrames ? profFrames : 1;
            p = fmtStr(p, "PROF");
            for (int i = 0; i < 12; i++) {
                if (!profSum[i]) continue;
                *p++ = ' ';
                p = fmtInt(p, i);
                p = kv(p, "=", profSum[i] / f);
                profSum[i] = 0;
            }
            fmtStr(p, "\n");
            print(buf);
            profFrames = 0;
            break;
        }
#endif
#ifndef CHSIM
        case 'B':
            chgame_enter_bootloader();
            break;
#endif
        default:
            // Mid-search: answer HELD now, OK/ERR once it has run.
            if (holdGame && holdGame()) { memcpy(held, line, sizeof held); print("HELD\n"); break; }
            print(hook && hook(cmd, args) ? "OK\n" : "ERR\n");
            break;
    }
}

void waitInput() {
#ifdef CHSIM
    sim_waitInput();
#endif
}

void poll() {
    if (held[0] && !(holdGame && holdGame())) {
        const char *args = held + 1;
        while (*args == ' ') args++;
        print(hook && hook(held[0], args) ? "OK\n" : "ERR\n");
        held[0] = 0;
    }
    if (ackPending && arduboy.lockstep == 0) {
        ackPending = false;
        char buf[20];
        fmtStr(fmtInt(fmtStr(buf, "OK "), (int32_t)arduboy.frameCount), "\n");
        print(buf);
    }
    while (Serial.available()) {
        int c = Serial.read();
        if (c < 0) break;
        if (c == '\n' || c == '\r') {
            if (len) execute();
            len = 0;
        } else if (c >= 32 && c < 127 && len < sizeof(line) - 1) {
            line[len++] = (char)c;
        } else {
            len = 0;    // binary noise: drop the line
        }
    }
}

void markUpdateStart() {
    uint32_t now = micros();
    if (frames && arduboy.lockstep < 0 && now - lastFrameStart > 17500) late++;
    lastFrameStart = now;
}
void markRenderStart() {
    tRnd = micros();
#ifdef CHSIM
    pcT0 = sim_hostNanos();
#endif
}
void markRenderEnd() {
#ifdef CHSIM
    uint64_t d = sim_hostNanos() - pcT0;
    pcSum += d;
    if (d > pcMax) pcMax = d;
#endif
    uint32_t r = micros() - tRnd;
    sumRnd += r;
    if (r > maxRnd) maxRnd = r;
    frames++;
}

}  // namespace dbg
#endif
