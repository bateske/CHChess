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
namespace stage { extern uint64_t simLap[8]; }
#endif

namespace screens {

enum class Scr : uint8_t { Title, Setup, Play, Options, Stats };
static Scr cur = Scr::Title, pending = Scr::Title;
static uint16_t t;                   // frames on this screen
static uint8_t fadeOut, fadeIn;
static uint8_t sel;                  // menu cursor
static Scr optBack = Scr::Title;

static Options opt;
static Stats stats;
static bool hasGame;                 // a saved game is waiting

// Play-screen overlays.
enum Overlay : uint8_t { NONE, PAUSE, PROMO, RESULT };
static Overlay overlay;
static uint8_t promoFrom, promoTo, promoSel;
static uint8_t pendingAction;        // chosen from the pause menu while the CPU was thinking
static bool statsCounted;

static const char *const OPPONENT[match::LEVELS] = {"ROOKIE", "REGULAR", "SHARK", "HIGH ROLLER", "THE HOUSE"};
static const char *const OPP_LINE[match::LEVELS] = {
    "STILL LEARNING THE ROPES", "PLAYS IT STRAIGHT", "SMELLS WEAKNESS", "GOES FOR BROKE",
    "THE HOUSE ALWAYS WINS"};

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
        // The board behind the title: a fresh game, the camera drifting over it.
        match::Setup demo = {match::TWO_PLAYER, 0, 0, 1};
        match::start(demo);
        stage::update(false);
        audio::sfx(Sfx::Title);
    }
    if (s == Scr::Setup) sel = 2;
    if (s == Scr::Play) stage::opponentName = OPPONENT[match::setup.level];
}

static void persist(bool withGame) {
    gfx_wait();                      // save builds its page in the chunk scratch
    save::store(opt, stats, withGame);
    hasGame = withGame;
}

static void applyOptions() {
    audio::setOn(opt.sound != 0);
    pal::setTheme(opt.felt);
    stage::setHints(opt.hints != 0);
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
enum Item : uint8_t { I_ONE, I_TWO, I_CONTINUE, I_OPTIONS, I_STATS };
static const char *const ITEM[5] = {"1 PLAYER", "2 PLAYERS", "CONTINUE", "OPTIONS", "STATS"};

static uint8_t titleItems(uint8_t *items) {
    uint8_t n = 0;
    if (hasGame) items[n++] = I_CONTINUE;
    items[n++] = I_ONE;
    items[n++] = I_TWO;
    items[n++] = I_OPTIONS;
    items[n++] = I_STATS;
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
            case I_OPTIONS: optBack = Scr::Title; go(Scr::Options); break;
            case I_STATS: go(Scr::Stats); break;
        }
    }
    // Drift over the board: along White's army, round to Black's and back.
    int a = (int)t / 3;
    iso::cam.flip = false;
    iso::cam.x = (fx::isin(a) * 150) >> 8;
    iso::cam.y = 118 + ((fx::isin(a * 2 + 64) * 60) >> 8);
}

