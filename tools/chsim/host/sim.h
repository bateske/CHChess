// chsim internals shared by the host shims.
#pragma once
#include <stdint.h>

uint32_t sim_now();                 // virtual microseconds
void sim_advance(uint32_t us);
void sim_present();                 // a frame went to the "panel"
void sim_bug(const char *msg);      // report a correctness bug (exit code 3)
void sim_waitInput();               // block until the driver sends more input
uint64_t sim_hostNanos();           // real PC time, for the render cost estimate
