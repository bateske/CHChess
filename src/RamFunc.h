// RAMFUNC(name): run a function from SRAM. Flash has 3 wait states and no
// cache: from flash a per-pixel loop costs ~3 us a pixel on this part. The
// flash programming in Save.cpp must run from SRAM too.
//
// The section name is CHGfx 1.3's trick: the core's link script puts
// *(.gnu.linkonce.r.*) first in .data (copied to SRAM at boot), ahead of
// .sdata/.sbss. Under the old .srodata.* name the code sat after .sdata,
// inside the 4 KB the global pointer reaches, and pushed variables out of
// it, each access to them an instruction longer. linkonce merges
// sections of the same name, so every function needs a name of its own
// (which also lets the linker drop unused ones one at a time).
#pragma once

#ifdef CHSIM
#define RAMFUNC(name) __attribute__((noinline))
#else
#define RAMFUNC(name) __attribute__((section(".gnu.linkonce.r.chch." #name), noinline))
#endif
