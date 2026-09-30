// Sound and the status LED.
//
// CHBlackjack's piezo sequencer: effects are short step lists (6 bytes a
// step) kept inside the piezo's 1-4 kHz sweet spot. There is no music
// player here - the fanfares are effects too (flash is spent on the chess).
#pragma once
#include <stdint.h>

enum class Sfx : uint8_t {
    Cursor, Select, Deny, Land, Hop, Capture, Coin, Check, Castle, Promote,
    Whoosh, Flip, Mate, Win, Lose, Draw, Turn, Title,
    Tick, Tock,                      // soft (quieter than the rest): keep them last
    COUNT
};

namespace audio {

bool begin(bool on);
void setOn(bool on);
void sfx(Sfx s);
bool playing();                     // an effect is sounding
void update();                      // once per frame: LED patterns

// Status LED (PB9): short patterns for wins.
enum Led : uint8_t { LED_OFF, LED_BLINK, LED_TRIPLE, LED_PARTY };
void led(Led pattern);

}  // namespace audio
