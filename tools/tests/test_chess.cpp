// Host tests for the chess engine port and the game logic.
//
//   python tools/tests/run_tests.py
//
// Engine.cpp is compiled into this file (one translation unit) so the tests
// can reach ch2k's internals for perft and the book walk.
#define CH2K_EXTRAS 1
#define ENG_CHECKS 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../../src/engine/Engine.cpp"
static ch2k::game &g = eng::g;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); \
    if (_a != _b) { printf("FAIL %s:%d: %s = %lld, expected %lld\n", __FILE__, __LINE__, #a, _a, _b); failures++; } } while (0)

// ---------------------------------------------------------------------------
// perft
// ---------------------------------------------------------------------------
static bool oom;
static uint64_t perft(ch2k::game &gm, ch2k::move *m, int depth) {
    uint8_t n = gm.gen_moves(m);
    if (n == ch2k::game::GEN_MOVES_OUT_OF_MEM) { oom = true; return 0; }
    if (depth == 1) return n;
    uint64_t t = 0;
    for (uint8_t i = 0; i < n; i++) {
        gm.do_move(m[i]);
        t += perft(gm, m + n, depth - 1);
        gm.undo_move(m[i]);
    }
    return t;
}

struct PerftCase { const char *name, *fen; int depth; uint64_t nodes[6]; };
static const PerftCase PERFT[] = {
    {"start", "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq -", 5,
     {20, 400, 8902, 197281, 4865609}},
    {"kiwipete", "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq -", 4,
     {48, 2039, 97862, 4085603}},
    {"pos3", "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - -", 5,
     {14, 191, 2812, 43238, 674624}},
    {"pos4", "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq -", 4,
     {6, 264, 9467, 422333}},
    {"pos5", "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ -", 4,
     {44, 1486, 62379, 2103487}},
};

static void testPerft() {
    for (auto &c : PERFT) {
        g.load_fen(c.fen);
        for (int d = 1; d <= c.depth; d++) {
            oom = false;
            uint64_t n = perft(g, &g.mvs_[0], d);
            CHECK(!oom);
            if (n != c.nodes[d - 1]) {
                printf("FAIL perft %s d%d = %llu, expected %llu\n", c.name, d,
                       (unsigned long long)n, (unsigned long long)c.nodes[d - 1]);
                failures++;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Draw rules, including the material-draw fix
// ---------------------------------------------------------------------------
static uint8_t statusOf(const char *fen) { g.load_fen(fen); return g.check_status(); }

static void testDraws() {
    using G = ch2k::game;
    // The upstream bug: three points a side ended the game as a draw.
    CHECK_EQ(statusOf("4k3/ppp5/8/8/8/8/PPP5/4K3 w - -"), G::STATUS_NORMAL);   // K+3P v K+3P
    CHECK_EQ(statusOf("4k3/8/8/3n4/8/8/8/2B1K3 w - -"), G::STATUS_NORMAL);     // KB v KN
    CHECK_EQ(statusOf("4k3/8/8/4b3/8/8/8/3BK3 w - -"), G::STATUS_NORMAL);      // opposite bishops (e5, d1)
    CHECK_EQ(statusOf("4k3/8/8/3b4/8/8/8/3BK3 w - -"), G::STATUS_DRAW_MATERIAL); // same colour (d5, d1)
    CHECK_EQ(statusOf("4k3/8/8/8/8/8/8/4K3 w - -"), G::STATUS_DRAW_MATERIAL);  // KvK
    CHECK_EQ(statusOf("4k3/8/8/8/8/8/8/3NK3 w - -"), G::STATUS_DRAW_MATERIAL); // KNvK
    CHECK_EQ(statusOf("4k3/8/8/8/8/8/8/3BK3 w - -"), G::STATUS_DRAW_MATERIAL); // KBvK
    CHECK_EQ(statusOf("4k3/8/8/8/8/8/4P3/4K3 w - -"), G::STATUS_NORMAL);       // KPvK
    CHECK_EQ(statusOf("7k/5Q2/6K1/8/8/8/8/8 b - -"), G::STATUS_DRAW_STALEMATE);
    CHECK_EQ(statusOf("7k/6Q1/6K1/8/8/8/8/8 b - -"), G::STATUS_MATED);
}

// ---------------------------------------------------------------------------
// Opening book: every entry must be a legal move where it is played
// ---------------------------------------------------------------------------
static int bookEntries;
static void walkBook(const ch2k::game &at, uint16_t i, int depth) {
    for (;;) {
        uint8_t d = ch2k::OPENING_BOOK[i - 1];
        ch2k::game gm = at;
        uint8_t n = gm.gen_moves(&gm.mvs_[0]);
        CHECK((d & 0x3f) < n);
        bookEntries++;
        if ((d & 0x80) && (d & 0x3f) < n && depth < 12) {
            gm.execute_move(gm.mvs_[d & 0x3f]);
            walkBook(gm, (uint16_t)(i + 1), depth + 1);
        }
        if (!ch2k::book_advance_to_next_sibling(i)) break;
    }
}

static void testBook() {
    g.new_game();
    bookEntries = 0;
    walkBook(g, 1, 0);
    CHECK_EQ(bookEntries, (int)sizeof ch2k::OPENING_BOOK);
    // Book moves come out in play: the first CPU move is always from it.
    eng::seed(1);
    eng::newGame();
    eng::Level lv = {512, 0, 0};
    eng::Move m = eng::think(lv);
    CHECK(m != eng::NO_MOVE);
    CHECK(eng::nodes() == 0);                 // no search ran
}

// ---------------------------------------------------------------------------
// Snapshot / restore
// ---------------------------------------------------------------------------
static int cmpMoves(const void *a, const void *b) {
    return (int)*(const eng::Move *)a - (int)*(const eng::Move *)b;
}

static uint32_t lcg = 12345;
static uint32_t rnd() { lcg = lcg * 1103515245u + 12345u; return lcg >> 8; }

static void testSnapshot() {
    eng::Move ms[256], ms2[256];
    for (int game = 0; game < 200; game++) {
        eng::newGame();
        int plies = (int)(rnd() % 80);
        for (int p = 0; p < plies; p++) {
            uint8_t n = eng::legal(ms);
            if (!n || eng::status() > eng::CHECK) break;
            eng::play(ms[rnd() % n]);
        }
        // movers(): exactly the squares legal moves start from, once each.
        {
            uint8_t n = eng::legal(ms), sq[64], k = eng::movers(sq);
            bool from[64] = {}, seen[64] = {};
            for (uint8_t i = 0; i < n; i++) from[eng::from(ms[i])] = true;
            int count = 0;
            for (int q = 0; q < 64; q++) count += from[q];
            CHECK_EQ(k, count);
            for (uint8_t i = 0; i < k; i++) { CHECK(from[sq[i]] && !seen[sq[i]]); seen[sq[i]] = true; }
        }
        eng::Snap s;
        eng::snapshot(s);
        uint8_t board[64];
        for (int q = 0; q < 64; q++) board[q] = eng::pieceAt((uint8_t)q);
        uint8_t n = eng::legal(ms);
        bool black = eng::blackToMove();
        int mat = eng::materialBalance();
        eng::newGame();
        eng::restore(s);
        for (int q = 0; q < 64; q++) CHECK_EQ(eng::pieceAt((uint8_t)q), board[q]);
        CHECK_EQ(eng::blackToMove(), black);
        CHECK_EQ(eng::materialBalance(), mat);
        uint8_t n2 = eng::legal(ms2);
        CHECK_EQ(n2, n);
        // Same moves, whatever order the rebuilt piece lists give.
        qsort(ms, n, sizeof ms[0], cmpMoves);
        qsort(ms2, n2, sizeof ms2[0], cmpMoves);
        CHECK(n == n2 && memcmp(ms, ms2, n * sizeof ms[0]) == 0);
    }
}

// ---------------------------------------------------------------------------
// The CPU: legal moves, polls, abort, determinism, games that end
// ---------------------------------------------------------------------------
static int polls;
static bool pollAbortAt;
static void countPoll() {
    polls++;
    CHECK(eng::thinking());
    eng::Move r = eng::rootMove();     // the one engine read allowed mid-search
    (void)r;
    if (pollAbortAt && polls == 3) eng::abort();
}

static bool isLegal(eng::Move m) {
    eng::Move ms[256];
    uint8_t n = eng::legal(ms);
    for (uint8_t i = 0; i < n; i++) if (ms[i] == m) return true;
    return false;
}

static const eng::Level LEVELS[] = {
    {512, 150, 0}, {2048, 60, 0}, {8192, 25, 0}, {32768, 10, 2}, {131072, 0, 0},
};

static void testAi() {
    eng::pollHook = countPoll;
    // Out of the book, every level returns a legal move and polls.
    const char *fens[] = {
        "r1bqkbnr/pppp1ppp/2n5/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq -",
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq -",
        "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - -",
    };
    for (auto f : fens) {
        for (auto &lv : LEVELS) {
            g.load_fen(f);
            g.gd.opening_index_ = 0;
            g.gd.ply_ = 20;
            polls = 0;
            eng::Move m = eng::think(lv);
            CHECK(m != eng::NO_MOVE);
            CHECK(isLegal(m));
            CHECK(polls >= (int)(eng::nodes() / 256) - 1);
            if (lv.nodes >= 8192) CHECK(polls > 0);
        }
    }
    // Abort from the hook: no move, and the engine is usable afterwards.
    g.load_fen(fens[1]); g.gd.opening_index_ = 0;
    polls = 0; pollAbortAt = true;
    eng::Move m = eng::think(LEVELS[4]);
    pollAbortAt = false;
    CHECK(m == eng::NO_MOVE);
    CHECK(eng::aborted());
    m = eng::think(LEVELS[0]);
    CHECK(m != eng::NO_MOVE && isLegal(m));
    // Mate in one is found at every level with a margin below a mate score.
    for (auto &lv : LEVELS) {
        g.load_fen("6k1/5ppp/8/8/8/8/5PPP/3R2K1 w - -");
        g.gd.opening_index_ = 0;
        m = eng::think(lv);
        eng::play(m);
        CHECK_EQ(eng::status(), eng::MATED);
    }
    eng::pollHook = nullptr;
}

// Full games at low budgets end by the rules, and the same seed plays the
// same game.
static uint32_t playGame(uint32_t seed, int &plies, uint8_t &result) {
    eng::seed(seed);
    eng::newGame();
    uint32_t h = 2166136261u;
    plies = 0;
    for (;;) {
        eng::Status st = eng::status();
        if (st > eng::CHECK) { result = st; break; }
        eng::Level lv = LEVELS[(plies + seed) % 2];
        eng::Move m = eng::think(lv);
        if (m == eng::NO_MOVE || !isLegal(m)) { result = 255; break; }
        eng::play(m);
        h = (h ^ m) * 16777619u;
        if (++plies > 1200) { result = 254; break; }
    }
    return h;
}

static void testGames() {
    int counts[8] = {};
    for (uint32_t s = 1; s <= 40; s++) {
        int plies; uint8_t result;
        uint32_t h = playGame(s, plies, result);
        CHECK(result != 255 && result != 254);
        if (result < 8) counts[result]++;
        if (s <= 3) {
            int plies2; uint8_t result2;
            CHECK_EQ(playGame(s, plies2, result2), h);
        }
    }
    printf("  40 CPU games: mate %d, stalemate %d, 50-move %d, repetition %d, material %d\n",
           counts[eng::MATED], counts[eng::STALEMATE], counts[eng::DRAW_50],
           counts[eng::DRAW_REPETITION], counts[eng::DRAW_MATERIAL]);
}

// ---------------------------------------------------------------------------
// Match: the game flow with a stage that is never busy
// ---------------------------------------------------------------------------
#include "../../src/game/Match.h"

static bool sameBoard(const uint8_t *a) {
    for (int s = 0; s < 64; s++) if (a[s] != eng::pieceAt((uint8_t)s)) return false;
    return true;
}

// A random legal human move through the UI-facing API.
static bool humanRandomMove() {
    uint8_t from[64], nf = 0, to[32], cap[32];
    for (uint8_t s = 0; s < 64; s++) {
        uint8_t p = match::board[s];
        if (p && ((p & eng::BLACK) != 0) == match::blackToMove() && match::movesFrom(s, to, cap)) from[nf++] = s;
    }
    if (!nf) return false;
    uint8_t f = from[rnd() % nf];
    uint8_t n = match::movesFrom(f, to, cap);
    uint8_t t = to[rnd() % n];
    static const uint8_t PROMO[4] = {eng::QUEEN, eng::KNIGHT, eng::ROOK, eng::BISHOP};
    return match::play(f, t, PROMO[rnd() & 3]);
}

static int runMatch(const match::Setup &s, int maxPlies, int undoEvery) {
    match::start(s);
    match::Event e;
    int guard = 0, moves = 0;
    uint8_t before[64];
    while (match::active() && guard++ < 20000) {
        match::update(false);
        while (match::popEvent(e)) {
            if (e.type == match::EV_MOVE) {
                moves++;
                CHECK(e.piece != 0);
                CHECK(match::board[e.b] != 0);
                CHECK(match::board[e.a] == 0);
            }
        }
        if (match::humanToMove()) {
            if (undoEvery && moves && rnd() % undoEvery == 0 && match::canUndo()) {
                memcpy(before, match::board, 64);
                CHECK(match::undo());
                CHECK(sameBoard(match::board));
                continue;
            }
            CHECK(humanRandomMove());
        }
        if ((int)match::plies > maxPlies) break;
    }
    CHECK(guard < 20000);
    return moves;
}

static void testMatch() {
    // Hot-seat games of random moves, with undo sprinkled in.
    int total = 0;
    for (uint32_t i = 0; i < 300; i++) {
        match::Setup s = {match::TWO_PLAYER, 0, 0, i + 1};
        total += runMatch(s, 400, 7);
        CHECK(!match::active() || match::plies > 400);
    }
    // Against the CPU, from both sides, at the two lowest levels.
    for (uint32_t i = 0; i < 40; i++) {
        match::Setup s = {match::VS_CPU, (uint8_t)(i & 1), (uint8_t)(i % 2), i + 100};
        total += runMatch(s, 300, 11);
    }
    printf("  %d moves through Match\n", total);

    // Long games fold history into a snapshot; save/load replays the rest.
    for (uint32_t i = 0; i < 30; i++) {
        match::Setup s = {match::TWO_PLAYER, 0, 0, i + 500};
        match::start(s);
        match::Event e;
        int target = 20 + (int)(rnd() % 260);
        while (match::active() && (int)match::plies < target) {
            match::update(false);
            while (match::popEvent(e)) {}
            if (match::humanToMove()) humanRandomMove();
        }
        if (!match::active()) continue;
        uint8_t b[64];
        memcpy(b, match::board, 64);
        bool black = match::blackToMove();
        static match::Record r;
        match::save(r);
        CHECK(sameBoard(b));                  // saving leaves the game as it was
        match::Setup other = {match::TWO_PLAYER, 0, 0, 1};
        match::start(other);
        CHECK(match::load(r));
        CHECK(memcmp(b, match::board, 64) == 0);
        CHECK(match::blackToMove() == black);
    }
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);   // a trap (UBSan, hands-off guard) keeps what was printed
    printf("perft...\n");      testPerft();
    printf("draws...\n");      testDraws();
    printf("book...\n");       testBook();
    printf("snapshot...\n");   testSnapshot();
    printf("ai...\n");         testAi();
    printf("games...\n");      testGames();
    printf("match...\n");      testMatch();
    printf(failures ? "%d FAILURES\n" : "all tests passed\n", failures);
    return failures ? 1 : 0;
}
