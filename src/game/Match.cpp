#pragma GCC optimize("Os")   // cold code: size over speed
#include <string.h>
#include "Match.h"

namespace match {

// Node budget, how far below the best a move may score and still be picked
// (centipawns), contempt (16 cp units). The budgets are first guesses: they
// get tuned on the board so the strongest thinks for a few seconds.
const eng::Level LEVEL[LEVELS] = {
    {512, 150, 0},       // ROOKIE
    {2048, 60, 0},       // REGULAR
    {8192, 25, 0},       // SHARK
    {32768, 10, 2},      // HIGH ROLLER: plays for the win
    {131072, 0, 0},      // THE HOUSE
};

uint8_t board[64];
Setup setup;
uint8_t lastFrom = 0xFF, lastTo = 0xFF, checkSq = 0xFF;
Result result;
Reason reason;
uint16_t plies;

enum Phase : uint8_t { OFF, WAIT, HUMAN, CPU, CPU_PICKED, OVER };
static Phase phase;
static eng::Move cpuMove;
static bool abortReq, inThink;

// Moves since `base` (the start position, or a snapshot once the game gets
// long). Undo and saved games replay from it.
static const uint8_t HIST = 128;
static eng::Move hist[HIST];
static uint8_t nh;
static eng::Snap base;
static bool baseStart;

static Event q[8];
static uint8_t qHead, qCount;

static void push(uint8_t type, uint8_t a = 0, uint8_t b = 0) {
    if (qCount == 8) { qHead = (uint8_t)((qHead + 1) & 7); qCount--; }   // never stall: drop the oldest
    Event &e = q[(qHead + qCount++) & 7];
    memset(&e, 0, sizeof e);
    e.type = type; e.a = a; e.b = b;
}

bool peekEvent(Event &e) {
    if (!qCount) return false;
    e = q[qHead];
    return true;
}

bool popEvent(Event &e) {
    if (!qCount) return false;
    e = q[qHead];
    qHead = (uint8_t)((qHead + 1) & 7);
    qCount--;
    return true;
}

static bool isHuman(bool black) { return setup.mode == TWO_PLAYER || black == (setup.humanBlack != 0); }

static uint8_t kingSquare(bool black) {
    uint8_t k = (uint8_t)(eng::KING | (black ? eng::BLACK : 0));
    for (uint8_t s = 0; s < 64; s++) if (board[s] == k) return s;
    return 0xFF;
}

static void sync() {
    for (uint8_t s = 0; s < 64; s++) board[s] = eng::pieceAt(s);
}

static void replay(uint8_t n) {
    if (baseStart) eng::newGame(); else eng::restore(base);
    for (uint8_t i = 0; i < n; i++) eng::play(hist[i]);
}

// Fold the oldest k moves into the base snapshot.
static void rebase(uint8_t k) {
    replay(k);
    eng::snapshot(base);
    baseStart = false;
    memmove(hist, hist + k, (size_t)(nh - k) * sizeof hist[0]);
    nh = (uint8_t)(nh - k);
    replay(nh);
}

static void setLast() {
    if (nh) { lastFrom = eng::from(hist[nh - 1]); lastTo = eng::to(hist[nh - 1]); }
    else lastFrom = lastTo = 0xFF;
}

void start(const Setup &s) {
    setup = s;
    eng::seed(s.seed);
    eng::newGame();
    baseStart = true;
    nh = 0;
    plies = 0;
    qHead = qCount = 0;
    result = PLAYING;
    abortReq = false;
    checkSq = 0xFF;
    lastFrom = lastTo = 0xFF;
    sync();
    push(EV_START);
    phase = WAIT;
}

#ifdef CHSIM
void startFen(const Setup &s, const char *fen) {
    start(s);
    eng::loadFen(fen);
    eng::snapshot(base);             // the position becomes the base: undo stops here
    baseStart = false;
    sync();
    qHead = qCount = 0;
    push(EV_START);
}
#endif

bool active() { return phase != OFF && phase != OVER; }
bool blackToMove() { return eng::blackToMove(); }
bool humanToMove() { return phase == HUMAN; }
bool cpuThinking() { return inThink; }
int material() { return eng::materialBalance(); }

static void finish(Result r, Reason why) {
    result = r;
    reason = why;
    phase = OVER;
    push(EV_OVER, r, why);
}

// After a move (or on starting): verdicts, then the next turn.
static bool judge() {
    eng::Status st = eng::status();
    bool black = eng::blackToMove();
    checkSq = (st == eng::CHECK || st == eng::MATED) ? kingSquare(black) : 0xFF;
    switch (st) {
        case eng::MATED:     finish(black ? WHITE_WINS : BLACK_WINS, BY_MATE); return false;
        case eng::STALEMATE: finish(STALEMATE, BY_RULE); return false;
        case eng::DRAW_50:   finish(DRAW_50, BY_RULE); return false;
        case eng::DRAW_REPETITION: finish(DRAW_REPETITION, BY_RULE); return false;
        case eng::DRAW_MATERIAL:   finish(DRAW_MATERIAL, BY_RULE); return false;
        default: return true;
    }
}

static void doMove(eng::Move m) {
    uint8_t f = eng::from(m), t = eng::to(m);
    push(EV_MOVE, f, t);
    Event &e = q[(qHead + qCount - 1) & 7];
    e.piece = board[f];
    e.capSq = 0xFF;
    e.rookFrom = e.rookTo = 0xFF;
    if (board[t]) { e.captured = board[t]; e.capSq = t; }
    if (eng::isEnPassant(m)) {
        e.capSq = (uint8_t)((f & 0x38) | (t & 7));
        e.captured = board[e.capSq];
    }
    if (eng::isCastle(m)) {
        bool kingside = (t & 7) > (f & 7);
        e.rookFrom = (uint8_t)((f & 0x38) | (kingside ? 7 : 0));
        e.rookTo = (uint8_t)((f & 0x38) | (kingside ? 5 : 3));
    }
    e.promo = eng::isPromotion(m) ? eng::promotion(m) : 0;

    if (nh == HIST) rebase(HIST / 2);       // (replays: before playing m)
    eng::play(m);
    hist[nh++] = m;
    plies++;
    lastFrom = f; lastTo = t;
    sync();
    if (judge()) {
        if (checkSq != 0xFF) push(EV_CHECK, checkSq);
        phase = WAIT;
    }
}

void update(bool stageBusy) {
    switch (phase) {
        case WAIT:
            if (stageBusy || qCount) break;
            {
                bool black = eng::blackToMove();
                bool human = isHuman(black);
                push(EV_TURN, black, human);
                phase = human ? HUMAN : CPU;
            }
            break;
        case CPU:
            if (stageBusy || qCount) break;
            push(EV_THINK);
            inThink = true;
            cpuMove = eng::think(LEVEL[setup.level]);
            inThink = false;
            if (abortReq || cpuMove == eng::NO_MOVE) {
                abortReq = false;
                phase = WAIT;          // whatever asked for the abort decides what's next
                break;
            }
            push(EV_PICK, eng::from(cpuMove), eng::to(cpuMove));
            phase = CPU_PICKED;
            break;
        case CPU_PICKED:
            if (stageBusy || qCount) break;
            doMove(cpuMove);
            break;
        default:
            break;
    }
}

uint8_t movesFrom(uint8_t from, uint8_t *to, uint8_t *capture) {
    if (phase != HUMAN) return 0;
    eng::Move ms[32];
    uint8_t n = eng::movesFrom(from, ms), k = 0;
    for (uint8_t i = 0; i < n; i++) {
        uint8_t t = eng::to(ms[i]);
        bool dup = false;
        for (uint8_t j = 0; j < k; j++) if (to[j] == t) dup = true;      // promotions: one per square
        if (dup) continue;
        to[k] = t;
        capture[k] = board[t] != 0 || eng::isEnPassant(ms[i]);
        k++;
    }
    return k;
}

bool needsPromotion(uint8_t from, uint8_t to) {
    return (board[from] & eng::TYPE) == eng::PAWN && ((to >> 3) == 0 || (to >> 3) == 7);
}

bool play(uint8_t from, uint8_t to, uint8_t promo) {
    if (phase != HUMAN) return false;
    eng::Move m = eng::findMove(from, to, needsPromotion(from, to) ? promo : 0);
    if (m == eng::NO_MOVE) return false;
    doMove(m);
    return true;
}

// Plies to take back so that a human is to move again.
static uint8_t undoCount() {
    if (setup.mode == TWO_PLAYER) return nh ? 1 : 0;
    // Last mover: the side not to move now (or, after a pause mid-think, the human).
    bool lastWasHuman = isHuman(!eng::blackToMove());
    uint8_t k = lastWasHuman ? 1 : 2;
    return nh >= k ? k : 0;
}

bool canUndo() { return !inThink && phase != OFF && undoCount() > 0; }

bool undo() {
    if (!canUndo()) return false;
    uint8_t k = undoCount();
    nh = (uint8_t)(nh - k);
    plies = (uint16_t)(plies - k);
    replay(nh);
    sync();
    setLast();
    result = PLAYING;
    qHead = qCount = 0;
    judge();
    push(EV_START);
    phase = WAIT;
    return true;
}

void resign() {
    if (!active()) return;
    bool black = setup.mode == TWO_PLAYER ? eng::blackToMove() : setup.humanBlack != 0;
    finish(black ? WHITE_WINS : BLACK_WINS, BY_RESIGNATION);
}

void abortThink() {
    if (!inThink) return;
    abortReq = true;
    eng::abort();
}

// ---------------------------------------------------------------------------
// Saved games
// ---------------------------------------------------------------------------
void save(Record &r) {
    const uint8_t cap = sizeof r.m / sizeof r.m[0];
    if (nh > cap) rebase((uint8_t)(nh - cap));
    r.base = base;
    r.baseIsStart = baseStart;
    r.mode = setup.mode; r.humanBlack = setup.humanBlack; r.level = setup.level;
    r.n = nh;
    memcpy(r.m, hist, nh * sizeof hist[0]);
}

bool load(const Record &r) {
    const uint8_t cap = sizeof r.m / sizeof r.m[0];
    if (r.n > cap || r.mode > TWO_PLAYER || r.level >= LEVELS) return false;
    Setup s = {r.mode, r.humanBlack, r.level, setup.seed};
    start(s);
    base = r.base;
    baseStart = r.baseIsStart != 0;
    // Replay, checking every move is legal where it is played.
    if (baseStart) eng::newGame(); else eng::restore(base);
    for (uint8_t i = 0; i < r.n; i++) {
        eng::Move m = r.m[i];
        if (eng::findMove(eng::from(m), eng::to(m), eng::isPromotion(m) ? eng::promotion(m) : 0) != m) {
            start(s);
            return false;
        }
        eng::play(m);
        hist[i] = m;
    }
    nh = r.n;
    plies = baseStart ? nh : (uint16_t)(base.ply + nh);
    sync();
    setLast();
    qHead = qCount = 0;
    if (!judge()) return false;
    push(EV_START);
    phase = WAIT;
    return true;
}

}  // namespace match