static void titleRender(uint32_t frame) {
    stage::renderScene(frame);
    dither(0, 0, 128, 34, INK, 0);
    // The top rows are FX_B, so the palette makes the logo shimmer.
    title35("CHESS", 4, 4, FX_B, GOLD, WOOD, WINE, 17);
    centred35(29, "~CASINO~EDITION~", CYAN);
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

static void setupRender(uint32_t frame) {
    feltBackdrop();
    title35("OPPONENT", 10, 3, FX_B, GOLD, WOOD, WINE, 13);
    // Stars for how hard they play.
    for (int i = 0; i <= opt.level; i++) text35(64 - opt.level * 4 + i * 8, 36, "*", FX_B);
    int w = text35x2Width(OPPONENT[opt.level]);
    if (sel == 0) fillRound(64 - w / 2 - 5, 48, w + 10, 15, 3, NAVY);
    centred2(51, OPPONENT[opt.level], sel == 0 ? GOLD : WHITE);
    arrows(53, w, sel == 0, frame);
    centred35(66, OPP_LINE[opt.level], FELT_LT);

    static const char *const SIDE[3] = {"PLAY WHITE", "PLAY BLACK", "RANDOM SIDE"};
    w = text35x2Width(SIDE[opt.side]);
    if (sel == 1) fillRound(64 - w / 2 - 5, 78, w + 10, 15, 3, NAVY);
    centred2(81, SIDE[opt.side], sel == 1 ? GOLD : WHITE);
    arrows(83, w, sel == 1, frame);
    menuItem(104, "DEAL ME IN", sel == 2, frame);
}

// ---------------------------------------------------------------------------
// Play
// ---------------------------------------------------------------------------
static bool ownPiece(uint8_t sq) {
    uint8_t p = match::board[sq];
    return p && ((p & eng::BLACK) != 0) == match::blackToMove();
}

// B + direction: jump to the next of your pieces that can move.
static void cycle(int dir) {
    uint8_t to[32], cap[32];
    int u0, v0;
    iso::toView(stage::cursor(), stage::flipped(), u0, v0);
    int start = v0 * 8 + u0;
    for (int k = 1; k <= 64; k++) {
        int i = (start + dir * k + 128) & 63;
        uint8_t sq = iso::fromView(i & 7, i >> 3, stage::flipped());
        if (ownPiece(sq) && match::movesFrom(sq, to, cap)) {
            stage::setCursor(sq);
            audio::sfx(Sfx::Cursor);
            return;
        }
    }
}

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

static void playInput() {
    uint8_t c = stage::cursor();
    bool held = arduboy.pressed(B_BUTTON);
    static bool bUsed;
    if (arduboy.justPressed(B_BUTTON)) bUsed = false;
    int du = 0, dv = 0;
    if (arduboy.repeat(UP_BUTTON)) dv = -1;
    if (arduboy.repeat(DOWN_BUTTON)) dv = 1;
    if (arduboy.repeat(LEFT_BUTTON)) du = -1;
    if (arduboy.repeat(RIGHT_BUTTON)) du = 1;
    if (du || dv) {
        if (held) { cycle(du + dv > 0 ? 1 : -1); bUsed = true; }
        else stage::moveCursor(du, dv);
    }
    if (arduboy.justReleased(B_BUTTON) && !bUsed && stage::selected() != 0xFF) {
        stage::deselect();
        audio::sfx(Sfx::Cursor);
    }
    if (arduboy.justPressed(A_BUTTON)) {
        uint8_t s = stage::selected();
        uint8_t to[32], cap[32];
        if (s == c) { stage::deselect(); audio::sfx(Sfx::Cursor); }
        else if (ownPiece(c)) {
            uint8_t n = match::movesFrom(c, to, cap);
            if (n) stage::select(c, to, cap, n);
            else stage::deny(c);
        } else if (s != 0xFF) {
            uint8_t n = match::movesFrom(s, to, cap);
            bool ok = false;
            for (uint8_t i = 0; i < n; i++) if (to[i] == c) ok = true;
            if (ok) tryTarget(c);
            else audio::sfx(Sfx::Deny);
        } else audio::sfx(Sfx::Deny);
    }
}

static void promoInput() {
    if (arduboy.repeat(LEFT_BUTTON) || arduboy.repeat(UP_BUTTON)) { promoSel = (uint8_t)((promoSel + 3) & 3); audio::sfx(Sfx::Flip); }
    if (arduboy.repeat(RIGHT_BUTTON) || arduboy.repeat(DOWN_BUTTON)) { promoSel = (uint8_t)((promoSel + 1) & 3); audio::sfx(Sfx::Flip); }
    if (arduboy.justPressed(B_BUTTON)) { overlay = NONE; audio::sfx(Sfx::Cursor); }
    if (arduboy.justPressed(A_BUTTON)) {
        static const uint8_t P[4] = {eng::QUEEN, eng::ROOK, eng::BISHOP, eng::KNIGHT};
        overlay = NONE;
        if (match::play(promoFrom, promoTo, P[promoSel])) stage::deselect();
    }
}

enum Action : uint8_t { A_NONE, A_RESUME, A_UNDO, A_RESIGN, A_QUIT };
static const char *const PAUSE_ITEM[4] = {"RESUME", "UNDO", "RESIGN", "SAVE & QUIT"};

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
        else if (arduboy.justPressed(START_BUTTON)) { overlay = PAUSE; sel = 0; audio::sfx(Sfx::Select); }
        if (arduboy.justPressed(SELECT_BUTTON)) { stage::setStrategy(!stage::strategy()); audio::sfx(Sfx::Whoosh); }
        stage::update(true);
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
            if (arduboy.justPressed(START_BUTTON)) { overlay = PAUSE; sel = 0; audio::sfx(Sfx::Select); break; }
            if (arduboy.justPressed(SELECT_BUTTON)) { stage::setStrategy(!stage::strategy()); audio::sfx(Sfx::Whoosh); }
            if (match::humanToMove() && !stage::busy()) playInput();
            break;
    }
    match::update(stage::busy());
    stage::update(false);
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
    uint32_t ui = overlay | (sel << 4) | (promoSel << 8);
    if (overlay) ui ^= (frame >> 3) << 12;            // blinking arrows and borders
    if (!stage::render(frame, ui)) return;
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
        static const uint8_t P[4] = {eng::QUEEN, eng::ROOK, eng::BISHOP, eng::KNIGHT};
        static const char *const N[4] = {"QUEEN", "ROOK", "BISHOP", "KNIGHT"};
        uint8_t colour = match::blackToMove() ? eng::BLACK : 0;
        gfx_fillRect(44, 38, 40, 52, INK);
        gfx_rect(43, 37, 42, 54, GOLD);
        stage::drawPieceAt((uint8_t)(P[promoSel] | colour), 64, 82);
        text35(34, 60, "<", (frame & 8) ? FX_B : GOLD);
        text35(91, 60, ">", (frame & 8) ? FX_B : GOLD);
        centred35(94, N[promoSel], WHITE);
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
// Options
// ---------------------------------------------------------------------------
enum Opt : uint8_t { O_SOUND, O_FELT, O_HINTS, O_COORDS, O_SPEED, O_BACK, OPT_COUNT };
static const char *const OPT_TEXT[OPT_COUNT] = {
    "SOUND|OFF|ON", "BOARD|GREEN|BLUE|RED|PURPLE", "HINTS|OFF|ON", "COORDS|OFF|ON", "PACE|SHOWY|QUICK",
    "BACK",
};

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
        uint8_t *f = (uint8_t *)&opt + sel;
        *f = (uint8_t)((*f + n + d) % n);
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
    title35("OPTIONS", 8, 3, FX_B, GOLD, WOOD, WINE, 13);
    for (uint8_t i = 0; i < OPT_COUNT; i++) {
        int y = 32 + i * 15;
        char label[12], value[12];
        optField(OPT_TEXT[i], 0, label);
        if (i == sel) {
            fillRound(8, y - 3, 112, 15, 3, NAVY);
            roundRect(8, y - 3, 112, 15, 3, (frame & 16) ? FX_B : GOLD);
        }
        if (i == O_BACK) { centred2(y, label, i == sel ? GOLD : WHITE); continue; }
        text35x2(15, y, label, i == sel ? GOLD : WHITE);
        optField(OPT_TEXT[i], (uint8_t)(((uint8_t *)&opt)[i] + 1), value);
        text35x2(114 - text35x2Width(value), y, value, i == sel ? WHITE : FELT_LT);
    }
}

