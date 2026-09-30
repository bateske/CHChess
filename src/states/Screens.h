// The screens: title, setup, play (with pause), options, stats, credits.
#pragma once
#include <stdint.h>

namespace screens {

void begin();
void update(bool thinking);         // logic, before gfx_wait (thinking: CPU search running)
void render(uint32_t frame);                  // after gfx_wait
bool holdFrames();                   // a menu or a held button wants frames mid-search

}  // namespace screens
