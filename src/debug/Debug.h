// Serial debug protocol (CHCH_DEBUG builds only).
//
// Line-based ASCII over the USB CDC port. Unknown input is ignored and never
// answered, so it cannot confuse chgame-upload.
//
//   ?          -> "CHCS <ver> frame=<n> lock=<n>"
//   S          -> "FB <frame> 8224\n" then 8192 framebuffer + 32 palette bytes
//   K <hex>    hold these buttons (ORed with the real ones); K alone releases
//   L1 / L0    lockstep on / off (on = the game only advances on N)
//   N <k>      run k frames, then answer "OK <frame>"
//   P          -> "PERF upd=<us> wait=<us> rnd=<us> max=<us> late=<n> ram=<b>"
//   T          -> "PROF <slot>=<us> ..." section timings from dbg::prof()
//   B          -> reboot into the CHGame bootloader
//   anything else is offered to the game's hook (reseed, stacked deck, ...)
#pragma once
#include "../../config.h"
#include <stdint.h>

namespace dbg {

#if CHCH_DEBUG
void poll();
void paintStack();                  // at boot: for the stack high-water mark in P
extern uint32_t *frameStackLo, *frameStackHi;   // the second stack (src/Frame.cpp)
void markUpdateStart();
void markRenderStart();
void markRenderEnd();
void print(const char *s);
void waitInput();                   // block until input may have arrived (sim) / no-op
// While true for a command, it (a game command, to the hook) is held until
// it turns false: nothing may touch the game while the CPU searches.
extern bool (*holdGame)(char cmd);
// Profiling (CHCH_PROFILE builds): prof(i) charges the time since the
// previous prof() to slot i; the T command reports and resets the averages.
#if CHCH_PROFILE
void profStart();
void prof(uint8_t slot);
#else
inline void profStart() {}
inline void prof(uint8_t) {}
#endif
uint32_t parseNum(const char *&p, uint8_t base);   // skips leading spaces/commas
// Game-specific commands: return true if handled.
extern bool (*hook)(char cmd, const char *args);
#else
inline void poll() {}
inline void paintStack() {}
inline void markUpdateStart() {}
inline void markRenderStart() {}
inline void markRenderEnd() {}
inline void print(const char *) {}
inline void waitInput() {}
inline void profStart() {}
inline void prof(uint8_t) {}
#endif

}  // namespace dbg
