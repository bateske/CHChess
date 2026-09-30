#pragma GCC optimize("Os")   // cold code: size over speed (hot pixel loops live in Draw/Iso)
#include <string.h>
#include <CHGfx.h>
#include <Arduino.h>
#include "../../config.h"
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

namespace stage {

using namespace iso;

// ---------------------------------------------------------------------------
// Colour remaps for the art (its neutral tones -> a side, and effects)
// ---------------------------------------------------------------------------
static uint8_t RM_W[16], RM_B[16], RM_HIT[16], RM_CPU[16], RM_ID[16];

static void initRemaps() {
    for (uint8_t i = 0; i < 16; i++) RM_W[i] = RM_B[i] = RM_CPU[i] = RM_ID[i] = i, RM_HIT[i] = WHITE;
    // Body tones dark to light are BLUE, NAVY, SILVER, WHITE, then CYAN's
    // glint (tools/pieces.py). Black keeps its darkest tone off INK, so the
    // outline still draws the silhouette.
    RM_W[NAVY] = SILVER; RM_W[SILVER] = WHITE; RM_W[CYAN] = WHITE;
    RM_B[BLUE] = NAVY; RM_B[SILVER] = NAVY; RM_B[WHITE] = BLUE; RM_B[CYAN] = SILVER;
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
static uint8_t intent = 0xFF;        // the CPU's chosen destination, shown while it moves
static uint8_t viewMode;
static bool hints = true, fast;
static bool humanTurn, thinking;

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

// Camera: eased (world, Q4) towards its aim, plus the B+direction spring
// (screen pixels, Q4) on top.
static int32_t cx16, cy16;
static int16_t aimX, aimY;
static uint8_t aimShift = 2;
static int16_t sprX, sprY, sprVX, sprVY;
static int8_t sprDX, sprDY;

// The whip zoom: iso::tileH steps towards zoomTo (in, a step a frame; back
// out, one every other), and zoomHold is the look at a landed move before
// the camera pulls back.
static uint8_t zoomTo = 5, zoomHold, zoomTick;

// The last move in words where the hover plate goes, popping up word by
// word: "KNIGHT TO F3", "ROOK TAKES QUEEN ON A4".
static const char *annW[5];
static uint8_t annC[5], annN, annT;
static char annSq[3];

// ---------------------------------------------------------------------------
// Geometry and the camera
// ---------------------------------------------------------------------------
static int depthOf(uint8_t sq) {
    int u, v;
    toView(sq, cam.flip, u, v);
    return flat ? v : u + v;
}

// Frame a world point: halfway between it and the board's centre (less so
// zoomed in: right on it at the closest), then as little further as keeps it
// well on screen, then never past the board's edges (no empty carpet for
// nothing). The view is 128 x 118 below the HUD.
static void aimAt(int fx, int fy, uint8_t shift) {
    int cx = 0, cy = 8 * hh(), lean = 10 - tileH;
    int x = fx + (cx - fx) * lean / 10, y = fy + (cy - fy) * lean / 10;
    int mx = 44, my = 30;
    if (x < fx - mx) x = fx - mx;
    if (x > fx + mx) x = fx + mx;
    if (y < fy - my) y = fy - my;
    if (y > fy + my) y = fy + my;
    // Content bounds: the board and its slab, and the tallest piece on the
    // far squares.
    int x0 = -8 * hw() - 6, x1 = 8 * hw() + 6;
    int y0 = -zoomed(24), y1 = 16 * hh() + slab() + 10;
    if (x1 - x0 <= 128) x = (x0 + x1) / 2;
    else { if (x < x0 + 64) x = x0 + 64; if (x > x1 - 64) x = x1 - 64; }
    if (y1 - y0 <= 118) y = (y0 + y1) / 2;
    else { if (y < y0 + 59) y = y0 + 59; if (y > y1 - 59) y = y1 - 59; }
    aimX = (int16_t)x; aimY = (int16_t)y; aimShift = shift;
}

static void aimSq(uint8_t sq, uint8_t shift) {
    int x, y;
    worldOf(sq, x, y);
    aimAt(x, y - zoomed(6), shift);
}

static void snapCamera() {
    cx16 = aimX << 4; cy16 = aimY << 4;
}

static void stepCamera() {
    int32_t dx = (aimX << 4) - cx16, dy = (aimY << 4) - cy16;
    cx16 += dx >> aimShift; cy16 += dy >> aimShift;
    if (dx > -16 && dx < 16) cx16 = aimX << 4;
    if (dy > -16 && dy < 16) cy16 = aimY << 4;
    // The spring: pulled towards 40 px in the held direction, overshooting a
    // little either way.
    sprVX += (int16_t)(((sprDX * 40 * 16 - sprX) >> 3) - (sprVX >> 2));
    sprVY += (int16_t)(((sprDY * 32 * 16 - sprY) >> 3) - (sprVY >> 2));
    sprX += sprVX; sprY += sprVY;
    if (!sprDX && sprX > -16 && sprX < 16 && sprVX > -8 && sprVX < 8) sprX = sprVX = 0;
    if (!sprDY && sprY > -16 && sprY < 16 && sprVY > -8 && sprVY < 8) sprY = sprVY = 0;
    cam.x = (int)(cx16 >> 4) + (sprX >> 4);
    cam.y = (int)(cy16 >> 4) + (sprY >> 4);
}

// Height of the piece on a square (finger tip rests just above it).
static int topOf(uint8_t sq) {
    uint8_t p = shown[sq];
    return zoomed(p ? art(p).ay + 1 : 2);
}

static uint8_t pieceValue(uint8_t p) {
    static const uint8_t V[7] = {0, 1, 3, 3, 5, 9, 0};
    return V[p & eng::TYPE];
}

static void fingerTo(uint8_t sq) {
    int x, y;
    worldOf(sq, x, y);
    fx16 = (int32_t)x << 4;
    fy16 = (int32_t)(y - topOf(sq)) << 4;
}

// ---------------------------------------------------------------------------
// Public controls
// ---------------------------------------------------------------------------
uint8_t cursor() { return cur; }
bool flipped() { return cam.flip; }
uint8_t view() { return viewMode; }

// Everything held in world space keeps its place on the board.
void setZoom(uint8_t h) {
    uint8_t o = tileH;
    if (h == o) return;
    tileH = h;
    cx16 = cx16 * h / o; cy16 = cy16 * h / o;
    aimX = (int16_t)(aimX * h / o); aimY = (int16_t)(aimY * h / o);
    fx16 = fx16 * h / o; fy16 = fy16 * h / o;
    int16_t *f[5] = {&fly.x, &fly.y, &fly.z, &fly.vx, &fly.vz};
    for (int16_t *v : f) *v = (int16_t)(*v * h / o);
}
void setHints(bool on) { hints = on; }
void setCoords(bool on) { iso::coords = on; }
void setFast(bool on) { fast = on; }
uint8_t selected() { return sel; }

void setView(uint8_t v) {
    viewMode = v;
    iso::setView(v == MAP);
    setZoom(zoomTo = 5);
    zoomHold = 0;
    fly.on = 0;                      // it lives in the old view's world
    sprX = sprY = sprVX = sprVY = 0;
    aimSq(cur, 2);
    snapCamera();
    cam.x = aimX; cam.y = aimY;
    if (fingerSq != 0xFF) fingerTo(fingerSq);
    dipT = 6;
}

void spring(int dx, int dy) {
    sprDX = (int8_t)(flat ? 0 : dx);
    sprDY = (int8_t)(flat ? 0 : dy);
}

void setCursor(uint8_t sq) {
    cur = sq;
    aimSq(sq, 2);
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
    fx::burst(fx::STAR, x, y - zoomed(10), 6, 20, GOLD);
}

void deselect() {
    sel = 0xFF;
    nTgt = 0;
    pal::setMode(pal::CASINO);
}

bool busy() {
    return mv[0].on || mv[1].on || fly.on || holdT || picking || topT || handT || zoomHold ||
           (tileH != zoomTo && !flat);
}
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
    setZoom(zoomTo = 5);
    zoomHold = annT = 0;
    handT = 0;
    turnBlack = match::blackToMove();
    bool black = match::setup.mode == match::VS_CPU ? match::setup.humanBlack : match::blackToMove();
    cam.flip = black;
    curW = 12; curB = 52;                     // e2 / e7
    cur = black ? curB : curW;
    aimSq(cur, 2);
    snapCamera();
    fingerTo(cur);
}

static void onTurn(bool black, bool human) {
    humanTurn = human;
    turnBlack = black;
    intent = 0xFF;
    if (!human) return;
    if (match::setup.mode == match::TWO_PLAYER) {
        // Hand the board to the other player.
        if (cam.flip) curB = cur; else curW = cur;
        cur = black ? curB : curW;
        if (cam.flip != black) {
            handT = 1;
            handBlack = black;
            humanTurn = false;               // no finger until the camera arrives
            aimAt(0, 8 * hh(), 1);           // the board's centre, fast
            audio::sfx(Sfx::Whoosh);
        }
    } else {
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
    m.T = (uint8_t)(fast ? 10 + d * 2 : 14 + d * 3);
    m.arc = arc; m.delay = delay; m.on = 1;
}

static match::Event pending;          // the move being shown (captures resolve on landing)

static void onMove(const match::Event &e) {
    pending = e;
    if (humanTurn) cur = e.b;            // your cursor goes with the piece
    humanTurn = false;
    thinking = false;
    deselect();
    if (!flat && !fast) zoomTo = 10;     // whip in on it
    shown[e.a] = 0;
    bool knight = (e.piece & eng::TYPE) == eng::KNIGHT;
    launch(mv[0], e.piece, e.a, e.b, knight ? 12 : 3, 0);
    if (knight) audio::sfx(Sfx::Hop);
    if (e.rookFrom != 0xFF) {
        uint8_t rook = shown[e.rookFrom];
        shown[e.rookFrom] = 0;
        launch(mv[1], rook, e.rookFrom, e.rookTo, 10, 10);
        audio::sfx(Sfx::Castle);
    }
}

static const char *const NAMES[7] = {"", "PAWN", "KNIGHT", "BISHOP", "ROOK", "QUEEN", "KING"};

static void addWord(const char *w, uint8_t c) { annW[annN] = w; annC[annN++] = c; }

static void announce(const match::Event &e) {
    annN = 0;
    annT = 1;
    annSq[0] = (char)('A' + (e.b & 7)); annSq[1] = (char)('1' + (e.b >> 3)); annSq[2] = 0;
    if (e.rookFrom != 0xFF) {
        addWord("CASTLES", WHITE);
        addWord((e.b & 7) > 4 ? " KINGSIDE" : " QUEENSIDE", GOLD);
        return;
    }
    addWord(NAMES[e.piece & eng::TYPE], WHITE);
    if (e.captured) {
        addWord(" TAKES ", RED);
        addWord(NAMES[e.captured & eng::TYPE], WHITE);
        addWord(e.capSq != e.b ? " EN PASSANT" : " ON ", SILVER);
        if (e.capSq != e.b) return;
    } else {
        addWord(" TO ", SILVER);
    }
    addWord(annSq, GOLD);
    if (e.promo) { addWord(" = ", SILVER); addWord(NAMES[e.promo], FX_B); }
}

static const uint8_t ANN_FRAMES = 120;

static void land(Mover &m, bool main) {
    m.on = 0;
    int x, y;
    screenOf(m.to, x, y);
    if (!main) { shown[m.to] = m.piece; fx::burst(fx::DUST, x, y, 6, 16, SILVER); return; }
    const match::Event &e = pending;
    uint8_t piece = e.promo ? (uint8_t)(e.promo | (e.piece & eng::BLACK)) : e.piece;
    int up = zoomed(14);
    if (e.captured) {
        // Knock the victim off: it flies on in the attacker's direction.
        shown[e.capSq] = 0;
        int ax, ay, bx, by;
        worldOf(e.a, ax, ay); worldOf(e.capSq, bx, by);
        fly.piece = e.captured;
        fly.x = (int16_t)(bx << 4); fly.y = (int16_t)(by << 4); fly.z = 0;
        int s = zoomed(16);
        fly.vx = (int16_t)(bx > ax ? s : bx < ax ? -s : (fx::rnd() & 1 ? s : -s));
        fly.vz = (int16_t)zoomed(44);
        fly.spin = (int8_t)(fly.vx > 0 ? 11 : -11);
        fly.ang = 0; fly.t = 0; fly.on = 1;
        fx::burst(fx::SPARK, x, y - up / 2, 12, 36, GOLD);
        fx::burst(fx::STAR, x, y - up / 2, 4, 24, WHITE);
        fx::shake(10, 2);
        audio::sfx(Sfx::Capture);
        char buf[6] = "+";
        fmtInt(buf + 1, pieceValue(e.captured));
        fx::floatText(buf, x, y - up - 6, GOLD);
    } else {
        fx::shake(4, 1);
        audio::sfx(Sfx::Land);
    }
    fx::burst(fx::DUST, x, y, 7, 18, SILVER);
    shown[m.to] = piece;
    if (e.promo) {
        fx::burst(fx::STAR, x, y - up, 12, 36, GOLD);
        audio::sfx(Sfx::Promote);
        holdT = 30;
    }
    intent = 0xFF;
    holdT = (uint8_t)(holdT > 12 ? holdT : 12);
    announce(e);
    // Take in the landing, then pull back - unless it was mate: stay on it.
    match::Event nx;
    bool mate = match::peekEvent(nx) && nx.type == match::EV_OVER && nx.b == match::BY_MATE;
    if (zoomTo > 5 && !mate) zoomHold = (uint8_t)(e.captured ? 34 : 20);
}

static void onCheck() {
    fx::banner("CHECK!", fx::B_RED, 36, 70);
    audio::sfx(Sfx::Check);
    audio::led(audio::LED_TRIPLE);
    holdT = 40;
}

static void onOver(uint8_t result, uint8_t reason) {
    over = true;
    overT = 0;
    humanTurn = thinking = false;
    bool humanWon = match::setup.mode == match::TWO_PLAYER ||
                    (result == match::WHITE_WINS) == (match::setup.humanBlack == 0);
    bool decisive = result == match::WHITE_WINS || result == match::BLACK_WINS;
    if (decisive && reason == match::BY_MATE) {
        topSq = match::checkSq;
        topT = 1;
        if (!flat) zoomTo = 10;
        topDir = (int8_t)(fx::rnd() & 1 ? 1 : -1);
        fx::banner("CHECKMATE!", fx::B_RAINBOW, 34, 170);
    } else if (decisive) {
        fx::banner(result == match::WHITE_WINS ? "BLACK RESIGNS" : "WHITE RESIGNS", fx::B_WHITE, 36, 150);
    } else {
        fx::banner(result == match::STALEMATE ? "STALEMATE" : "DRAW", fx::B_CYAN, 36, 150);
    }
    if (decisive && humanWon) {
        audio::sfx(reason == match::BY_MATE ? Sfx::Mate : Sfx::Win);
        audio::led(audio::LED_PARTY);
        fx::fountain(40, 90, 20);
        fx::fountain(88, 90, 20);
    } else {
        audio::sfx(decisive ? Sfx::Lose : Sfx::Draw);
    }
}

void begin() {
    initRemaps();
}

// ---------------------------------------------------------------------------
// Per tick
// ---------------------------------------------------------------------------
static void moverPos(const Mover &m, int &x, int &y, int &z) {
    int ax, ay, bx, by;
    worldOf(m.from, ax, ay);
    worldOf(m.to, bx, by);
    int e = m.delay ? 0 : fx::ease(fx::IN_OUT, m.t, m.T);
    x = ax + (((bx - ax) * e) >> 8);
    y = ay + (((by - ay) * e) >> 8);
    // Lift, glide, drop: a sine arc for hops, a flat top for sliding pieces.
    int arc = zoomed(m.arc);
    if (m.arc > 5) z = (arc * fx::isin((m.t * 128) / m.T)) >> 8;
    else z = m.t < 4 ? m.t * arc / 4 : (m.T - m.t < 4 ? (m.T - m.t) * arc / 4 : arc);
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
            case match::EV_CHECK: onCheck(); break;
            case match::EV_OVER:  onOver(e.a, e.b); break;
        }
    }

