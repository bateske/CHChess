// One frame of the game: input, logic, drawing, the DMA flush.
//
// loop() runs it, and so does the CPU's search: the engine calls
// frame::thinkPoll() every 8 nodes, which runs a frame whenever one is
// due, with the game logic paused (Match waits for the search) but the
// camera, the CPU's pointing finger, particles and palette all moving.
#pragma once
#include <stdint.h>

namespace frame {

void begin();
bool run(bool thinking);            // true if a frame was due and ran
void thinkPoll();                   // the engine's poll hook

}  // namespace frame
