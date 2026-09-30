// The game's face on the ch2k engine (ArduChess, MPL-2.0): squares 0..63
// (a1 = 0, h8 = 63), pieces as small codes, moves as the engine's raw two
// bytes. Nothing else in the game includes ch2k.hpp.
//
// While think() runs, the engine's board is mid-search: the poll hook (and
// everything it calls: drawing, the stage, debug commands) must not call
// anything here except rootMove() and nodes().
#pragma once
#include <stdint.h>

namespace eng {

enum : uint8_t { EMPTY = 0, PAWN, KNIGHT, BISHOP, ROOK, QUEEN, KING };
constexpr uint8_t BLACK = 8;                    // colour bit of a piece code
constexpr uint8_t TYPE = 7;

typedef uint16_t Move;                          // ch2k move: a | b << 8
constexpr Move NO_MOVE = 0;

uint8_t from(Move m);                           // 0..63
uint8_t to(Move m);
bool isCastle(Move m);
bool isEnPassant(Move m);
bool isPromotion(Move m);
uint8_t promotion(Move m);                      // KNIGHT..QUEEN
Move withPromotion(Move m, uint8_t type);

inline uint8_t sqAt(uint8_t file, uint8_t rank) { return (uint8_t)(rank * 8 + file); }
inline uint8_t fileOf(uint8_t s) { return s & 7; }
inline uint8_t rankOf(uint8_t s) { return s >> 3; }

enum Status : uint8_t {
    NORMAL, CHECK, DRAW_50, DRAW_REPETITION, DRAW_MATERIAL, STALEMATE, MATED,
};

// A position the game can come back to without replaying from the start
// (long games, saved games). 38 bytes.
struct Snap {
    uint8_t board[32];                          // nibble per square: type | BLACK
    uint8_t black;                              // side to move
    uint8_t castle;                             // engine castling flags
    uint8_t lastA, lastB;                       // last move (en passant)
    uint16_t ply;
};

void newGame();
void snapshot(Snap &s);
void restore(const Snap &s);                    // out of book, 50-move count restarts
void play(Move m);                              // must be legal
uint8_t legal(Move *out);                       // all legal moves (<= 218; tests)
uint8_t movesFrom(uint8_t from, Move *out);     // legal moves of one piece (<= 32)
Move findMove(uint8_t from, uint8_t to, uint8_t promo);   // NO_MOVE if illegal
uint8_t moveCount();                            // number of legal moves
Status status();
uint8_t pieceAt(uint8_t square);                // EMPTY or type | BLACK
bool blackToMove();
uint16_t ply();
bool inBook();
int materialBalance();                          // white minus black, pawns = 1

// The CPU: node budget, how far below the best a move may score and still
// be picked, and draw contempt (1 = 16 cp).
struct Level { uint32_t nodes; int16_t margin; uint8_t contempt; };
Move think(const Level &lv);                    // blocking; calls CH2K_POLL
Move benchThink(const Level &lv);               // no book, no poll hook (speed tests)
void abort();                                   // from the poll hook
bool aborted();
int16_t lastScore();                            // side to move's view, cp
uint32_t nodes();
Move rootMove();                                // root move being searched (hook-safe)

#ifdef CHSIM
void loadFen(const char *fen);                  // simulator / tests only
#endif

void seed(uint32_t s);                          // the AI's own random stream
uint8_t rand8();

// Called every 8 nodes during think() (null = nothing).
extern void (*pollHook)();
bool thinking();

}  // namespace eng