    if (holdT) holdT--;
    if (zoomHold && !--zoomHold) zoomTo = 5;
    if (tileH != zoomTo && !flat && (tileH < zoomTo || (++zoomTick & 1)))
        setZoom((uint8_t)(tileH < zoomTo ? tileH + 1 : tileH - 1));
    if (annT && ++annT > ANN_FRAMES) annT = 0;
    for (int k = 0; k < 2; k++) {
        Mover &m = mv[k];
        if (!m.on) continue;
        if (m.delay) m.delay--;
        else if (++m.t >= m.T) land(m, k == 0);
    }

    if (fly.on) {
        fly.t++;
        fly.x += fly.vx; fly.y += fly.vx / 3;
        fly.z += fly.vz; fly.vz -= (int16_t)zoomed(4);
        fly.ang += fly.spin;
        int sx = toScreenX(fly.x >> 4), sy = toScreenY(fly.y >> 4) - (fly.z >> 4);
        if (fly.t > 90 || sx < -30 || sx > 158 || sy > 190) fly.on = 0;
    }

    if (topT && topT < 60) topT++;
    if (topT >= 60) topT = 0;                   // down: drawn lying from topSq while over

    // What the camera frames: the move in flight, the CPU's finger, the
    // toppling king, or your cursor.
    if (mv[0].on && !mv[0].delay) {
        int x, y, z;
        moverPos(mv[0], x, y, z);
        aimAt(x, y - zoomed(6), 2);
    } else if (topSq != 0xFF && over) {
        int x, y;
        worldOf(topSq, x, y);
        aimAt(x, y + zoomed(8), 3);          // the fallen king above the result panel
    } else if (think || picking) {
        if (think) {
            thinkT = (uint8_t)(thinkT < 255 ? thinkT + 1 : 255);
            // Follow the root move being searched, without flitting about.
            eng::Move r = eng::rootMove();
            if (++dwell > 20 && r != eng::NO_MOVE) {
                uint8_t s = eng::from(r);
                if (s != fingerSq) { fingerSq = s; dwell = 0; }
            }
        }
        aimSq(fingerSq, 3);
    } else if (humanTurn) {
        aimSq(cur, 2);
    }
    if (picking) {
        pickT++;
        uint8_t need = (uint8_t)(fast ? 8 : (thinkT < 36 ? 56 - thinkT : 20));
        if (pickT >= need && !tapT) { tapT = 1; audio::sfx(Sfx::Select); }
        if (tapT && ++tapT > 12) { picking = false; tapT = 0; }
    }
    if (handT) {
        handT++;
        if (handT < 12) aimAt(0, 8 * hh(), 1);
        if (handT == 12) {
            // Over the centre (the same world point from either side, and the
            // squares keep their colours): turn round behind the dip.
            cam.flip = handBlack;
            dipT = 8;
            fx::banner(handBlack ? "BLACK TO MOVE" : "WHITE TO MOVE", fx::B_GOLD, 40, 60);
        }
        if (handT > 12) aimSq(cur, 3);
        if (handT > 40) { handT = 0; humanTurn = true; fingerTo(cur); }
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
        worldOf(fingerSq, x, y);
        y -= topOf(fingerSq);
        fx16 += ((x << 4) - fx16) >> 1;
        fy16 += ((y << 4) - fy16) >> 1;
    }

