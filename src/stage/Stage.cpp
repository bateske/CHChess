#pragma GCC optimize("Os")   // cold code: size over speed (hot pixel loops live in Draw/Iso)
#include <string.h>
#include <CHGfx.h>
#include "Stage.h"
#include "../game/Match.h"
#include "../engine/Engine.h"
#include "../iso/Iso.h"
#include "../gfx/Draw.h"
#include "../gfx/Palette.h"
#include "../gfx/Fmt.h"
#include "../fx/Fx.h"
#include "../audio/Audio.h"
#include "../assets/Assets.h"
#ifdef CHSIM
#include <sim.h>
namespace stage { extern uint64_t simLap[8]; }
#endif

namespace stage {

using namespace iso;

// ---------------------------------------------------------------------------
// Colour remaps for the art (its neutral tones -> a side, and effects)
// ---------------------------------------------------------------------------
static uint8_t RM_W[16], RM_B[16], RM_HIT[16], RM_CPU[16], RM_ID[16], RM_ICON_B[16];

static void initRemaps() {
    for (uint8_t i = 0; i < 16; i++) RM_W[i] = RM_B[i] = RM_CPU[i] = RM_ID[i] = RM_ICON_B[i] = i, RM_HIT[i] = WHITE;
    RM_ICON_B[WHITE] = INK; RM_ICON_B[INK] = SILVER;
    RM_W[NAVY] = SILVER; RM_W[SILVER] = WHITE; RM_W[CYAN] = WHITE;
    RM_B[BLUE] = INK; RM_B[SILVER] = NAVY; RM_B[WHITE] = BLUE; RM_B[CYAN] = SILVER;
    RM_HIT[INK] = INK;
    RM_CPU[GOLD] = RED; RM_CPU[WOOD] = WINE;
}

static const uint8_t *remapFor(uint8_t p) { return (p & eng::BLACK) ? RM_B : RM_W; }
static const PieceArt &art(uint8_t p) { return PIECE_ART[(p & eng::TYPE) - 1]; }

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static uint8_t shown[64];            // pieces standing still
static uint8_t cur = 12, curW = 12, curB = 52;
static uint8_t sel = 0xFF, nTgt, tgt[32], tgtCap[32];
static uint8_t wiggleSq = 0xFF, wiggleT;
static uint8_t intent = 0xFF;        // the CPU's chosen destination, shown while it moves
static bool strat, hints = true, fast;
static bool cpuTurn, humanTurn, thinking;

struct Mover { uint8_t piece, from, to, t, T, arc, delay, on; };
static Mover mv[2];

// A captured piece, knocked off and tumbling (world position, Q4).
struct Flyer { uint8_t piece, t, on; int8_t spin; int16_t x, y, z, vx, vz; uint8_t ang; };
static Flyer fly;

// Toppling king at mate.
static uint8_t topSq = 0xFF, topT;
static int8_t topDir;

// The CPU's pointing finger and the player's; one shows at a time.
static int32_t fx16, fy16;           // finger tip, world, Q4
static uint8_t fingerSq = 0xFF;
static uint8_t dwell, thinkT, pickT, pickFrom = 0xFF, tapT;
static bool picking;

static uint8_t holdT;                // frames the stage keeps the game waiting
// 2P hand-over: the camera whips to the board's centre, the view turns
// round behind a quick dip to dark, then it swoops onto the new side.
static uint8_t handT;
static bool handBlack;
static uint8_t dipT;                 // palette dip (view changes)
static bool turnBlack;               // whose turn the HUD shows
static uint8_t overT;
static bool over, overDone;
const char *opponentName = "CPU";

// Camera (world, Q4) and its target.
static int32_t cx16, cy16;
static int16_t aimX, aimY;
static uint8_t aimShift = 2;


// ---------------------------------------------------------------------------
// Geometry helpers
// ---------------------------------------------------------------------------
static void sqWorld(uint8_t sq, int &x, int &y) {
    int u, v;
    toView(sq, cam.flip, u, v);
    x = worldX(u, v);
    y = worldTop(u, v) + HH;
}

static int depthOf(uint8_t sq) {
    int u, v;
    toView(sq, cam.flip, u, v);
    return u + v;
}

static void aim(int x, int y, uint8_t shift) { aimX = (int16_t)x; aimY = (int16_t)(y - 12); aimShift = shift; }
static void aimSq(uint8_t sq, uint8_t shift) { int x, y; sqWorld(sq, x, y); aim(x, y, shift); }

static void snapCamera() {
    cx16 = aimX << 4; cy16 = aimY << 4;
    cam.x = aimX; cam.y = aimY;
}

static void stepCamera() {
    int32_t dx = (aimX << 4) - cx16, dy = (aimY << 4) - cy16;
    cx16 += dx >> aimShift; cy16 += dy >> aimShift;
    if (dx > -16 && dx < 16) cx16 = aimX << 4;
    if (dy > -16 && dy < 16) cy16 = aimY << 4;
    cam.x = (int)(cx16 >> 4); cam.y = (int)(cy16 >> 4);
}

// Height of the piece on a square (finger tip rests just above it).
static int topOf(uint8_t sq) {
    uint8_t p = shown[sq];
    return p ? art(p).ay + 2 : 4;
}

static uint8_t pieceValue(uint8_t p) {
    static const uint8_t V[7] = {0, 1, 3, 3, 5, 9, 0};
    return V[p & eng::TYPE];
}

// ---------------------------------------------------------------------------
// Public controls
// ---------------------------------------------------------------------------
uint8_t cursor() { return cur; }
bool flipped() { return cam.flip; }
bool strategy() { return strat; }
void setStrategy(bool on) { strat = on; dipT = 6; }
void setHints(bool on) { hints = on; }
void setCoords(bool on) { iso::coords = on; }
void setFast(bool on) { fast = on; }
uint8_t selected() { return sel; }

void setCursor(uint8_t sq) {
    cur = sq;
    if (!strat) aimSq(sq, 2);
}

void moveCursor(int du, int dv) {
    int u, v;
    toView(cur, cam.flip, u, v);
    u += du; v += dv;
    if (u < 0 || u > 7 || v < 0 || v > 7) { audio::sfx(Sfx::Deny); return; }
    setCursor(fromView(u, v, cam.flip));
    audio::sfx(Sfx::Cursor);
}

void select(uint8_t sq, const uint8_t *to, const uint8_t *cap, uint8_t n) {
    sel = sq;
    nTgt = n;
    memcpy(tgt, to, n);
    memcpy(tgtCap, cap, n);
    pal::setMode(pal::TARGETS);
    audio::sfx(Sfx::Select);
    int x, y;
    screenOf(sq, x, y);
    fx::burst(fx::STAR, x, y - 20, 6, 24, GOLD);
}

void deselect() {
    sel = 0xFF;
    nTgt = 0;
    pal::setMode(pal::CASINO);
}

void deny(uint8_t sq) {
    wiggleSq = sq;
    wiggleT = 14;
    audio::sfx(Sfx::Deny);
}

bool busy() { return mv[0].on || mv[1].on || fly.on || holdT || picking || topT || handT; }
bool overShown() { return overDone; }

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------
static void resetFromBoard() {
    memcpy(shown, match::board, 64);
    holdT = 0; picking = false; thinking = false; tapT = 0;
    fx::clear();
    mv[0].on = mv[1].on = 0;
    fly.on = 0;
    topT = 0; topSq = 0xFF;
    over = overDone = false;
    overT = 0;
    intent = 0xFF;
    deselect();
}

static void onStart() {
    resetFromBoard();
    handT = 0;
    turnBlack = match::blackToMove();
    bool black = match::setup.mode == match::VS_CPU ? match::setup.humanBlack : match::blackToMove();
    cam.flip = black;
    curW = 12; curB = 52;                     // e2 / e7
    cur = black ? curB : curW;
    aimSq(cur, 2);
    snapCamera();
    fx16 = (int32_t)aimX << 4; fy16 = (int32_t)aimY << 4;
}

static void onTurn(bool black, bool human) {
    humanTurn = human;
    cpuTurn = !human;
    turnBlack = black;
    intent = 0xFF;
    if (!human) return;
    if (match::setup.mode == match::TWO_PLAYER) {
        // Hand the board to the other player.
        if (cam.flip) curB = cur; else curW = cur;
        cur = black ? curB : curW;
        if (cam.flip != black && !strat) {
            handT = 1;
            handBlack = black;
            humanTurn = false;           // no finger until the camera arrives
            aim(0, 8 * HH, 1);           // the board's centre, fast
            audio::sfx(Sfx::Whoosh);
        } else {
            cam.flip = black;
            aimSq(cur, 2);
        }
    } else {
        aimSq(cur, 2);
        audio::sfx(Sfx::Turn);
    }
    fingerSq = cur;
}

static void onThink() {
    thinking = true;
    thinkT = 0;
    dwell = 0;
    // Start from the CPU's king.
    uint8_t k = (uint8_t)(eng::KING | (match::blackToMove() ? eng::BLACK : 0));
    for (uint8_t s = 0; s < 64; s++) if (shown[s] == k) fingerSq = s;
}

static void onPick(uint8_t from, uint8_t to) {
    thinking = false;
    picking = true;
    pickFrom = from;
    pickT = 0;
    tapT = 0;
    fingerSq = from;
    intent = to;
}

static void launch(Mover &m, uint8_t piece, uint8_t from, uint8_t to, uint8_t arc, uint8_t delay) {
    int df = (from & 7) - (to & 7), dr = (from >> 3) - (to >> 3);
    if (df < 0) df = -df;
    if (dr < 0) dr = -dr;
    int d = df > dr ? df : dr;
    m.piece = piece; m.from = from; m.to = to; m.t = 0;
    m.T = (uint8_t)(fast ? 12 + d * 2 : 16 + d * 4);
    m.arc = arc; m.delay = delay; m.on = 1;
}

static match::Event pending;          // the move being shown (captures resolve on landing)

static void onMove(const match::Event &e) {
    pending = e;
    humanTurn = cpuTurn = false;
    thinking = false;
    deselect();
    shown[e.a] = 0;
    bool knight = (e.piece & eng::TYPE) == eng::KNIGHT;
    launch(mv[0], e.piece, e.a, e.b, knight ? 24 : 7, 0);
    if (knight) audio::sfx(Sfx::Hop);
    if (e.rookFrom != 0xFF) {
        uint8_t rook = shown[e.rookFrom];
        shown[e.rookFrom] = 0;
        launch(mv[1], rook, e.rookFrom, e.rookTo, 20, 10);
        audio::sfx(Sfx::Castle);
    }
}

static void land(Mover &m, bool main) {
    m.on = 0;
    int x, y;
    screenOf(m.to, x, y);
    if (!main) { shown[m.to] = m.piece; fx::burst(fx::DUST, x, y, 6, 20, SILVER); return; }
    const match::Event &e = pending;
    uint8_t piece = e.promo ? (uint8_t)(e.promo | (e.piece & eng::BLACK)) : e.piece;
    if (e.captured) {
        // Knock the victim off: it flies on in the attacker's direction.
        shown[e.capSq] = 0;
        int ax, ay, bx, by;
        sqWorld(e.a, ax, ay); sqWorld(e.capSq, bx, by);
        fly.piece = e.captured;
        fly.x = (int16_t)(bx << 4); fly.y = (int16_t)(by << 4); fly.z = 0;
        fly.vx = (int16_t)(bx > ax ? 40 : bx < ax ? -40 : (fx::rnd() & 1 ? 32 : -32));
        fly.vz = 88;
        fly.spin = (int8_t)(fly.vx > 0 ? 11 : -11);
        fly.ang = 0; fly.t = 0; fly.on = 1;
        fx::burst(fx::SPARK, x, y - 14, 14, 44, GOLD);
        fx::burst(fx::STAR, x, y - 14, 5, 30, WHITE);
        fx::shake(10, 3);
        audio::sfx(Sfx::Capture);
        char buf[6] = "+";
        fmtInt(buf + 1, pieceValue(e.captured));
        fx::floatText(buf, x, y - 30, GOLD);
    } else {
        fx::shake(4, 1);
        audio::sfx(Sfx::Land);
    }
    fx::burst(fx::DUST, x, y, 8, 22, SILVER);
    shown[m.to] = piece;
    if (e.promo) {
        fx::burst(fx::STAR, x, y - 24, 12, 40, GOLD);
        fx::floatText("PROMOTED!", x, y - 44, FX_B);
        audio::sfx(Sfx::Promote);
        holdT = 30;
    }
    if (e.rookFrom != 0xFF) fx::floatText("CASTLE!", x, y - 44, WHITE);
    else if (e.capSq != 0xFF && e.capSq != e.b) fx::floatText("EN PASSANT!", x, y - 44, WHITE);
    intent = 0xFF;
    holdT = (uint8_t)(holdT > 12 ? holdT : 12);
}

static void onCheck(uint8_t sq) {
    fx::banner("CHECK!", fx::B_RED, 36, 70);
    audio::sfx(Sfx::Check);
    audio::led(audio::LED_TRIPLE);
    (void)sq;
    holdT = 40;
}

static void onOver(uint8_t result, uint8_t reason) {
    over = true;
    overT = 0;
    humanTurn = cpuTurn = thinking = false;
    bool humanWon = match::setup.mode == match::TWO_PLAYER ||
                    (result == match::WHITE_WINS) == (match::setup.humanBlack == 0);
    bool decisive = result == match::WHITE_WINS || result == match::BLACK_WINS;
    if (decisive && reason == match::BY_MATE) {
        topSq = match::checkSq;
        topT = 1;
        topDir = (int8_t)(fx::rnd() & 1 ? 1 : -1);
        aimSq(topSq, 3);
        fx::banner("CHECKMATE!", fx::B_RAINBOW, 34, 170);
    } else if (decisive) {
        fx::banner(result == match::WHITE_WINS ? "BLACK RESIGNS" : "WHITE RESIGNS", fx::B_WHITE, 36, 150);
    } else {
        fx::banner(result == match::STALEMATE ? "STALEMATE" : "DRAW", fx::B_CYAN, 36, 150);
        static const char *const WHY[] = {"", "", "", "", "50 MOVES", "REPETITION", "NO MATERIAL"};
        if (result >= match::DRAW_50) fx::floatText(WHY[result], 64, 60, WHITE);
    }
    if (decisive && humanWon) {
        audio::sfx(reason == match::BY_MATE ? Sfx::Mate : Sfx::Win);
        audio::led(audio::LED_PARTY);
        fx::fountain(40, 90, 20);
        fx::fountain(88, 90, 20);
    } else if (decisive) {
        audio::sfx(Sfx::Lose);
    } else {
        audio::sfx(Sfx::Draw);
    }
}

void begin() {
    initRemaps();
}

// ---------------------------------------------------------------------------
// Per tick
// ---------------------------------------------------------------------------
static void stepMover(Mover &m, bool main) {
    if (!m.on) return;
    if (m.delay) { m.delay--; return; }
    if (++m.t >= m.T) land(m, main);
}

static void moverPos(const Mover &m, int &x, int &y, int &z) {
    int ax, ay, bx, by;
    sqWorld(m.from, ax, ay);
    sqWorld(m.to, bx, by);
    int e = fx::ease(fx::IN_OUT, m.t, m.T);
    x = ax + (((bx - ax) * e) >> 8);
    y = ay + (((by - ay) * e) >> 8);
    // Lift, glide, drop: a sine arc plus a flat top for sliding pieces.
    int s = fx::isin((m.t * 128) / m.T);
    int lift = m.arc > 10 ? (m.arc * s) >> 8 : (m.t < 5 ? m.t * m.arc / 5 : (m.T - m.t < 5 ? (m.T - m.t) * m.arc / 5 : m.arc));
    z = lift;
}

void update(bool think) {
    match::Event e;
    // A new position (new game, undo, a restored game) cuts in on anything
    // still showing; everything else waits its turn.
    while (match::peekEvent(e) && (!busy() || e.type == match::EV_START)) {
        match::popEvent(e);
        switch (e.type) {
            case match::EV_START: onStart(); break;
            case match::EV_TURN:  onTurn(e.a != 0, e.b != 0); break;
            case match::EV_THINK: onThink(); break;
            case match::EV_PICK:  onPick(e.a, e.b); break;
            case match::EV_MOVE:  onMove(e); break;
            case match::EV_CHECK: onCheck(e.a); break;
            case match::EV_OVER:  onOver(e.a, e.b); break;
        }
    }

    if (holdT) holdT--;
    if (wiggleT) wiggleT--;
    stepMover(mv[0], true);
    stepMover(mv[1], false);

    if (fly.on) {
        fly.t++;
        fly.x += fly.vx; fly.y += fly.vx / 3;
        fly.z += fly.vz; fly.vz -= 7;
        fly.ang += fly.spin;
        int sx = (fly.x >> 4) - cam.x + CX, sy = (fly.y >> 4) - cam.y + CY - (fly.z >> 4);
        if (fly.t > 90 || sx < -30 || sx > 158 || sy > 190) fly.on = 0;
    }

    if (topT && topT < 60) topT++;
    if (topT >= 60 && over) topT = 0;           // stays down: drawn from topSq

    // Camera and fingers.
    if (mv[0].on && !mv[0].delay) {
        int x, y, z;
        moverPos(mv[0], x, y, z);
        aim(x, y, 2);
    }
    if (think) {
        thinkT = (uint8_t)(thinkT < 255 ? thinkT + 1 : 255);
        // Follow the root move being searched, without flitting about.
        eng::Move r = eng::rootMove();
        if (++dwell > 20 && r != eng::NO_MOVE) {
            uint8_t s = eng::from(r);
            if (s != fingerSq) { fingerSq = s; dwell = 0; }
        }
        aimSq(fingerSq, 3);
    }
    if (picking) {
        aimSq(pickFrom, 3);
        pickT++;
        uint8_t need = (uint8_t)(fast ? 8 : (thinkT < 36 ? 56 - thinkT : 20));
        if (pickT >= need && !tapT) { tapT = 1; audio::sfx(Sfx::Select); }
        if (tapT && ++tapT > 12) { picking = false; tapT = 0; }
    }
    if (handT) {
        handT++;
        if (handT == 12) {
            // Over the centre (the same world point from either side, and the
            // squares keep their colours): turn round behind the dip.
            cam.flip = handBlack;
            dipT = 8;
            fx::banner(handBlack ? "BLACK TO MOVE" : "WHITE TO MOVE", fx::B_GOLD, 40, 60);
        }
        if (handT == 14) aimSq(cur, 3);
        if (handT > 40) { handT = 0; humanTurn = true; fx16 = (int32_t)aimX << 4; fy16 = (int32_t)aimY << 4; }
    }
    if (dipT) {
        dipT--;
        pal::setFade((uint8_t)(dipT > 4 ? 16 - (8 - dipT) * 3 : 16 - dipT * 3));
    }
    if (humanTurn) fingerSq = cur;
    stepCamera();

    // The finger glides to its square.
    if (fingerSq != 0xFF) {
        int x, y;
        sqWorld(fingerSq, x, y);
        y -= topOf(fingerSq);
        fx16 += ((x << 4) - fx16) >> 1;
        fy16 += ((y << 4) - fy16) >> 1;
    }

    if (over && overT < 255 && ++overT > 150) overDone = true;
}

// ---------------------------------------------------------------------------
// Drawing: the iso scene
// ---------------------------------------------------------------------------
static bool isTarget(uint8_t sq, bool &cap) {
    for (uint8_t i = 0; i < nTgt; i++) if (tgt[i] == sq) { cap = tgtCap[i]; return true; }
    return false;
}

static void drawOverlays(uint32_t frame) {
    uint8_t ph = (uint8_t)(frame >> 2);
    if (match::lastFrom != 0xFF && !mv[0].on) {
        tileTint(match::lastFrom, 3, GOLD);
        tileBorder(match::lastTo, 2, GOLD, GOLD, 0);
    }
    if (match::checkSq != 0xFF && !mv[0].on) {
        tileTint(match::checkSq, 1, RED);
        tileBorder(match::checkSq, 1, RED, WHITE, ph);
    }
    if (intent != 0xFF) {
        tileTint(intent, 2, shown[intent] ? RED : CYAN);
        tileBorder(intent, 1, shown[intent] ? RED : CYAN, WHITE, ph);
    }
    if (sel != 0xFF && hints) {
        for (uint8_t i = 0; i < nTgt; i++) {
            if (tgtCap[i]) { tileTint(tgt[i], 2, RED); tileBorder(tgt[i], 1, FX_B, RED, ph); }
            else           { tileTint(tgt[i], 3, CYAN); tileBorder(tgt[i], 2, FX_A, CYAN, ph); }
        }
    }
    if (humanTurn) {
        if (sel != 0xFF) tileBorder(sel, 0, WHITE, WHITE, 0);
        tileBorder(cur, 0, sel != 0xFF ? WHITE : FX_B, GOLD, ph);
    }
}

// Pieces standing in front of the cursor, over its square, are ghosted.
static bool hidesCursor(uint8_t sq, int x, int y, const PieceArt &a) {
    if (!humanTurn || strat) return false;
    int d = depthOf(sq) - depthOf(cur);
    if (d < 1 || d > 4) return false;
    int cxs, cys;
    screenOf(cur, cxs, cys);
    int left = x - a.ax, top = y - a.ay, w = a.data[0];
    return cxs + 8 > left && cxs - 8 < left + w && cys - 4 > top;
}

static void drawPiece(uint8_t p, int x, int y, uint8_t flags, const uint8_t *remap) {
    const PieceArt &a = art(p);
    bool mirror = (p & eng::TYPE) == eng::KNIGHT && ((p & eng::BLACK) != 0) != cam.flip;
    int w = a.data[0];
    int left = mirror ? x - (w - 1 - a.ax) : x - a.ax;
    sprite4(a.data, left, y - a.ay, remap ? remap : remapFor(p), (uint8_t)(flags | (mirror ? SPR_MIRROR : 0)));
}

static void drawMover(const Mover &m) {
    int x, y, z;
    moverPos(m, x, y, z);
    int sx = x - cam.x + CX, sy = y - cam.y + CY;
    fillEllipse(sx, sy, 7, 3, INK);                 // its shadow stays on the board
    drawPiece(m.piece, sx, sy - z, 0, nullptr);
}

static void drawPieces(uint32_t frame) {
    // Movers go in at their current depth, between the diagonals.
    int md[2] = {99, 99};
    for (int k = 0; k < 2; k++) {
        if (!mv[k].on) continue;
        int da = depthOf(mv[k].from), db = depthOf(mv[k].to);
        int e = mv[k].delay ? 0 : fx::ease(fx::IN_OUT, mv[k].t, mv[k].T);
        md[k] = da + (((db - da) * e + (db > da ? 255 : 0)) >> 8);
    }
    for (int d = 0; d <= 14; d++) {
        for (int u = 0; u < 8; u++) {
            int v = d - u;
            if (v < 0 || v > 7) continue;
            uint8_t sq = fromView(u, v, cam.flip);
            uint8_t p = shown[sq];
            if (!p) continue;
            int x, y;
            screenOf(sq, x, y);
            if (x < -20 || x > 148 || y < -8 || y > 190) continue;
            const PieceArt &a = art(p);
            if (sq == topSq && topT) {
                // Checkmated: the king topples, bouncing as it lands.
                int e = fx::ease(fx::OUT_BOUNCE, topT, 30);
                spriteRot(a.data, a.ax, a.ay, x, y, (uint8_t)((topDir * 60 * e) >> 8), 256, remapFor(p), false);
                continue;
            }
            uint8_t flags = hidesCursor(sq, x, y, a) ? SPR_GHOST : 0;
            if (sq == sel) {
                fillEllipse(x, y, 8, 4, INK);
                y -= 6 + ((fx::isin((int)(frame >> 2) * 24) * 2) >> 8);
            }
            if (sq == wiggleSq && wiggleT) x += (fx::isin(wiggleT * 40) * 2) >> 8;
            bool cap = false;
            bool blink = sel != 0xFF && (frame & 8) && isTarget(sq, cap) && cap;
            drawPiece(p, x, y, flags, blink ? RM_HIT : nullptr);
        }
        for (int k = 0; k < 2; k++) if (mv[k].on && md[k] == d) drawMover(mv[k]);
    }
    if (fly.on) {
        int sx = (fly.x >> 4) - cam.x + CX, sy = (fly.y >> 4) - cam.y + CY - (fly.z >> 4);
        const PieceArt &a = art(fly.piece);
        spriteRot(a.data, a.data[0] / 2, a.data[1] / 2, sx, sy - a.ay / 2, fly.ang, 256,
                  fly.t < 4 ? RM_HIT : remapFor(fly.piece), false);
    }
}

static void drawFinger(uint32_t frame) {
    if (fingerSq == 0xFF || (!humanTurn && !thinking && !picking)) return;
    int x = (int)(fx16 >> 4) - cam.x + CX, y = (int)(fy16 >> 4) - cam.y + CY;
    int bob = (fx::isin((int)(frame >> 2) * 20) * 3) >> 8;
    if (tapT) bob = tapT < 6 ? tapT : 12 - tapT;
    sprite4(HAND, x - 7, y - HAND[1] + bob - 2, humanTurn ? RM_ID : RM_CPU, 0);
}

// ---------------------------------------------------------------------------
// HUD: whose turn (a casino chip, spinning while the CPU thinks), the
// material edge, the captured pieces, and what the finger is over.
// ---------------------------------------------------------------------------
static void chip(int x, int y, bool black, int squash) {
    int rx = 6 * squash / 256;
    if (rx < 1) rx = 1;
    fillEllipse(x, y + 2, rx, 3, black ? NAVY : SILVER);
    fillEllipse(x, y, rx, 3, black ? INK : WHITE);
    ellipse(x, y, rx, 3, black ? GOLD : RED);
    if (rx > 3) fillEllipse(x, y, rx - 3, 1, black ? NAVY : SILVER);
}

static const char *const NAMES[7] = {"", "PAWN", "KNIGHT", "BISHOP", "ROOK", "QUEEN", "KING"};

// The captured pieces as little bars (taller = worth more), from what is
// missing off the board: the haul at a glance.
static int trays(int x) {
    static const uint8_t START[7] = {0, 8, 2, 2, 2, 1, 1};
    uint8_t have[2][7] = {};
    for (uint8_t sq = 0; sq < 64; sq++) if (shown[sq]) have[(shown[sq] & eng::BLACK) ? 1 : 0][shown[sq] & eng::TYPE]++;
    for (int side = 1; side >= 0; side--) {
        for (int t = eng::QUEEN; t >= eng::PAWN; t--) {
            for (int k = have[side][t]; k < START[t]; k++) {
                int h = t == eng::QUEEN ? 6 : t == eng::ROOK ? 5 : t == eng::PAWN ? 2 : 4;
                gfx_fillRect(x, 7 - h, 2, h, side ? BLUE : WHITE);
                x -= 3;
            }
        }
        x -= 3;
    }
    return x;
}

static void drawHud(uint32_t frame) {
    bool black = turnBlack;
    int sq = 256;
    if (thinking) { sq = fx::isin((int)frame * 6 + 64); if (sq < 0) sq = -sq; }
    gfx_fillRect(0, 0, 128, 9, INK);
    gfx_hline(0, 9, 128, GOLD);
    chip(7, 3, black, sq);
    char who[16];
    if (over) fmtStr(who, "GAME OVER");
    else if (match::setup.mode == match::TWO_PLAYER) fmtStr(who, black ? "BLACK" : "WHITE");
    else if (black == (match::setup.humanBlack != 0)) fmtStr(who, "YOUR MOVE");
    else {
        // The CPU by name, with dots while it thinks.
        char *p = fmtStr(who, opponentName);
        if (thinking) for (uint32_t k = 0; k < ((frame >> 4) & 3); k++) *p++ = '.', *p = 0;
    }
    text35(16, 2, who, thinking ? FX_B : WHITE);
    // Material edge (White's view), then each side's captures.
    int m = match::material();
    int x = 126;
    if (m) {
        char buf[6] = "+";
        fmtInt(buf + 1, m < 0 ? -m : m);
        x -= text35Width(buf);
        text35(x, 2, buf, m > 0 ? WHITE : SILVER);
        x -= 4;
    }
    trays(x - 1);
    // What the finger points at.
    if (humanTurn && shown[cur] && sel == 0xFF && !strat) {
        char buf[16];
        char *p = fmtStr(buf, NAMES[shown[cur] & eng::TYPE]);
        *p++ = ' '; *p++ = (char)('A' + (cur & 7)); *p++ = (char)('1' + (cur >> 3)); *p = 0;
        int w = text35Width(buf) + 7;
        fillRound(2, 117, w, 10, 2, NAVY);
        roundRect(2, 117, w, 10, 2, GOLD);
        text35(6, 119, buf, (shown[cur] & eng::BLACK) ? SILVER : WHITE);
    }
}

// ---------------------------------------------------------------------------
// Strategy view: the whole board from above.
// ---------------------------------------------------------------------------
static const int SX = 11, SY = 10, SS = 14;

static void squareXY(uint8_t sq, int &x, int &y) {
    int f = sq & 7, r = sq >> 3;
    if (cam.flip) { f = 7 - f; r = 7 - r; }
    x = SX + f * SS; y = SY + (7 - r) * SS;
}

static void icon(uint8_t p, int x, int y) {
    sprite4(ICON_ART[(p & eng::TYPE) - 1], x + 1, y + 1, (p & eng::BLACK) ? RM_ICON_B : RM_ID, 0);
}

static void render2D(uint32_t frame) {
    gfx_clear(NAVY);
    gfx_rect(SX - 2, SY - 2, 8 * SS + 4, 8 * SS + 4, GOLD);
    gfx_rect(SX - 1, SY - 1, 8 * SS + 2, 8 * SS + 2, WINE);
    for (uint8_t s = 0; s < 64; s++) {
        int x, y;
        squareXY(s, x, y);
        gfx_fillRect(x, y, SS, SS, ((s >> 3) + s) & 1 ? lightSq : darkSq);
    }
    for (int i = 0; i < 8; i++) {
        char c[2] = {(char)('A' + (cam.flip ? 7 - i : i)), 0};
        text35(SX + i * SS + 5, SY + 8 * SS + 2, c, GOLD);
        c[0] = (char)('8' - (cam.flip ? 7 - i : i));
        text35(SX - 8, SY + i * SS + 4, c, GOLD);
    }
    int x, y;
    if (match::lastFrom != 0xFF) {
        squareXY(match::lastFrom, x, y); gfx_rect(x + 1, y + 1, SS - 2, SS - 2, GOLD);
        squareXY(match::lastTo, x, y);   gfx_rect(x + 1, y + 1, SS - 2, SS - 2, GOLD);
    }
    if (match::checkSq != 0xFF) { squareXY(match::checkSq, x, y); gfx_fillRect(x, y, SS, SS, RED); }
    for (uint8_t s = 0; s < 64; s++) {
        uint8_t p = shown[s];
        if (!p) continue;
        squareXY(s, x, y);
        if (s == sel) y -= 1 + ((frame >> 3) & 1);
        icon(p, x + 2, y + 2);
    }
    for (int k = 0; k < 2; k++) {
        if (!mv[k].on) continue;
        int ax, ay, bx, by;
        squareXY(mv[k].from, ax, ay); squareXY(mv[k].to, bx, by);
        int e = mv[k].delay ? 0 : fx::ease(fx::IN_OUT, mv[k].t, mv[k].T);
        icon(mv[k].piece, ax + (((bx - ax) * e) >> 8) + 2, ay + (((by - ay) * e) >> 8) + 2);
    }
    if (sel != 0xFF && hints) {
        for (uint8_t i = 0; i < nTgt; i++) {
            squareXY(tgt[i], x, y);
            if (tgtCap[i]) gfx_rect(x, y, SS, SS, FX_B);
            else gfx_fillRect(x + 5, y + 5, 4, 4, FX_A);
        }
    }
    if (intent != 0xFF) { squareXY(intent, x, y); gfx_rect(x, y, SS, SS, RED); }
    if (humanTurn) { squareXY(cur, x, y); gfx_rect(x - 1, y - 1, SS + 2, SS + 2, FX_B); gfx_rect(x, y, SS, SS, INK); }
    if ((thinking || picking) && fingerSq != 0xFF) { squareXY(fingerSq, x, y); gfx_rect(x, y, SS, SS, RED); }
}

void renderScene(uint32_t frame) {
    drawTable();
    drawBoard();
    drawPieces(frame);
}

void drawPieceAt(uint8_t piece, int x, int y) { drawPiece(piece, x, y, 0, nullptr); }

// A still scene is not redrawn: the frame is flushed again, so palette
// effects keep moving at 60 Hz, and the bob and the marching borders step at
// 15 Hz, so an idle board costs a quarter of the frames.
static uint32_t lastSig;

static uint32_t signature(uint32_t frame, uint32_t ui) {
    int lo, hi;
    if (fx::activeRows(lo, hi) || mv[0].on || mv[1].on || fly.on || (topT && topT < 60)) return frame;
    if (cx16 != (int32_t)aimX << 4 || cy16 != (int32_t)aimY << 4) return frame;
    uint32_t h = 2166136261u;
    uint32_t v[] = {
        (uint32_t)cam.x, (uint32_t)cam.y, cam.flip, cur, sel, strat, wiggleT, intent, humanTurn, thinking,
        picking, fingerSq, (uint32_t)(fx16 >> 4), (uint32_t)(fy16 >> 4), frame >> 2, match::lastTo,
        match::checkSq, nTgt, over, (uint32_t)match::material(), tapT, ui,
    };
    for (uint32_t x : v) h = (h ^ x) * 16777619u;
    return h;
}

void invalidate() { lastSig = 0; }

bool render(uint32_t frame, uint32_t ui) {
    uint32_t sig = signature(frame, ui);
    if (sig == lastSig) return false;
    lastSig = sig;
#ifdef CHSIM
#define LAP(k) do { uint64_t n = sim_hostNanos(); simLap[k] += n - simT; simT = n; } while (0)
    uint64_t simT = sim_hostNanos();
#else
#define LAP(k) ((void)0)
#endif
    if (strat) {
        render2D(frame);
        LAP(0);
    } else {
        drawTable();     LAP(0);
        drawBoard();     LAP(1);
        drawOverlays(frame); LAP(2);
        drawPieces(frame);   LAP(3);
        drawFinger(frame);   LAP(4);
    }
    drawHud(frame);      LAP(5);
    fx::drawParticles();
    fx::drawFloats();
    fx::drawBanner();
    fx::applyShake(10, 127);
    LAP(6);
    return true;
}

#ifdef CHSIM
uint64_t simLap[8];
#endif

}  // namespace stage