// ---------------------------------------------------------------------------
// Stats and credits (hold SELECT to reset the stats)
// ---------------------------------------------------------------------------
static void statsUpdate() {
    static uint8_t hold;
    hold = arduboy.pressed(SELECT_BUTTON) ? (uint8_t)(hold + 1) : 0;
    if (hold == 90) {
        memset(&stats, 0, sizeof stats);
        persist(hasGame);
        audio::sfx(Sfx::Capture);
        fx::shake(10, 2);
    }
    if (arduboy.justPressed(A_BUTTON | B_BUTTON)) { audio::sfx(Sfx::Select); go(Scr::Title); }
}

static void statsRender(uint32_t frame) {
    (void)frame;
    feltBackdrop();
    title35("STATS", 8, 3, WHITE, CYAN, BLUE, NAVY, 13);
    text35(66, 31, "WON LOST DRAW", GOLD);
    for (int i = 0; i < match::LEVELS; i++) {
        int y = 40 + i * 10;
        text35(10, y, OPPONENT[i], WHITE);
        uint16_t v[3] = {stats.won[i], stats.lost[i], stats.drawn[i]};
        for (int k = 0; k < 3; k++) {
            char buf[8];
            fmtInt(buf, v[k]);
            text35(78 + k * 17 - text35Width(buf), y, buf, k == 0 ? FX_B : SILVER);
        }
    }
    centred35(91, "HOLD SELECT TO RESET", FELT_LT);
    // Credits.
    centred35(102, "CHESS ENGINE: ARDUCHESS", GOLD);
    centred35(109, "BY PETER BROWN (MPL-2.0)", SILVER);
    centred35(116, "FONT: PRESS PLAY ON TAPE", SILVER);
    fx::applyShake(0, 127);
}


