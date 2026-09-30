// A game of chess: whose turn, the CPU, the rules' verdicts, undo, and the
// events the presentation turns into motion. No graphics or sound here, so
// the host tests can play thousands of games (tools/tests/test_chess.cpp).
//
// The flow mirrors CHBlackjack's Round: the match reports events and waits
// while the stage is busy showing them, so the rules never outrun the show.
// The CPU's search is blocking; while it runs, the engine's poll hook keeps
// frames coming (src/Frame.h) and update() is not re-entered.
#pragma once
#include <stdint.h>
#include "../engine/Engine.h"

namespace match {

enum Mode : uint8_t { VS_CPU, TWO_PLAYER };

enum Result : uint8_t {
    PLAYING, WHITE_WINS, BLACK_WINS, STALEMATE, DRAW_50, DRAW_REPETITION, DRAW_MATERIAL,
};
enum Reason : uint8_t { BY_MATE, BY_RESIGNATION, BY_RULE };

enum Ev : uint8_t {
    EV_START,          // a game began (or was restored/undone): redraw from board[]
    EV_TURN,           // a = side to move (0 white, 1 black), b = 1 if a human moves
    EV_THINK,          // the CPU starts thinking
    EV_PICK,           // the CPU chose: a = from, b = to (the stage points, then taps)
    EV_MOVE,           // a = from, b = to, piece, captured (code), capSq, rookFrom/To, promo
    EV_CHECK,          // a = square of the king in check
    EV_OVER,           // a = Result, b = Reason
};
struct Event {
    uint8_t type, a, b;
    uint8_t piece, captured, capSq, rookFrom, rookTo, promo;
};

struct Setup {
    uint8_t mode;       // Mode
    uint8_t humanBlack; // VS_CPU: the human plays Black (the CPU opens)
    uint8_t level;      // VS_CPU: 0..LEVELS-1
    uint32_t seed;      // the CPU's random stream
};

constexpr uint8_t LEVELS = 5;
extern const eng::Level LEVEL[LEVELS];

// State the stage and the screens read.
extern uint8_t board[64];           // settled position (piece codes)
extern Setup setup;
extern uint8_t lastFrom, lastTo;    // 0xFF: none
extern uint8_t checkSq;             // king in check, 0xFF: none
extern Result result;
extern Reason reason;
extern uint16_t plies;              // half-moves played

void start(const Setup &s);
#ifdef CHSIM
void startFen(const Setup &s, const char *fen);   // scripted positions (simulator)
#endif
void update(bool stageBusy);        // once per logic tick
bool active();                      // a game is on (not over)
bool blackToMove();
bool humanToMove();                 // waiting for input
bool cpuThinking();

// Human moves.
uint8_t movesFrom(uint8_t from, uint8_t *to, uint8_t *capture);   // destinations (<= 32)
uint8_t movable(uint8_t *sq);        // the human's pieces that have a move (<= 16)
bool needsPromotion(uint8_t from, uint8_t to);
bool play(uint8_t from, uint8_t to, uint8_t promo = eng::QUEEN);  // false if illegal
bool canUndo();
bool undo();                        // vs CPU: back to your previous move; 2P: one move
void resign();
void abortThink();                  // from the think hook: the next update() drops the search

bool popEvent(Event &e);
bool peekEvent(Event &e);
int material();                     // white minus black

// Saved games: a snapshot plus the moves since, replayed on load.
struct Record {
    eng::Snap base;                 // 38 B
    uint8_t baseIsStart;
    uint8_t mode, humanBlack, level;
    uint8_t n;                      // moves in m[]
    eng::Move m[76];
};
void save(Record &r);
bool load(const Record &r);

}  // namespace match
