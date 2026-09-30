#pragma GCC optimize("Os")
#include <Arduino.h>
#include <CHGfx.h>
#include "../config.h"
#include "Frame.h"
#include "CHGame.h"
#include "gfx/Palette.h"
#include "states/Screens.h"
#include "debug/Debug.h"
#include "engine/Engine.h"
#include "audio/Audio.h"
#ifdef CHSIM
#include <sim.h>
#endif

namespace frame {

// Lockstep (scripts, the simulator): one frame per this many polls, i.e.
// per 256 nodes, so a scripted CPU move always takes the same frames.
static const uint8_t POLLS_PER_FRAME = 1;
#ifdef CHSIM
// Free-running simulator: virtual time the device might spend per node
// (an estimate until measured on the board).
static const uint32_t SIM_US_PER_256_NODES = 256 * 40;
#endif

void begin() {
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
    dbg::markWaitStart();
    gfx_wait();
    pal::commit();
    dbg::markRenderStart();
    screens::render(arduboy.frameCount, thinking);
    dbg::markRenderEnd();
    gfx_flushAsync();
    return true;
}

void thinkPoll() {
#if CHCH_DEBUG
    if (arduboy.lockstep >= 0) {
        static uint8_t polls;
        dbg::poll();
        if (++polls < POLLS_PER_FRAME) return;
        polls = 0;
        // Out of frames: wait for the driver's next N, as loop() would.
        while (arduboy.lockstep == 0) { dbg::poll(); dbg::waitInput(); }
        run(true);
        return;
    }
#endif
#ifdef CHSIM
    sim_advance(SIM_US_PER_256_NODES);
#endif
    run(true);
}

}  // namespace frame
