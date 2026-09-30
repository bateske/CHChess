// chsim - just enough Arduino for CHGame sketches to run on a PC.
//
// Time is virtual: micros()/millis() advance only when the simulator says so,
// so a run is exactly reproducible. Serial is the process's stdin/stdout,
// which is how the Python driver talks to the sketch's debug protocol.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define PROGMEM
#define F(s) (s)
#define pgm_read_byte(p) (*(const uint8_t *)(p))
#define pgm_read_word(p) (*(const uint16_t *)(p))
#define pgm_read_ptr(p) (*(void *const *)(p))

#define HIGH 1
#define LOW 0
#define INPUT 0
#define OUTPUT 1
#define INPUT_PULLUP 2
#define LED_BUILTIN 20
#define F_CPU 48000000u

uint32_t micros();
uint32_t millis();
void delay(uint32_t ms);
void delayMicroseconds(uint32_t us);
void pinMode(uint32_t, uint32_t);
void digitalWrite(uint32_t pin, uint32_t v);
int digitalRead(uint32_t);
long random(long howbig);
long random(long lo, long hi);
void randomSeed(unsigned long s);

template <class T> static inline T constrain(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }
#ifndef min
#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))
#endif

class SimSerial {
public:
    int available();
    int read();
    size_t write(uint8_t c);
    size_t write(const uint8_t *p, size_t n);
    size_t print(const char *s);
    size_t println(const char *s = "");
    int printf(const char *fmt, ...);
    void flush() {}
    void begin(unsigned long) {}
    explicit operator bool() const { return true; }
    bool waitForPC(uint32_t = 0) { return true; }
};
extern SimSerial Serial;

// Hooks the simulator exposes to the harness-aware bits of a sketch.
extern uint32_t sim_ledState;
