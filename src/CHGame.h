// CHGame - Arduboy-flavoured input and frame pacing for the CHGame board.
//
// Started from the helper shared by CHSpriteView/CHMultiSprite/CHStlView and
// reworked for this game:
//   * button masks are parenthesised, so ~UP_BUTTON and A|B behave;
//   * every query reads the state captured by pollButtons(), so a frame sees
//     one consistent snapshot and injected input (debug protocol, simulator)
//     behaves exactly like a real press;
//   * nextFrame() uses a microsecond accumulator, so 60 fps is 60.0, not the
//     62.5 that 1000/60 = 16 ms gave;
//   * a lockstep mode lets the debug protocol step the game frame by frame;
//   * auto-repeat for held buttons (menus, bet adjust).
#pragma once
#include <stdint.h>

#define A_BUTTON      (1u << 0)
#define B_BUTTON      (1u << 1)
#define UP_BUTTON     (1u << 2)
#define DOWN_BUTTON   (1u << 3)
#define LEFT_BUTTON   (1u << 4)
#define RIGHT_BUTTON  (1u << 5)
#define START_BUTTON  (1u << 6)
#define SELECT_BUTTON (1u << 7)

// Physical button mask, pressed = 1. Implemented per platform (device: GPIO
// registers; simulator: the scripted input).
uint8_t chgame_readButtons();

class CHGame {
public:
    void boot();
    void setFrameRate(uint8_t fps);
    bool nextFrame();

    void pollButtons();
    uint8_t buttons() const              { return cur; }
    bool pressed(uint8_t b) const        { return (cur & b) == b; }
    bool anyPressed(uint8_t b) const     { return (cur & b) != 0; }
    bool justPressed(uint8_t b) const    { return (cur & ~prev & b) != 0; }
    bool justReleased(uint8_t b) const   { return (prev & ~cur & b) != 0; }
    uint8_t justPressedMask() const      { return (uint8_t)(cur & ~prev); }
    // Press edge, then auto-repeat while held (delay/rate in frames).
    bool repeat(uint8_t b, uint8_t delay = 18, uint8_t rate = 5) const;
    void clearButtonState()              { prev = cur; }

    bool everyXFrames(uint16_t n) const  { return (frameCount % n) == 0; }

    uint32_t frameCount = 0;
    uint8_t  injected = 0;          // ORed into the physical buttons
#ifdef CHSIM
    int32_t  lockstep = 0;          // the simulator starts paused, driven by N
#else
    int32_t  lockstep = -1;         // <0 free-running, else frames still allowed
#endif

private:
    uint8_t  cur = 0, prev = 0;
    uint16_t held[8] = {};          // frames each button has been held
    uint32_t period = 16667, next = 0;
};

extern CHGame arduboy;              // one instance, Arduboy-style name
