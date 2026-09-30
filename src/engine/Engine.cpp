// The one translation unit that includes ch2k.hpp (ArduChess's engine,
// MPL-2.0 - see the notice at the top of that file).
#pragma GCC optimize("Os", "no-strict-aliasing")   // ch2k reuses arrays as other types
#include "Engine.h"

namespace eng {
void (*pollHook)() = nullptr;
static uint32_t rng = 0x9E3779B9u;
uint8_t rand8() {
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return (uint8_t)(rng >> 24);
}
}  // namespace eng

static void engPoll() { if (eng::pollHook) eng::pollHook(); }
#if defined(CHSIM) && !defined(CH2K_EXTRAS)
#define CH2K_EXTRAS 1       // FEN loading for scripted positions
#endif
#define CH2K_SEE 0          // MVV-LVA capture ordering: ~900 bytes smaller
#define CH2K_RAND eng::rand8
#define CH2K_POLL engPoll
#define CH2K_POLL_NODES 8    // ~5 ms at the board's ~1,700 nodes/s: frames start on time
#define CH2K_MAX_PLY 10      // 10 plies * ~80 B of stack, beside the game's own
#include "ch2k.hpp"

namespace eng {

static ch2k::game g;
static bool busy;
static bool book;               // the last think() came from the opening book

#ifdef ENG_CHECKS
#define GUARD() do { if (busy) __builtin_trap(); } while (0)   // host tests: hands off mid-search
#else
#define GUARD() ((void)0)
#endif

static inline ch2k::move raw(Move m) { return ch2k::move{(uint8_t)m, (uint8_t)(m >> 8)}; }
static inline Move pack(ch2k::move m) { return (Move)(m.a | (m.b << 8)); }
static inline uint8_t toSq(ch2k::square s) { return (uint8_t)((7 - s.row()) * 8 + s.col()); }
static inline ch2k::square fromSq(uint8_t s) { return ch2k::square::from_rowcol((uint8_t)(7 - (s >> 3)), s & 7); }

uint8_t from(Move m)      { return toSq(raw(m).fr()); }
uint8_t to(Move m)        { return toSq(raw(m).to()); }
bool isCastle(Move m)     { return raw(m).is_castle(); }
bool isPromotion(Move m)  { return raw(m).is_promotion(); }
bool isEnPassant(Move m)  { ch2k::move r = raw(m); return !r.is_promotion() && r.is_en_passant(); }

// ch2k types: 1/2 pawns, 3 N, 4 B, 5 R, 6 Q, 7 K -> ours 1..6.
static uint8_t ourType(uint8_t t) { return t <= 2 ? PAWN : (uint8_t)(t - 1); }

uint8_t promotion(Move m) { return ourType(raw(m).promotion_piece_type().x); }
Move withPromotion(Move m, uint8_t type) {
    ch2k::move r = raw(m);
    r.clear_set_promotion_piece_type(ch2k::piece_type{(uint8_t)(type + 1)});
    return pack(r);
}

static uint8_t code(ch2k::piece p) {
    if (p.is_nothing()) return EMPTY;
    return (uint8_t)(ourType(p.type().x) | (p.color().is_black() ? BLACK : 0));
}

void newGame() { GUARD(); g.new_game(); }

uint8_t pieceAt(uint8_t s) {
    GUARD();
    ch2k::piece_index i = g.square_to_index(fromSq(s));
    return i.is_nothing() ? EMPTY : code(g.index_to_piece(i));
}

bool blackToMove() { return g.gd.c_.is_black(); }
uint16_t ply() { return g.gd.ply_; }
bool inBook() { return g.gd.opening_index_ != 0; }
int materialBalance() { return (int)g.pval(ch2k::piece_color::W) - (int)g.pval(ch2k::piece_color::B); }

void snapshot(Snap &s) {
    GUARD();
    for (uint8_t i = 0; i < 32; i++) {
        uint8_t lo = pieceAt((uint8_t)(i * 2)), hi = pieceAt((uint8_t)(i * 2 + 1));
        s.board[i] = (uint8_t)(lo | (hi << 4));
    }
    s.black = g.gd.c_.is_black();
    s.castle = g.stack_base()->flags;
    s.lastA = g.stack_base()->lastmove.a;
    s.lastB = g.stack_base()->lastmove.b;
    s.ply = g.gd.ply_;
}

void restore(const Snap &s) {
    GUARD();
    g.clear();
    for (uint8_t i = 0; i < 64; i++) {
        uint8_t c = (uint8_t)((s.board[i >> 1] >> ((i & 1) * 4)) & 15);
        if (!c) continue;
        uint8_t t = c & TYPE;
        uint8_t ct = t == PAWN ? ((c & BLACK) ? 2 : 1) : (uint8_t)(t + 1);
        ch2k::piece_color col = (c & BLACK) ? ch2k::piece_color::B : ch2k::piece_color::W;
        g.add_piece(ch2k::piece::from_type_color(ch2k::piece_type{ct}, col), fromSq(i));
    }
    g.gd.c_ = s.black ? ch2k::piece_color::B : ch2k::piece_color::W;
    g.gd.ply_ = s.ply;
    g.gd.opening_index_ = 0;
    for (auto &m : g.gd.rep_moves_) m = ch2k::move::NO_MOVE;
    g.state_index = 0;
    g.stack().flags = s.castle;
    g.stack().half_move = 0;           // no history before this point to walk back through
    g.stack().cap = ch2k::piece_index::NOTHING;
    g.stack().lastmove.a = s.lastA;
    g.stack().lastmove.b = s.lastB;
}

void play(Move m) { GUARD(); g.execute_move(raw(m)); }

uint8_t legal(Move *out) {
    GUARD();
    uint8_t n = g.gen_moves(&g.mvs_[0]);
    if (n == ch2k::game::GEN_MOVES_OUT_OF_MEM) n = 0;   // cannot happen at the root
    for (uint8_t i = 0; i < n; i++) out[i] = pack(g.mvs_[i]);
    return n;
}

uint8_t movers(uint8_t *out) {
    GUARD();
    uint8_t n = g.gen_moves(&g.mvs_[0]), k = 0;
    if (n == ch2k::game::GEN_MOVES_OUT_OF_MEM) return 0;
    for (uint8_t i = 0; i < n; i++) {
        uint8_t f = toSq(g.mvs_[i].fr()), j = 0;
        while (j < k && out[j] != f) j++;
        if (j == k) out[k++] = f;
    }
    return k;
}

uint8_t movesFrom(uint8_t from, Move *out) {
    GUARD();
    uint8_t n = g.gen_moves(&g.mvs_[0]), k = 0;
    if (n == ch2k::game::GEN_MOVES_OUT_OF_MEM) return 0;
    for (uint8_t i = 0; i < n && k < 32; i++)
        if (toSq(g.mvs_[i].fr()) == from) out[k++] = pack(g.mvs_[i]);
    return k;
}

Move findMove(uint8_t from, uint8_t to, uint8_t promo) {
    GUARD();
    uint8_t n = g.gen_moves(&g.mvs_[0]);
    if (n == ch2k::game::GEN_MOVES_OUT_OF_MEM) return NO_MOVE;
    for (uint8_t i = 0; i < n; i++) {
        ch2k::move m = g.mvs_[i];
        if (toSq(m.fr()) != from || toSq(m.to()) != to) continue;
        if (m.is_promotion() && ourType(m.promotion_piece_type().x) != promo) continue;
        return pack(m);
    }
    return NO_MOVE;
}

uint8_t moveCount() {
    GUARD();
    uint8_t n = g.gen_moves(&g.mvs_[0]);
    return n == ch2k::game::GEN_MOVES_OUT_OF_MEM ? 0 : n;
}

Status status() { GUARD(); return (Status)g.check_status(); }

Move think(const Level &lv) {
    GUARD();
    g.max_depth_ = 8;
    g.max_nodes_ = lv.nodes;
    g.margin_ = lv.margin;
    g.contempt_ = lv.contempt;
    g.stop_ = 0;
    g.npick_ = 0;
    book = g.gd.opening_index_ != 0;
    busy = true;
    g.ai_move();
    busy = false;
    if (g.stop_) return NO_MOVE;
    ch2k::move best = g.best_;
    if (!book && lv.margin > 0 && g.npick_ > 1) {
        // Any candidate within the margin of the best, uniformly: the
        // lower levels play plausible but beatable chess.
        ch2k::score floor = (ch2k::score)(g.score_ - lv.margin);
        uint8_t ok[CH2K_MAX_CANDIDATES], n = 0;
        for (uint8_t i = 0; i < g.npick_; i++)
            if (g.pick_score_[i] >= floor) ok[n++] = i;
        if (n) best = g.pick_[ok[ch2k::nrand(n)]];
    }
    return pack(best);
}

Move benchThink(const Level &lv) {
    uint16_t book = g.gd.opening_index_;
    void (*hook)() = pollHook;
    g.gd.opening_index_ = 0;
    pollHook = nullptr;
    Move m = think(lv);
    pollHook = hook;
    g.gd.opening_index_ = book;
    return m;
}

void abort() { g.abort(); }
bool aborted() { return g.stop_ != 0; }
int16_t lastScore() { return g.score_; }
uint32_t nodes() { return g.nodes_; }
bool thinking() { return busy; }

Move rootMove() {
    if (!busy || g.state_index < 1) return NO_MOVE;
    return pack(g.stack_base()[1].lastmove);
}

void seed(uint32_t s) { rng = s ? s : 0x9E3779B9u; }

#ifdef CHSIM
void loadFen(const char *fen) {
    GUARD();
    g.load_fen(fen);
    g.gd.opening_index_ = 0;
    g.gd.ply_ = 20;
}
#endif

}  // namespace eng
