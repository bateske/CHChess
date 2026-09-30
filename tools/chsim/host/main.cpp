// chsim - runs a CHGame sketch on a PC.
//
// The sketch starts in lockstep (CHGame::lockstep = 0) and is driven entirely
// through its serial debug protocol on stdin/stdout, the same protocol the
// real device speaks over USB. tools/chsim/chdrive.py talks to either.
#include <Arduino.h>
#include <stdarg.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif
#include "sim.h"
#include <chrono>

void setup();
void loop();

SimSerial Serial;
uint32_t sim_ledState = 0;
uint32_t sim_soundEffects = 0, sim_soundScores = 0;

static uint32_t s_now = 0;
static uint32_t s_idle = 0;
static int s_bugs = 0;
static char s_in[256];
static int s_inLen = 0, s_inPos = 0;

uint32_t sim_now() { return s_now; }
void sim_advance(uint32_t us) { s_now += us; }
void sim_present() { s_idle = 0; }
void sim_bug(const char *msg) {
    s_bugs++;
    fprintf(stderr, "BUG: %s (t=%u us)\n", msg, (unsigned)s_now);
}

uint32_t micros() { return s_now; }
uint32_t millis() { return s_now / 1000; }
void delay(uint32_t ms) { s_now += ms * 1000; }
void delayMicroseconds(uint32_t us) { s_now += us; }
void pinMode(uint32_t, uint32_t) {}
void digitalWrite(uint32_t pin, uint32_t v) { if (pin == LED_BUILTIN) sim_ledState = v; }
int digitalRead(uint32_t) { return HIGH; }

static uint32_t s_rng = 1;
static uint32_t rng() { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5; return s_rng; }
void randomSeed(unsigned long s) { s_rng = s ? (uint32_t)s : 1; }
long random(long hi) { return hi > 0 ? (long)(rng() % (uint32_t)hi) : 0; }
long random(long lo, long hi) { return hi > lo ? lo + random(hi - lo) : lo; }

// Input only arrives through the debug protocol's K command.
uint8_t chgame_readButtons() { return 0; }

// --- Serial on stdin/stdout -----------------------------------------------
static bool refill(bool block) {
    if (s_inPos < s_inLen) return true;
    if (!block) return false;
    if (!fgets(s_in, sizeof s_in, stdin)) {
        exit(s_bugs ? 3 : 0);       // driver closed the pipe: done
    }
    s_inLen = (int)strlen(s_in);
    s_inPos = 0;
    return s_inLen > 0;
}

// The sketch has nothing to do until the driver speaks (e.g. the CPU's
// search waiting for the next lockstep N).
void sim_waitInput() { refill(true); }

uint64_t sim_hostNanos() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

int SimSerial::available() { return refill(false) ? s_inLen - s_inPos : 0; }
int SimSerial::read() { return refill(false) ? (uint8_t)s_in[s_inPos++] : -1; }
size_t SimSerial::write(uint8_t c) { fputc(c, stdout); fflush(stdout); return 1; }
size_t SimSerial::write(const uint8_t *p, size_t n) { fwrite(p, 1, n, stdout); fflush(stdout); return n; }
size_t SimSerial::print(const char *s) { return write((const uint8_t *)s, strlen(s)); }
size_t SimSerial::println(const char *s) { size_t n = print(s); return n + print("\r\n"); }
int SimSerial::printf(const char *fmt, ...) {
    char buf[256];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    print(buf);
    return n;
}

// Debug.cpp's raw output path in simulator builds.
void sim_out(const uint8_t *p, uint32_t n) { fwrite(p, 1, n, stdout); fflush(stdout); }

int main() {
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    setup();
    for (;;) {
        // In lockstep the sketch idles between N commands; once it has gone
        // a few loops without presenting a frame, wait for the next command.
        if (++s_idle > 2) refill(true);
        loop();
        s_now += 100;
    }
}
uint8_t _ebss;   // stands in for the linker symbol Debug.cpp reports against
