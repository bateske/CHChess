// CHChess build switches.
//
// Keep feature switches here rather than in --build-property flags. The game
// needs the CHGame core 0.2.4+ with Optimize set to "Smallest + LTO" and the
// default Peripherals setting ("Game", which compiles out
// Serial1/tone/HardwareTimer: ~4 KB of flash). Release builds also set USB
// to "Upload only" (no Serial: ~0.6 KB).
#pragma once

#define CHCH_VERSION     "0.1"

// Serial debug protocol: screenshots, input injection, lockstep, perf.
// Off in normal builds. tools/device.py turns it on with
// --build-property build.extra_flags, and leaves USB at "Serial" for it.
#ifndef CHCH_DEBUG
#ifdef CHSIM
#define CHCH_DEBUG       1       // the simulator is driven through the protocol
#else
#define CHCH_DEBUG       0
#endif
#endif

// Device debug builds carry the ~2 KB protocol, so they leave out things the
// tests never need (saving, the options screen and its credits). The
// simulator (not flash-bound) and release builds keep everything.
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
