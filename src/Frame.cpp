#pragma GCC optimize("Os")
#include <Arduino.h>
#include <CHGfx.h>
#include "../config.h"
#include "Frame.h"
#include "CHGame.h"
#include "gfx/Palette.h"
#include "states/Screens.h"
#include "stage/Stage.h"
#include "debug/Debug.h"
#include "engine/Engine.h"
#include "audio/Audio.h"
#ifdef CHSIM
#include <sim.h>
#endif

namespace frame {

// While the CPU thinks, the search runs flat out - no frames at all - and
// every SEARCH_MS stops for a BURST_MS burst of frames at the full rate: the
// CPU's glove glides to the piece it is weighing (stage::thinkPick). On the
// board a flush alone costs the search ~5 ms of CPU and a redraw ~8 more, so
// short smooth bursts beat drawing thinly all along. A button starts a burst
// at once, and a held one or an open menu keeps it going. The engine calls
// back every 8 nodes (CH2K_POLL_NODES, ~5 ms), so that is prompt. Between
// bursts, a frame every BOB_MS keeps the glove bobbing (a step of it each).
static const uint16_t FIRST_MS = 300, SEARCH_MS = 2000, BURST_MS = 450, BOB_MS = 133;
static uint32_t burstAt, bobAt;
// Lockstep (scripts, the simulator): one frame per two polls, i.e. per 16
// nodes, so a scripted CPU move always takes the same frames.
static const uint8_t POLLS_PER_FRAME = 2;
#ifdef CHSIM
// Free-running simulator: the board's search speed (~1,700 nodes/s,
// measured), so thinking takes as long as there.
static const uint32_t SIM_US_PER_POLL = 8 * 590;
#endif

// Frames drawn from inside the search run on a stack of their own: the
// search can be ~1.5 KB deep in the 2 KB stack, and a frame (drawing, the
// debug protocol, interrupts) needs about 800 bytes more.
#ifndef CHSIM
static uint32_t frameStack[256] __attribute__((aligned(16)));   // 1 KB

__attribute__((noinline)) static void onFrameStack(void (*fn)()) {
    asm volatile(
        "mv   t1, sp\n"
        "mv   sp, %1\n"
        "addi sp, sp, -16\n"
        "sw   t1, 12(sp)\n"
        "jalr ra, 0(%0)\n"
        "lw   t1, 12(sp)\n"
        "mv   sp, t1\n"
        :
        : "r"(fn), "r"(frameStack + 256)
        : "ra", "t0", "t1", "t2", "t3", "t4", "t5", "t6",
          "a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7", "memory");
}
#else
static void onFrameStack(void (*fn)()) { fn(); }
#endif

void begin() {
#if CHCH_DEBUG && !defined(CHSIM)
    dbg::frameStackLo = frameStack;
    dbg::frameStackHi = frameStack + 256;
#endif
    dbg::paintStack();
    pal::init();
    screens::begin();
    eng::pollHook = thinkPoll;
}

bool run(bool thinking) {
    dbg::poll();
    if (!arduboy.nextFrame()) return false;
    dbg::markUpdateStart();
    // Logic runs at a fixed 60 Hz. If a heavy frame made drawing fall
    // behind, catch up (up to three ticks) before drawing again.
    uint8_t ticks = 0;
    do {
        arduboy.pollButtons();
        pal::tick();
        audio::update();
        screens::update(thinking);
    } while (++ticks < 3 && arduboy.nextFrame());
    gfx_wait();
    pal::commit();
    dbg::markRenderStart();
    screens::render(arduboy.frameCount);
    dbg::markRenderEnd();
    gfx_flushAsync();
    return true;
}

static void thinkFrame();

void thinkPoll() { onFrameStack(thinkFrame); }

static void thinkFrame() {
#if CHCH_DEBUG
    if (arduboy.lockstep >= 0) {
        static uint8_t polls;
        dbg::poll();
        if (++polls < POLLS_PER_FRAME) return;
        polls = 0;
        // Out of frames: wait for the driver's next N, as loop() would.
        for (;;) {
            dbg::poll();
            if (arduboy.lockstep != 0) break;
            dbg::waitInput();
        }
        run(true);
        return;
    }
#endif
#ifdef CHSIM
    sim_advance(SIM_US_PER_POLL);
#endif
    uint32_t now = millis();
    if (eng::nodes() <= 8) burstAt = now + FIRST_MS;        // a new search
    if ((int32_t)(now - burstAt) < 0 && !(chgame_readButtons() | arduboy.injected)) {
        if ((int32_t)(now - bobAt) >= 0) {
            bobAt = now + BOB_MS;
            arduboy.frameCount |= 7;                 // frame >> 3 (the bob) steps on by one
            run(true);
        }
        return;
    }
    stage::thinkPick();
    uint32_t end = now + BURST_MS;
    while ((int32_t)(millis() - end) < 0 || (chgame_readButtons() | arduboy.injected) || screens::holdFrames()) {
        if (!run(true)) {
#ifdef CHSIM
            sim_advance(1000);
#endif
        }
    }
    burstAt = millis() + SEARCH_MS;
}

}  // namespace frame
