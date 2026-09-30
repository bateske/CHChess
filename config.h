// CHChess build switches.
//
// Keep feature switches here rather than in --build-property flags. The game
// needs the CHGame core 0.2.2+ with its default Peripherals menu setting
// ("Game"), which compiles out Serial1/tone/HardwareTimer: ~4 KB of flash.
#pragma once

#define CHCH_VERSION     "0.1"

// Serial debug protocol: screenshots, input injection, lockstep, perf.
// Off in normal builds. tools/device.py turns it on with
// --build-property build.extra_flags.
#ifndef CHCH_DEBUG
#ifdef CHSIM
#define CHCH_DEBUG       1       // the simulator is driven through the protocol
#else
#define CHCH_DEBUG       0
#endif
#endif

// Device debug builds carry the ~1.3 KB protocol, so they leave out things
// the tests never need (the credits card). The simulator (not flash-bound)
// and release builds keep everything.
#if CHCH_DEBUG && !defined(CHSIM) && !defined(CHCH_FULL)
#define CHCH_LEAN        1
#else
#define CHCH_LEAN        0
#endif

// Section profiler (dbg::prof + the T command). Opt-in: costs flash.
#ifndef CHCH_PROFILE
#define CHCH_PROFILE     0
#endif

// Frame rate the game logic is paced for.
#define CHCH_FPS         60