// ---------------------------------------------------------------------------
// Debug protocol hooks (tools/chsim/chdrive.py 'say')
// ---------------------------------------------------------------------------
#if CHCH_DEBUG
//   G <mode> <humanBlack> <level> <seed>   start a game (mode 0 vs CPU, 1 two players)
//   M <from> <to> [promo]                   play a move (squares 0..63)
//   J <T|S|O|A>                             jump to title/setup/options/stats
//   X <fen>                                 (simulator) set up a position, two players
static bool debugHook(char cmd, const char *args) {
    switch (cmd) {
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
        case 'J': {
            static const char K[] = "TSOA";
            const char *q = strchr(K, args[0]);
            if (!q) return false;
            static const Scr S[] = {Scr::Title, Scr::Setup, Scr::Options, Scr::Stats};
            enter(S[q - K]);
            return true;
        }
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
        case 'Y': {
            // Section times of stage::render so far (host ns), then reset.
            char buf[96], *p = fmtStr(buf, "LAPS");
            for (int k = 0; k < 7; k++) { *p++ = ' '; p = fmtInt(p, (int32_t)(stage::simLap[k] / 1000)); stage::simLap[k] = 0; }
            fmtStr(p, "\n");
            dbg::print(buf);
            return true;
        }
        case 'X': {
            match::Setup s = {match::TWO_PLAYER, 0, 0, 1};
            match::startFen(s, args);
            overlay = NONE; statsCounted = false;
            enter(Scr::Play);
            return true;
        }
#endif
    }
    return false;
}
static bool searching() { return match::cpuThinking(); }
#endif

// ---------------------------------------------------------------------------
void begin() {
    stage::begin();
    opt.sound = 1; opt.hints = 1; opt.coords = 1;
    save::load(opt, stats, hasGame);
    applyOptions();
#if CHCH_DEBUG
    dbg::hook = debugHook;
    dbg::holdGame = searching;
#endif
    enter(Scr::Title);
}

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
        case Scr::Options: optionsUpdate(); break;
        case Scr::Stats:   statsUpdate(); break;
    }
    fx::update();
}

void render(uint32_t frame, bool thinking) {
    // While the CPU thinks, draw every other frame: the search gets the rest.
    if (thinking && (frame & 1)) return;
    switch (cur) {
        case Scr::Title:   titleRender(frame); break;
        case Scr::Setup:   setupRender(frame); break;
        case Scr::Play:    playRender(frame); break;
        case Scr::Options: optionsRender(frame); break;
        case Scr::Stats:   statsRender(frame); break;
    }
}

}  // namespace screens
