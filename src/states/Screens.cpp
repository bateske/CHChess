#pragma GCC optimize("Os")   // cold code: size over speed (hot pixel loops live in Draw/Mask/Iso)
#include <Arduino.h>
#include <string.h>
#include <CHGfx.h>
#include "../../config.h"
#include "Screens.h"
#include "../CHGame.h"
#include "../gfx/Palette.h"
#include "../gfx/Draw.h"
#include "../gfx/Mask.h"
#include "../gfx/Fmt.h"
#include "../fx/Fx.h"
#include "../audio/Audio.h"
#include "../iso/Iso.h"
#include "../engine/Engine.h"
#include "../game/Match.h"
#include "../stage/Stage.h"
#include "../save/Save.h"
#include "../debug/Debug.h"
#ifdef CHSIM
#include <sim.h>
#endif

namespace screens {

enum class Scr : uint8_t { Title, Setup, Play, Options };
static Scr cur = Scr::Title, pending = Scr::Title;
static uint16_t t;                   // frames on this screen
static uint8_t fadeOut, fadeIn;
static uint8_t sel;                  // menu cursor
#if !CHCH_LEAN
static Scr optBack = Scr::Title;
#endif

static Options opt;
static Stats stats;
static bool hasGame;                 // a saved game is waiting

// Play-screen overlays.
enum Overlay : uint8_t { NONE, PAUSE, PROMO, RESULT };
static Overlay overlay;
static uint8_t promoFrom, promoTo, promoSel;
static uint8_t pendingAction;        // chosen from the pause menu while the CPU was thinking
static bool statsCounted;
#if CHCH_DEBUG
static uint32_t thinkMs;            // the CPU's last search, wall time (debug W)
#endif

static const char *const OPPONENT[match::LEVELS] = {"BEGINNER", "CLUB PLAYER", "EXPERT", "MASTER", "GRANDMASTER"};
static const char *const OPP_LINE[match::LEVELS] = {
    "STILL LEARNING THE MOVES", "SOLID, BUT SLIPS UP", "PUNISHES MISTAKES", "PLAYS TO WIN, NOT DRAW",
    "ITS BEST MOVE, EVERY TIME"};

// ---------------------------------------------------------------------------
// Flow
// ---------------------------------------------------------------------------
static void go(Scr s) {
    if (fadeOut) return;
    pending = s;
    fadeOut = 8;
}

static void enter(Scr s) {
    cur = s;
    stage::invalidate();
    t = 0;
    sel = 0;
    fadeIn = 8;
    fx::clear();
    pal::setMode(pal::CASINO);
    if (s == Scr::Title) {
        // The board behind the title: a fresh game, close up, the camera
        // drifting over it.
        match::Setup demo = {match::TWO_PLAYER, 0, 0, 1};
        match::start(demo);
        stage::update();
        stage::setView(stage::NORMAL);
        stage::setZoom(10);
        audio::sfx(Sfx::Title);
    }
    if (s == Scr::Setup) sel = 2;
    if (s == Scr::Play) {
        stage::opponentName = OPPONENT[match::setup.level];
        stage::setView(stage::NORMAL);
    }
}

static void persist(bool withGame) {
    gfx_wait();                      // save builds its page in the chunk scratch
    save::store(opt, stats, withGame);
    hasGame = withGame;
}

static void applyOptions() {
    audio::setOn(opt.sound != 0);
    pal::setTheme(opt.felt);
    stage::setCoords(opt.coords != 0);
    stage::setFast(opt.speed != 0);
}

// ---------------------------------------------------------------------------
// Shared drawing (CHBlackjack's look)
// ---------------------------------------------------------------------------
static void feltBackdrop() {
    gfx_clear(FELT);
    dither(0, 0, 128, 6, FELT_DK, 0);
    dither(0, 122, 128, 6, FELT_DK, 1);
    dither(0, 0, 6, 128, FELT_DK, 0);
    dither(122, 0, 6, 128, FELT_DK, 1);
    gfx_rect(2, 2, 124, 124, GOLD);
}

// Big lettering in PPOT's font with a gradient, outline and shadow.
static void title35(const char *text, int y, uint8_t scale, uint8_t top, uint8_t mid, uint8_t low,
                    uint8_t shadow, uint8_t lowFrom) {
    int h = 6 * scale;
    Mask m = maskBegin(124, h);
    maskText35(m, 0, 0, text, scale);
    uint8_t ramp[32];
    for (int i = 0; i < h + 2 && i < 32; i++) ramp[i] = i < scale ? top : (i < lowFrom ? mid : low);
    maskDraw(m, 64 - text35WidthScaled(text, scale) / 2, y, mid, INK, shadow, ramp);
}

static void centred35(int y, const char *s, uint8_t c) { text35(64 - text35Width(s) / 2, y, s, c); }
static void centred2(int y, const char *s, uint8_t c) { text35x2(64 - text35x2Width(s) / 2, y, s, c); }

static void menuItem(int y, const char *s, bool on, uint32_t frame) {
    int w = text35x2Width(s);
    if (on) {
        fillRound(64 - w / 2 - 6, y - 3, w + 12, 15, 3, NAVY);
        roundRect(64 - w / 2 - 6, y - 3, w + 12, 15, 3, (frame & 16) ? FX_B : GOLD);
    }
    centred2(y, s, on ? GOLD : WHITE);
}

static bool menuNav(uint8_t n) {
    if (arduboy.repeat(UP_BUTTON) && sel > 0) { sel--; audio::sfx(Sfx::Cursor); }
    if (arduboy.repeat(DOWN_BUTTON) && sel + 1 < n) { sel++; audio::sfx(Sfx::Cursor); }
    return arduboy.justPressed(A_BUTTON);
}

static uint32_t seedNow() { return micros() * 2654435761u ^ arduboy.frameCount; }

// ---------------------------------------------------------------------------
// Title: the board in its starting position, the camera drifting over it.
// ---------------------------------------------------------------------------
enum Item : uint8_t { I_ONE, I_TWO, I_CONTINUE, I_OPTIONS };
static const char *const ITEM[4] = {"1 PLAYER", "2 PLAYERS", "CONTINUE", "OPTIONS"};

static uint8_t titleItems(uint8_t *items) {
    uint8_t n = 0;
    if (hasGame) items[n++] = I_CONTINUE;
    items[n++] = I_ONE;
    items[n++] = I_TWO;
#if !CHCH_LEAN
    items[n++] = I_OPTIONS;
#endif
    return n;
}

static void newGame(uint8_t mode) {
    match::Setup s;
    s.mode = mode;
    s.level = opt.level;
    s.humanBlack = opt.side == 2 ? (uint8_t)(fx::rnd() & 1) : opt.side;
    s.seed = seedNow();
    match::start(s);
    overlay = NONE;
    statsCounted = false;
    go(Scr::Play);
}

static void titleUpdate() {
    uint8_t items[5], n = titleItems(items);
    if (sel >= n) sel = 0;
    if (menuNav(n)) {
        audio::sfx(Sfx::Select);
        switch (items[sel]) {
            case I_ONE: go(Scr::Setup); break;
            case I_TWO: newGame(match::TWO_PLAYER); break;
            case I_CONTINUE:
                if (save::loadGame()) { overlay = NONE; statsCounted = false; go(Scr::Play); }
                else { hasGame = false; audio::sfx(Sfx::Deny); }
                break;
#if !CHCH_LEAN
            case I_OPTIONS: optBack = Scr::Title; go(Scr::Options); break;
#endif
        }
    }
    // Drift over the board, close up: along White's army, round to Black's
    // and back.
    int a = (int)t / 3;
    iso::cam.flip = false;
    iso::cam.x = (fx::isin(a) * 110) >> 8;
    iso::cam.y = 86 + ((fx::isin(a * 2 + 64) * 44) >> 8);
}

static void titleRender(uint32_t frame) {
    stage::renderScene(frame);
    dither(0, 0, 128, 34, INK, 0);
    // The top rows are FX_B, so the palette makes the logo shimmer.
    title35("CHESS", 4, 4, FX_B, GOLD, WOOD, WINE, 17);
    uint8_t items[5], n = titleItems(items);
    int y0 = 128 - n * 14 - 1;
    dither(0, y0 - 5, 128, 128 - y0 + 5, INK, 1);
    for (uint8_t i = 0; i < n; i++) menuItem(y0 + i * 14, ITEM[items[i]], i == sel, frame);
}

// ---------------------------------------------------------------------------
// Setup: the opponent and your colour.
// ---------------------------------------------------------------------------
static void setupUpdate() {
    int d = arduboy.repeat(RIGHT_BUTTON) ? 1 : (arduboy.repeat(LEFT_BUTTON) ? -1 : 0);
    if (arduboy.repeat(UP_BUTTON) && sel > 0) { sel--; audio::sfx(Sfx::Cursor); }
    if (arduboy.repeat(DOWN_BUTTON) && sel < 2) { sel++; audio::sfx(Sfx::Cursor); }
    if (d && sel == 0) { opt.level = (uint8_t)((opt.level + match::LEVELS + d) % match::LEVELS); audio::sfx(Sfx::Coin); }
    if (d && sel == 1) { opt.side = (uint8_t)((opt.side + 3 + d) % 3); audio::sfx(Sfx::Coin); }
    // Hold SELECT to wipe your record against this opponent.
    static uint8_t hold;
    hold = arduboy.pressed(SELECT_BUTTON) ? (uint8_t)(hold + 1) : 0;
    if (hold == 90) {
        stats.won[opt.level] = stats.lost[opt.level] = stats.drawn[opt.level] = 0;
        persist(hasGame);
        audio::sfx(Sfx::Capture);
    }
    if (arduboy.justPressed(A_BUTTON)) {
        if (sel < 2) { sel++; audio::sfx(Sfx::Cursor); }
        else { audio::sfx(Sfx::Select); persist(hasGame); newGame(match::VS_CPU); }
    }
    if (arduboy.justPressed(B_BUTTON)) { audio::sfx(Sfx::Select); go(Scr::Title); }
}

static void arrows(int y, int w, bool on, uint32_t frame) {
    if (!on) return;
    int bob = (frame >> 3) & 1;
    text35(64 - w / 2 - 9 - bob, y + 1, "<", GOLD);
    text35(64 + w / 2 + 6 + bob, y + 1, ">", GOLD);
}

// A setup choice: boxed while chosen, with arrows to change it.
static void choice(int y, const char *s, bool on, uint32_t frame) {
    int w = text35x2Width(s);
    if (on) fillRound(64 - w / 2 - 5, y - 3, w + 10, 15, 3, NAVY);
    centred2(y, s, on ? GOLD : WHITE);
    arrows(y + 2, w, on, frame);
}

static void setupRender(uint32_t frame) {
    feltBackdrop();
    title35("OPPONENT", 10, 3, FX_B, GOLD, WOOD, WINE, 13);
    // Stars for how hard they play.
    for (int i = 0; i <= opt.level; i++) text35(64 - opt.level * 4 + i * 8, 36, "*", FX_B);
    choice(51, OPPONENT[opt.level], sel == 0, frame);
    centred35(66, OPP_LINE[opt.level], FELT_LT);
    // Your record against them.
    char buf[24], *p = fmtStr(buf, "WON ");
    p = fmtInt(p, stats.won[opt.level]); p = fmtStr(p, " LOST ");
    p = fmtInt(p, stats.lost[opt.level]); p = fmtStr(p, " DRAWN ");
    fmtInt(p, stats.drawn[opt.level]);
    centred35(72, buf, GOLD);

    static const char *const SIDE[3] = {"PLAY WHITE", "PLAY BLACK", "RANDOM SIDE"};
    choice(81, SIDE[opt.side], sel == 1, frame);
    menuItem(104, "BEGIN", sel == 2, frame);
}

// ---------------------------------------------------------------------------
// Play
// ---------------------------------------------------------------------------
static void tryTarget(uint8_t sq) {
    uint8_t from = stage::selected();
    if (match::needsPromotion(from, sq)) {
        overlay = PROMO;
        promoFrom = from; promoTo = sq; promoSel = 0;
        audio::sfx(Sfx::Flip);
        return;
    }
    if (match::play(from, sq)) stage::deselect();
}

// A tap of B puts a piece back down. Held (or with a direction), it
// inspects the board: the camera zooms right in and the D-pad pushes the
// view to the board's edges and corners until B is let go. Holding a piece,
// a tap is anything up to HOLD_B frames, while a bar grows under the HUD.
static const uint8_t HOLD_B = 32;
static bool bUsed;
static uint8_t bHeld;
static uint8_t holdBar() {
    return bHeld && !bUsed && stage::selected() != 0xFF ? (uint8_t)(bHeld * (128 / HOLD_B)) : 0;
}
static void lookAround() {
    if (arduboy.justPressed(B_BUTTON)) bUsed = false;
    bHeld = arduboy.pressed(B_BUTTON) ? (uint8_t)(bHeld < 255 ? bHeld + 1 : 255) : 0;
    int dx = 0, dy = 0;
    if (bHeld) {
        if (arduboy.pressed(LEFT_BUTTON)) dx = -1;
        if (arduboy.pressed(RIGHT_BUTTON)) dx = 1;
        if (arduboy.pressed(UP_BUTTON)) dy = -1;
        if (arduboy.pressed(DOWN_BUTTON)) dy = 1;
        if (dx || dy || bHeld > (stage::selected() != 0xFF ? HOLD_B : 8)) bUsed = true;
    }
    stage::inspect(bHeld && bUsed, dx, dy);
}

static void cycleView() {
    stage::setView((uint8_t)((stage::view() + 1) % stage::VIEWS));
    audio::sfx(Sfx::Whoosh);
}

// The cursor visits your pieces (the plate says when one is stuck) - in
// check, only those that can get you out of it - or, once one is picked up,
// the squares it can go to.
static uint8_t spots(uint8_t *list) {
    uint8_t s = stage::selected(), cap[32], n = 0;
    if (s != 0xFF) return match::movesFrom(s, list, cap);
    // In check, the pieces that can move - worked out once a position.
    static uint32_t at;
    static uint8_t can[16], nCan;
    bool check = match::checkSq != 0xFF;
    uint32_t key = (uint32_t)match::plies << 16 | match::lastFrom << 8 | match::lastTo;
    if (check && key != at) {
        uint8_t to[32];
        at = key;
        nCan = 0;
        for (uint8_t sq = 0; sq < 64; sq++)
            if (match::board[sq] && ((match::board[sq] & eng::BLACK) != 0) == match::blackToMove() &&
                match::movesFrom(sq, to, cap))
                can[nCan++] = sq;
    }
    if (check) { memcpy(list, can, nCan); return nCan; }
    for (uint8_t sq = 0; sq < 64; sq++) {
        uint8_t p = match::board[sq];
        if (p && ((p & eng::BLACK) != 0) == match::blackToMove()) list[n++] = sq;
    }
    return n;
}

// The spot nearest `from` in screen direction (ux, uy) - diagonals and all,
// whatever the view - preferring ones straight ahead. With none that way it
// wraps round to the farthest the other way, so pressing on steps through
// every spot. (0, 0): simply the nearest.
static uint8_t nearest(const uint8_t *list, uint8_t n, uint8_t from, int ux, int uy) {
    int fx, fy;
    iso::worldOf(from, fx, fy);
    uint8_t best = 0xFF, back = 0xFF;
    int32_t bestS = 0x7FFFFFFF, backS = 0x7FFFFFFF;
    for (uint8_t i = 0; i < n; i++) {
        if (list[i] == from) continue;
        int x, y;
        iso::worldOf(list[i], x, y);
        int dx = x - fx, dy = y - fy;
        int along = dx * ux + dy * uy, side = dx * uy - dy * ux;
        if (side < 0) side = -side;
        if (!ux && !uy) along = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
        int32_t sc = along > 0 ? along + 2 * side : 4 * along + side;
        if (along > 0 && sc < bestS) { bestS = sc; best = list[i]; }
        if (along <= 0 && sc < backS) { backS = sc; back = list[i]; }
    }
    return best != 0xFF ? best : back;
}

static void playInput() {
    uint8_t list[32], n = spots(list), c = stage::cursor(), s = stage::selected();
    if (!n) return;
    // A new turn (or an undo) may leave the cursor on nothing playable.
    bool on = false;
    for (uint8_t i = 0; i < n; i++) on |= list[i] == c;
    if (!on) stage::setCursor(c = nearest(list, n, c, 0, 0));
    int ux = 0, uy = 0;
    if (!arduboy.pressed(B_BUTTON)) {
        if (arduboy.repeat(UP_BUTTON)) uy = -1;
        if (arduboy.repeat(DOWN_BUTTON)) uy = 1;
        if (arduboy.repeat(LEFT_BUTTON)) ux = -1;
        if (arduboy.repeat(RIGHT_BUTTON)) ux = 1;
    }
    if (ux || uy) {
        uint8_t t = nearest(list, n, c, ux, uy);
        if (t != 0xFF) { stage::setCursor(t); audio::sfx(Sfx::Cursor); }
        else audio::sfx(Sfx::Deny);
    }
    if (arduboy.justReleased(B_BUTTON) && !bUsed && s != 0xFF) {
        stage::deselect();
        stage::setCursor(s);                 // back onto the piece
        audio::sfx(Sfx::Cursor);
    }
    c = stage::cursor();
    uint8_t to[32], cap[32], k = s == 0xFF ? match::movesFrom(c, to, cap) : 1;
    stage::setBlocked(!k);
    if (arduboy.justPressed(A_BUTTON) && !bHeld) {
        if (s != 0xFF) tryTarget(c);
        else if (!k) stage::deny();
        else {
            stage::select(c, to, cap, k);
            stage::setCursor(nearest(to, k, c, 0, 0));
        }
    }
}

static const uint8_t PROMO_TO[4] = {eng::QUEEN, eng::ROOK, eng::BISHOP, eng::KNIGHT};

static void promoInput() {
    if (arduboy.repeat(LEFT_BUTTON) || arduboy.repeat(UP_BUTTON)) { promoSel = (uint8_t)((promoSel + 3) & 3); audio::sfx(Sfx::Flip); }
    if (arduboy.repeat(RIGHT_BUTTON) || arduboy.repeat(DOWN_BUTTON)) { promoSel = (uint8_t)((promoSel + 1) & 3); audio::sfx(Sfx::Flip); }
    if (arduboy.justPressed(B_BUTTON)) { overlay = NONE; audio::sfx(Sfx::Cursor); }
    if (arduboy.justPressed(A_BUTTON)) {
        overlay = NONE;
        if (match::play(promoFrom, promoTo, PROMO_TO[promoSel])) stage::deselect();
    }
}

enum Action : uint8_t { A_NONE, A_RESUME, A_UNDO, A_RESIGN, A_QUIT };
static const char *const PAUSE_ITEM[4] = {"RESUME", "UNDO", "RESIGN", "SAVE + QUIT"};

static void doAction(uint8_t a) {
    switch (a) {
        case A_UNDO:
            if (match::undo()) audio::sfx(Sfx::Whoosh);
            else audio::sfx(Sfx::Deny);
            break;
        case A_RESIGN: match::resign(); break;
        case A_QUIT:
            persist(match::active());
            go(Scr::Title);
            break;
    }
}

static void pauseInput(bool thinking) {
    bool start = arduboy.justPressed(START_BUTTON);
    if (menuNav(4) || start) {
        uint8_t a = start ? (uint8_t)A_RESUME : (uint8_t)(sel + 1);
        overlay = NONE;
        audio::sfx(Sfx::Select);
        if (a == A_RESUME) return;
        if (thinking) { pendingAction = a; match::abortThink(); }
        else doAction(a);
        return;
    }
    if (arduboy.justPressed(B_BUTTON)) overlay = NONE;
}

static void countResult() {
    if (statsCounted || match::setup.mode != match::VS_CPU) return;
    statsCounted = true;
    uint8_t lv = match::setup.level;
    bool white = match::setup.humanBlack == 0;
    switch (match::result) {
        case match::WHITE_WINS: if (white) stats.won[lv]++; else stats.lost[lv]++; break;
        case match::BLACK_WINS: if (white) stats.lost[lv]++; else stats.won[lv]++; break;
        default: stats.drawn[lv]++; break;
    }
}

static void playUpdate(bool thinking) {
    if (thinking) {
        // Mid-search: the pause menu and the view toggle work; the game waits.
        if (overlay == PAUSE) pauseInput(true);
        else if (arduboy.justPressed(START_BUTTON) && !bHeld) { overlay = PAUSE; sel = 0; audio::sfx(Sfx::Select); }
        if (overlay == NONE) lookAround();
        if (arduboy.justPressed(SELECT_BUTTON) && !bHeld) cycleView();
        stage::update();
        return;
    }
    if (pendingAction) { uint8_t a = pendingAction; pendingAction = 0; doAction(a); }
    switch (overlay) {
        case PAUSE: pauseInput(false); break;
        case PROMO: promoInput(); break;
        case RESULT:
            if (arduboy.justPressed(A_BUTTON)) {
                audio::sfx(Sfx::Select);
                persist(false);
                if (match::setup.mode == match::VS_CPU) {
                    opt.side = match::setup.humanBlack ? 0 : 1;      // swap sides for the rematch
                    newGame(match::VS_CPU);
                } else newGame(match::TWO_PLAYER);
            }
            if (arduboy.justPressed(B_BUTTON)) { audio::sfx(Sfx::Select); persist(false); go(Scr::Title); }
            break;
        default:
            lookAround();
            if (stage::waiting()) {
                if (arduboy.justPressedMask()) { stage::acknowledge(); audio::sfx(Sfx::Select); }
                break;
            }
            if (arduboy.justPressed(START_BUTTON) && !bHeld) { overlay = PAUSE; sel = 0; audio::sfx(Sfx::Select); break; }
            if (arduboy.justPressed(SELECT_BUTTON) && !bHeld) cycleView();
            if (match::humanToMove() && !stage::busy()) playInput();
            break;
    }
#if CHCH_DEBUG
    uint32_t t0 = millis();
#endif
    match::update(stage::busy());          // the CPU's search runs in here
#if CHCH_DEBUG
    if (millis() - t0 > 100) thinkMs = millis() - t0;
#endif
    stage::update();
    if (!match::active() && stage::overShown() && overlay != RESULT) {
        countResult();
        overlay = RESULT;
    }
}

static void panel(int y, int h) {
    fillRound(14, y, 100, h, 3, NAVY);
    roundRect(14, y, 100, h, 3, GOLD);
}

static void playRender(uint32_t frame) {
    uint8_t bar = overlay ? 0 : holdBar();
    uint32_t ui = overlay | (sel << 4) | (promoSel << 8) | ((uint32_t)bar << 16);
    if (overlay) ui ^= (frame >> 3) << 12;            // blinking arrows and borders
    if (!stage::render(frame, ui)) return;
    gfx_fillRect(0, 10, bar, 2, CYAN);                // B held: how long till it inspects
    if (overlay == PAUSE) {
        panel(32, 64);
        for (uint8_t i = 0; i < 4; i++) {
            bool dim = i == 1 && !match::canUndo() && !match::cpuThinking();
            int y = 38 + i * 14;
            if (i == sel) fillRound(18, y - 3, 92, 15, 3, INK);
            centred2(y, PAUSE_ITEM[i], dim ? SILVER : (i == sel ? FX_B : WHITE));
        }
    } else if (overlay == PROMO) {
        // A slot-machine reel of the four pieces; the pawn becomes the one showing.
        panel(26, 76);
        centred35(30, "PROMOTE TO", GOLD);
        uint8_t colour = match::blackToMove() ? eng::BLACK : 0;
        gfx_fillRect(44, 38, 40, 52, INK);
        gfx_rect(43, 37, 42, 54, GOLD);
        stage::drawPieceAt((uint8_t)(PROMO_TO[promoSel] | colour), 64, 82);
        arrows(59, 42, true, frame);
        centred35(94, stage::NAMES[PROMO_TO[promoSel]], WHITE);
    } else if (overlay == RESULT) {
        panel(84, 40);
        const char *head, *why = "";
        bool vsCpu = match::setup.mode == match::VS_CPU;
        bool whiteWon = match::result == match::WHITE_WINS, blackWon = match::result == match::BLACK_WINS;
        if (whiteWon || blackWon) {
            if (vsCpu) head = whiteWon == (match::setup.humanBlack == 0) ? "YOU WIN!" : "YOU LOSE";
            else head = whiteWon ? "WHITE WINS" : "BLACK WINS";
            why = match::reason == match::BY_MATE ? "BY CHECKMATE" : "BY RESIGNATION";
        } else {
            head = match::result == match::STALEMATE ? "STALEMATE" : "DRAW";
            static const char *const W[] = {"", "", "", "", "FIFTY MOVES", "REPETITION", "NO MATERIAL"};
            why = W[match::result];
        }
        centred2(87, head, FX_B);
        centred35(99, why, SILVER);
        centred35(111, vsCpu ? "A REMATCH   B MENU" : "A AGAIN   B MENU", WHITE);
    }
}

// ---------------------------------------------------------------------------
// Options (device debug builds leave the screen out to fit the protocol:
// their tests start games directly)
// ---------------------------------------------------------------------------
#if !CHCH_LEAN
enum Opt : uint8_t { O_SOUND, O_FELT, O_COORDS, O_SPEED, O_BACK, OPT_COUNT };
static const char *const OPT_TEXT[OPT_COUNT] = {
    "SOUND|OFF|ON", "BOARD|GREEN|BLUE|RED|PURPLE", "COORDS|OFF|ON", "PACE|FUN|QUICK", "BACK",
};
// The option's byte in Options (past the retired hints byte).
static uint8_t &optByte(uint8_t i) { return ((uint8_t *)&opt)[i + (i >= O_COORDS)]; }

static uint8_t optField(const char *s, uint8_t k, char *buf) {
    uint8_t n = 0;
    for (;;) {
        const char *e = s;
        while (*e && *e != '|') e++;
        if (n == k) { uint8_t len = (uint8_t)(e - s); memcpy(buf, s, len); buf[len] = 0; }
        n++;
        if (!*e) return n;
        s = e + 1;
    }
}

static void optionsUpdate() {
    if (arduboy.repeat(UP_BUTTON)) { sel = (uint8_t)((sel + OPT_COUNT - 1) % OPT_COUNT); audio::sfx(Sfx::Cursor); }
    if (arduboy.repeat(DOWN_BUTTON)) { sel = (uint8_t)((sel + 1) % OPT_COUNT); audio::sfx(Sfx::Cursor); }
    int d = arduboy.justPressed(RIGHT_BUTTON) ? 1 : (arduboy.justPressed(LEFT_BUTTON) ? -1 : 0);
    if (arduboy.justPressed(A_BUTTON) && sel != O_BACK) d = 1;
    if (d && sel != O_BACK) {
        char tmp[12];
        uint8_t n = (uint8_t)(optField(OPT_TEXT[sel], 0, tmp) - 1);
        uint8_t &f = optByte(sel);
        f = (uint8_t)((f + n + d) % n);
        applyOptions();
        audio::sfx(Sfx::Coin);
    }
    if ((arduboy.justPressed(A_BUTTON) && sel == O_BACK) || arduboy.justPressed(B_BUTTON)) {
        audio::sfx(Sfx::Select);
        persist(hasGame);
        go(optBack);
    }
}

static void optionsRender(uint32_t frame) {
    feltBackdrop();
    title35("OPTIONS", 7, 3, FX_B, GOLD, WOOD, WINE, 13);
    for (uint8_t i = 0; i < OPT_COUNT; i++) {
        int y = 29 + i * 13;
        char label[12], value[12];
        optField(OPT_TEXT[i], 0, label);
        if (i == sel) {
            fillRound(8, y - 3, 112, 15, 3, NAVY);
            roundRect(8, y - 3, 112, 15, 3, (frame & 16) ? FX_B : GOLD);
        }
        if (i == O_BACK) { centred2(y, label, i == sel ? GOLD : WHITE); continue; }
        text35x2(15, y, label, i == sel ? GOLD : WHITE);
        optField(OPT_TEXT[i], (uint8_t)(optByte(i) + 1), value);
        text35x2(114 - text35x2Width(value), y, value, i == sel ? WHITE : FELT_LT);
    }
    centred35(110, "ARDUCHESS ENGINE: PETER BROWN", SILVER);
    centred35(117, "FONT: PRESS PLAY ON TAPE", SILVER);
}
#endif

// ---------------------------------------------------------------------------
// Debug protocol hooks (tools/chsim/chdrive.py 'say')
// ---------------------------------------------------------------------------
#if CHCH_DEBUG
//   G <mode> <humanBlack> <level> <seed>   start a game (mode 0 vs CPU, 1 two players)
//   M <from> <to> [promo]                   play a move (squares 0..63)
//   J <T|S|O>                               jump to title/setup/options
//   X <fen>                                 (simulator) set up a position, two players
//   V <fen>                                 (simulator) ... you, the side to move, against the CPU
//   R <sq>                                  (simulator) the D-pad route to sq: ROUTE UDLR..
//   H                                       (simulator) the board: BOARD <64 chars> <w|b> <your turn 0|1> <waiting on a press 0|1>
static bool debugHook(char cmd, const char *args) {
    char buf[48], *p;
    switch (cmd) {
        case 'W':
            // W: the CPU's last move: wall time (frames drawn meanwhile) and nodes.
            p = fmtInt(fmtStr(buf, "THINK ms="), (int32_t)thinkMs);
            p = fmtInt(fmtStr(p, " nodes="), (int32_t)eng::nodes());
            fmtStr(p, "\n");
            dbg::print(buf);
            return true;
        case 'Y': {
            // Y: render cost by section on the board (table board overlays pieces hud fx, us).
            uint32_t us[6];
            gfx_wait();
            stage::profile(us);
            p = fmtStr(buf, "RPROF");
            for (int k = 0; k < 6; k++) { *p++ = ' '; p = fmtInt(p, (int32_t)us[k]); }
            fmtStr(p, "\n");
            dbg::print(buf);
            return true;
        }
        case 'G': {
            match::Setup s;
            s.mode = (uint8_t)dbg::parseNum(args, 10);
            s.humanBlack = (uint8_t)dbg::parseNum(args, 10);
            s.level = (uint8_t)dbg::parseNum(args, 10);
            s.seed = dbg::parseNum(args, 10);
            match::start(s);
            overlay = NONE; statsCounted = false;
            enter(Scr::Play);
            return true;
        }
        case 'M': {
            uint8_t f = (uint8_t)dbg::parseNum(args, 10), to = (uint8_t)dbg::parseNum(args, 10);
            uint8_t p = (uint8_t)dbg::parseNum(args, 10);
            return match::play(f, to, p ? p : eng::QUEEN);
        }
#ifdef CHSIM
        case 'J': {
            static const char K[] = "TSO";
            const char *q = strchr(K, args[0]);
            if (!q) return false;
            static const Scr S[] = {Scr::Title, Scr::Setup, Scr::Options};
            enter(S[q - K]);
            return true;
        }
#endif
#ifdef CHSIM
        case 'Q': {
            // Calibration for tools/chsim/perf.py: host ns for the primitives
            // the CHGfx benchmark measured on the board (benchmark-results.txt).

            static uint8_t spr[8 * 16];
            memset(spr, 0x3F, sizeof spr);
            uint64_t t0, r[5];
            t0 = sim_hostNanos(); for (int i = 0; i < 200; i++) gfx_clear((uint8_t)i); r[0] = (sim_hostNanos() - t0) / 200;
            t0 = sim_hostNanos(); for (int i = 0; i < 20000; i++) gfx_hline(0, i & 127, 128, (uint8_t)i); r[1] = (sim_hostNanos() - t0) / 20000;
            t0 = sim_hostNanos(); for (int i = 0; i < 2000; i++) gfx_blit(spr, i & 63, i & 63, 16, 16, 15); r[2] = (sim_hostNanos() - t0) / 2000;
            t0 = sim_hostNanos(); for (int i = 0; i < 1000; i++) gfx_text(0, i & 63, "ABCDEFGHIJKLMNOPQRSTUVWX", 1); r[3] = (sim_hostNanos() - t0) / 1000;
            t0 = sim_hostNanos(); for (int i = 0; i < 1000; i++) gfx_fillCircle(64, 64, 30, (uint8_t)i); r[4] = (sim_hostNanos() - t0) / 1000;
            char buf[96], *p = fmtStr(buf, "CAL");
            for (int k = 0; k < 5; k++) { *p++ = ' '; p = fmtInt(p, (int32_t)r[k]); }
            fmtStr(p, "\n");
            dbg::print(buf);
            return true;
        }
        case 'R': {
            // R <sq>: the D-pad presses (U D L R) that take the glove to sq,
            // as the cursor steps between its spots (fewest presses).
            uint8_t list[32], n = spots(list), c = stage::cursor(), to = (uint8_t)dbg::parseNum(args, 10);
            bool on = false;
            for (uint8_t i = 0; i < n; i++) on |= list[i] == c;
            if (!on && n) c = nearest(list, n, c, 0, 0);
            static const int8_t DX[4] = {0, 0, -1, 1}, DY[4] = {-1, 1, 0, 0};
            uint8_t from[64], how[64], q[64], qh = 0, qt = 0;
            memset(from, 0xFF, sizeof from);
            from[c] = c; q[qt++] = c;
            while (qh < qt && from[to] == 0xFF) {
                uint8_t sq = q[qh++];
                for (uint8_t d = 0; d < 4; d++) {
                    uint8_t t = nearest(list, n, sq, DX[d], DY[d]);
                    if (t != 0xFF && from[t] == 0xFF) { from[t] = sq; how[t] = d; q[qt++] = t; }
                }
            }
            char path[48];
            uint8_t k = 0;
            if (from[to] == 0xFF) path[k++] = '?';
            else for (uint8_t sq = to; sq != c && k < 40; sq = from[sq]) path[k++] = "UDLR"[how[sq]];
            p = fmtStr(buf, "ROUTE ");
            while (k) *p++ = path[--k];
            fmtStr(p, "\n");
            dbg::print(buf);
            return true;
        }
        case 'H': {
            // H: the board (a1..h8, PNBRQK white, lower case black) and who moves.
            static const char L[] = ".PNBRQK";
            char b[80];
            p = fmtStr(b, "BOARD ");
            for (uint8_t sq = 0; sq < 64; sq++) {
                uint8_t pc = match::board[sq];
                char ch = L[pc & eng::TYPE];
                *p++ = (pc & eng::BLACK) ? (char)(ch | 0x20) : ch;
            }
            *p++ = ' '; *p++ = match::blackToMove() ? 'b' : 'w';
            *p++ = ' '; *p++ = match::humanToMove() && !stage::busy() && overlay == NONE ? '1' : '0';
            *p++ = ' '; *p++ = stage::waiting() ? '1' : '0';
            fmtStr(p, "\n");
            dbg::print(b);
            return true;
        }
        case 'V':
        case 'X': {
            // X <fen>: two players from a position; V <fen>: you, the side to
            // move, against the CPU (BEGINNER).
            const char *sp = strchr(args, ' ');
            match::Setup s = {cmd == 'V' ? (uint8_t)match::VS_CPU : (uint8_t)match::TWO_PLAYER,
                              (uint8_t)(sp && sp[1] == 'b'), 0, 1};
            match::startFen(s, args);
            overlay = NONE; statsCounted = false;
            enter(Scr::Play);
            return true;
        }
#endif
    }
    return false;
}
// Game commands wait while the CPU searches or plays (a scripted move
// arrives when it is the human's turn, as a press would); the render
// profile and the think report never touch the game, so they run at once.
static bool searching(char cmd) {
    if (cmd == 'Y' || cmd == 'W' || cmd == 'R' || cmd == 'H') return false;
    if (stage::waiting()) stage::acknowledge();           // a scripted command answers CHECK! as a press would
    return match::cpuThinking() || (cur == Scr::Play && match::active() && !match::humanToMove());
}
#endif

// ---------------------------------------------------------------------------
void begin() {
    stage::begin();
    opt.sound = 1; opt.coords = 1;
    save::load(opt, stats, hasGame);
    applyOptions();
#if CHCH_DEBUG
    dbg::hook = debugHook;
    dbg::holdGame = searching;
#endif
    enter(Scr::Title);
}

bool holdFrames() { return cur == Scr::Play && (overlay != NONE || bHeld); }

void update(bool thinking) {
    t++;
    if (thinking) {
        if (cur == Scr::Play) playUpdate(true);
        fx::update();
        return;
    }
    if (fadeOut) {
        pal::setFade((uint8_t)((fadeOut - 1) * 2));
        if (--fadeOut == 0) enter(pending);
        fx::update();
        return;
    }
    if (fadeIn) { fadeIn--; pal::setFade((uint8_t)(16 - fadeIn * 2)); }
    switch (cur) {
        case Scr::Title:   titleUpdate(); break;
        case Scr::Setup:   setupUpdate(); break;
        case Scr::Play:    playUpdate(false); break;
#if !CHCH_LEAN
        case Scr::Options: optionsUpdate(); break;
#endif
    }
    fx::update();
}

void render(uint32_t frame) {
    switch (cur) {
        case Scr::Title:   titleRender(frame); break;
        case Scr::Setup:   setupRender(frame); break;
        case Scr::Play:    playRender(frame); break;
#if !CHCH_LEAN
        case Scr::Options: optionsRender(frame); break;
#endif
    }
}

}  // namespace screens
