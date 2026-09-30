// Saving options, lifetime stats and a game in progress.
//
// CHGame has no EEPROM, but its bootloader only erases the flash pages a new
// sketch occupies, so the last pages of the application region survive
// re-uploads. Two pages are used in turn, each record carrying a sequence
// number and a CRC, so a power cut mid-write can only lose the newest save.
// If the sketch ever grows into those pages, saving switches itself off
// rather than overwrite code. (From CHBlackjack, with its own magic: the two
// games share the pages, and each ignores the other's records.)
#pragma once
#include <stdint.h>
#include "../game/Match.h"

struct Options {
    uint8_t sound;      // 0 off, 1 on
    uint8_t felt;       // board colour theme (pal::Theme)
    uint8_t hints;      // show where a picked-up piece can go
    uint8_t coords;     // letters and numbers on the board's rim
    uint8_t speed;      // 0 normal, 1 fast (the CPU's camera and moves)
    uint8_t level;      // last opponent chosen
    uint8_t side;       // last side chosen: 0 white, 1 black, 2 random
    uint8_t pad;
};

struct Stats {
    uint16_t won[match::LEVELS], lost[match::LEVELS], drawn[match::LEVELS];
};

namespace save {

bool available();                   // false: image too big, or a write failed
bool load(Options &o, Stats &s, bool &hasGame);
bool loadGame();                    // the saved game into match (replayed)
// Call after gfx_wait(): the page is built in CHGfx's chunk scratch.
bool store(const Options &o, const Stats &s, bool withGame);

}  // namespace save