    if (over && overT < 255 && ++overT > 150) overDone = true;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
static bool isTarget(uint8_t sq, bool &cap) {
    for (uint8_t i = 0; i < nTgt; i++) if (tgt[i] == sq) { cap = tgtCap[i]; return true; }
    return false;
}

static void drawOverlays(uint32_t frame) {
    uint8_t ph = (uint8_t)(frame >> 3);
    uint8_t in = (uint8_t)((tileH + 2) / 5);        // borders sit this far in
    // The other side's last move, while it is your turn to answer it.
    if (match::lastFrom != 0xFF && humanTurn) {
        tileTint(match::lastFrom, in, GOLD);
        tileBorder(match::lastTo, in, GOLD, GOLD, 0);
    }
    if (match::checkSq != 0xFF && !mv[0].on) {
        tileTint(match::checkSq, 0, RED);
        tileBorder(match::checkSq, 0, RED, WHITE, ph);
    }
    if (intent != 0xFF) {
        tileTint(intent, in, shown[intent] ? RED : CYAN);
        tileBorder(intent, 0, shown[intent] ? RED : CYAN, WHITE, ph);
    }
    if (sel != 0xFF && hints) {
        for (uint8_t i = 0; i < nTgt; i++) {
            if (tgtCap[i]) { tileTint(tgt[i], in, RED); tileBorder(tgt[i], 0, FX_B, RED, ph); }
            else           { tileTint(tgt[i], in, CYAN); tileBorder(tgt[i], 0, FX_A, CYAN, ph); }
        }
    }
    if (humanTurn) {
        if (sel != 0xFF) tileBorder(sel, 0, WHITE, WHITE, 0);
        tileBorder(cur, 0, sel != 0xFF ? WHITE : FX_B, GOLD, ph);
    }
}

// Pieces standing in front of the cursor (or of the mated king), over its
// square, are ghosted.
static bool hidesCursor(uint8_t sq, int x, int y, const PieceArt &a) {
    uint8_t f = over ? topSq : (humanTurn ? cur : 0xFF);
    if (f == 0xFF || flat) return false;
    int d = depthOf(sq) - depthOf(f);
    if (d < 1 || d > 4) return false;
    int cxs, cys;
    screenOf(f, cxs, cys);
    int left = x - zoomed(a.ax), top = y - zoomed(a.ay), w = zoomed(a.data[0]);
    return cxs + zoomed(3) > left && cxs - zoomed(3) < left + w && cys - zoomed(2) > top;
}

static void drawPiece(uint8_t p, int x, int y, uint8_t flags, const uint8_t *remap, int scale) {
    const PieceArt &a = art(p);
    bool mirror = (p & eng::TYPE) == eng::KNIGHT && ((p & eng::BLACK) != 0) != cam.flip;
    int w = a.data[0];
    int left = x - (((mirror ? w - 1 - a.ax : a.ax) * scale) >> 8);
    sprite4(a.data, left, y - ((a.ay * scale) >> 8), remap ? remap : remapFor(p),
            (uint8_t)(flags | (mirror ? SPR_MIRROR : 0)), scale);
}

static void drawMover(const Mover &m) {
    int x, y, z;
    moverPos(m, x, y, z);
    int sx = toScreenX(x), sy = toScreenY(y);
    fillEllipse(sx, sy, zoomed(3), (tileH + 2) / 5, INK);    // its shadow stays on the board
    drawPiece(m.piece, sx, sy - z, 0, nullptr, zscale());
}

static void drawPieces(uint32_t frame) {
    // Movers go in at their current depth, between the rows.
    int md[2] = {99, 99};
    for (int k = 0; k < 2; k++) {
        if (!mv[k].on) continue;
        int da = depthOf(mv[k].from), db = depthOf(mv[k].to);
        int e = mv[k].delay ? 0 : fx::ease(fx::IN_OUT, mv[k].t, mv[k].T);
        md[k] = da + (((db - da) * e + (db > da ? 255 : 0)) >> 8);
    }
    int last = flat ? 7 : 14;
    for (int d = 0; d <= last; d++) {
        for (int u = 0; u < 8; u++) {
            int v = flat ? d : d - u;
            if (v < 0 || v > 7) continue;
            uint8_t sq = fromView(u, v, cam.flip);
            uint8_t p = shown[sq];
            if (!p) continue;
            int x, y;
            screenOf(sq, x, y);
            if (x < -20 || x > 148 || y < -8 || y > 190) continue;
            const PieceArt &a = art(p);
            if (sq == topSq && over) {
                // Checkmated: the king topples, bouncing as it lands, and stays down.
                int e = topT ? fx::ease(fx::OUT_BOUNCE, topT, 30) : 256;
                spriteRot(a.data, a.ax, a.ay, x, y, (uint8_t)((topDir * 60 * e) >> 8), zscale(), remapFor(p), false);
                continue;
            }
            uint8_t flags = hidesCursor(sq, x, y, a) ? SPR_GHOST : 0;
            if (sq == sel) {
                fillEllipse(x, y, zoomed(3), (tileH + 2) / 5, INK);
                y -= zoomed(3) + (zoomed(fx::isin((int)(frame >> 3) * 48)) >> 8);
            }
            bool cap = false;
            bool blink = sel != 0xFF && (frame & 8) && isTarget(sq, cap) && cap;
            drawPiece(p, x, y, flags, blink ? RM_HIT : nullptr, zscale());
        }
        for (int k = 0; k < 2; k++) if (mv[k].on && md[k] == d) drawMover(mv[k]);
    }
    if (fly.on) {
        int sx = toScreenX(fly.x >> 4), sy = toScreenY(fly.y >> 4) - (fly.z >> 4);
        const PieceArt &a = art(fly.piece);
        spriteRot(a.data, a.data[0] / 2, a.data[1] / 2, sx, sy - zoomed(a.ay) / 2, fly.ang, zscale(),
                  fly.t < 4 ? RM_HIT : remapFor(fly.piece), false);
    }
}

static void drawFinger(uint32_t frame) {
    if (fingerSq == 0xFF || (!humanTurn && !thinking && !picking)) return;
    int x = toScreenX((int)(fx16 >> 4)), y = toScreenY((int)(fy16 >> 4));
    int bob = (fx::isin((int)(frame >> 3) * 40) * 2) >> 8;
    if (tapT) bob = (tapT < 6 ? tapT : 12 - tapT) / 2;
    bob = zoomed(bob);
    int w = HAND[0], h = HAND[1];
    sprite4(HAND, x - zoomed(w / 2), y - zoomed(h) + bob - 1, humanTurn ? RM_ID : RM_CPU, 0, zscale());
}

// ---------------------------------------------------------------------------
// HUD: whose turn (a casino chip, spinning while the CPU thinks), the
// material edge, the captured pieces; and a plate at the foot of the screen
// naming what the finger is on, or the last move.
// ---------------------------------------------------------------------------

// The captured pieces as little bars (taller = worth more), from what is
// missing off the board: the haul at a glance.
static void trays(int x) {
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
}

// Words in their colours on a rounded plate centred at y: grow (Q8) is the
// plate's width so far, and word k shows from frame 6 + 3k, dropping in.
static void plate(const char *const *w, const uint8_t *c, uint8_t n, int y, int grow, int t) {
    int tw = 0;
    for (uint8_t i = 0; i < n; i++) tw += text35Width(w[i]);
    int pw = ((tw + 8) * grow) >> 8;
    if (pw < 6) return;
    fillRound(64 - pw / 2, y, pw, 11, 2, NAVY);
    roundRect(64 - pw / 2, y, pw, 11, 2, GOLD);
    int x = 64 - tw / 2;
    for (uint8_t i = 0; i < n; i++) {
        int d = t - 6 - 3 * i;
        if (d >= 0) text35(x, y + 3 - (d < 3 ? 3 - d : 0), w[i], c[i]);
        x += text35Width(w[i]);
    }
}

static void drawHud(uint32_t frame) {
    bool black = turnBlack;
    gfx_fillRect(0, 0, 128, 9, INK);
    gfx_hline(0, 9, 128, GOLD);
    // The turn chip, spinning while the CPU thinks.
    int rx = 5;
    if (thinking) { rx = (fx::isin((int)frame * 6 + 64) * 5) >> 8; if (rx < 0) rx = -rx; if (!rx) rx = 1; }
    fillEllipse(7, 5, rx, 2, black ? NAVY : SILVER);         // edge
    fillEllipse(7, 4, rx, 2, black ? GOLD : RED);            // rim
    fillEllipse(7, 4, rx - 1, 1, black ? INK : WHITE);       // face
    char who[16];
    if (over) fmtStr(who, "GAME OVER");
    else if (match::setup.mode == match::TWO_PLAYER) fmtStr(who, black ? "BLACK" : "WHITE");
    else if (black == (match::setup.humanBlack != 0)) fmtStr(who, "YOUR MOVE");
    else {
        // The CPU by name, with dots while it thinks.
        char *p = fmtStr(who, opponentName);
        if (thinking) for (uint32_t k = 0; k < ((frame >> 4) & 3); k++) *p++ = '.', *p = 0;
    }
    text35(15, 2, who, thinking ? FX_B : WHITE);
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
    int py = 116;
    if (annT) {
        // The last move: the plate springs open, then the words drop in.
        int t = annT, grow = t < 8 ? fx::ease(fx::OUT_BACK, t, 8) : t > ANN_FRAMES - 8 ? (ANN_FRAMES - t) * 32 : 256;
        plate(annW, annC, annN, py, grow, t);
    } else if (humanTurn && (shown[cur] || sel != 0xFF)) {
        // What the finger is on: a piece, or where the picked-up one would go.
        char sq[3] = {(char)('A' + (cur & 7)), (char)('1' + (cur >> 3)), 0};
        const char *w[3] = {NAMES[shown[sel != 0xFF ? sel : cur] & eng::TYPE], " ", sq};
        uint8_t c[3] = {WHITE, WHITE, GOLD};
        if (sel != 0xFF && shown[cur]) { w[1] = " TAKES "; c[1] = RED; w[2] = NAMES[shown[cur] & eng::TYPE]; c[2] = WHITE; }
        else if (sel != 0xFF) { w[1] = " TO "; c[1] = SILVER; }
        plate(w, c, 3, py, 256, 99);
    }
}

void renderScene(uint32_t frame) {
    drawTable();
    drawBoard();
    drawPieces(frame);
}

// Drawn doubled (the promotion reel), whatever the view.
void drawPieceAt(uint8_t piece, int x, int y) { drawPiece(piece, x, y, 0, nullptr, 512); }

// A still scene is not redrawn: the frame is flushed again, so palette
// effects keep moving at 60 Hz, and the bob and the marching borders step at
// 7.5 Hz, so an idle board costs an eighth of the frames.
static uint32_t lastSig;

static uint32_t signature(uint32_t frame, uint32_t ui) {
    int lo, hi;
    if (fx::activeRows(lo, hi) || mv[0].on || mv[1].on || fly.on || (topT && topT < 60)) return frame;
    if (cx16 != (int32_t)aimX << 4 || cy16 != (int32_t)aimY << 4 || sprX || sprY) return frame;
    uint32_t h = 2166136261u;
    uint32_t v[] = {
        (uint32_t)cam.x, (uint32_t)cam.y, cam.flip, cur, sel, viewMode, tileH, intent, humanTurn, thinking,
        picking, fingerSq, (uint32_t)(fx16 >> 4), (uint32_t)(fy16 >> 4), frame >> 3, match::lastTo,
        match::checkSq, nTgt, over, (uint32_t)match::material(), tapT, annT, ui,
    };
    for (uint32_t x : v) h = (h ^ x) * 16777619u;
    return h;
}

void invalidate() { lastSig = 0; }

bool render(uint32_t frame, uint32_t ui) {
    uint32_t sig = signature(frame, ui);
    if (sig == lastSig) return false;
    lastSig = sig;
    drawTable();
    drawBoard();
    drawOverlays(frame);
    drawPieces(frame);
    drawFinger(frame);
    drawHud(frame);
    fx::drawParticles();
    fx::drawFloats();
    fx::drawBanner();
    fx::applyShake(10, 127);
    return true;
}

#if CHCH_DEBUG
// Device render profile (debug Y command): microseconds per section,
// averaged over 8 draws of the current scene.
static void profTable(uint32_t) { drawTable(); }
static void profBoard(uint32_t) { drawBoard(); }
static void profFx(uint32_t) { fx::drawParticles(); fx::drawFloats(); fx::drawBanner(); }
void profile(uint32_t *us) {
    static void (*const PART[6])(uint32_t) = {profTable, profBoard, drawOverlays, drawPieces, drawHud, profFx};
    for (int k = 0; k < 6; k++) {
        uint32_t t = micros();
        for (int i = 0; i < 8; i++) PART[k](0);
        us[k] = (micros() - t) / 8;
    }
    invalidate();
}
#endif

}  // namespace stage
