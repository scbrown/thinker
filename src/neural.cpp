
#include "neural.h"
#include "savegame.h"
#include "veh.h"

/*
The observation sink.

Deliberately NOT debug_log: `debug()` compiles to nothing outside BUILD_DEBUG
(main.h:35,46), and the A0 spike has to produce evidence from the release DLL
that people will actually run. So this writes its own file, unconditionally.

One JSON object per line, appended. JSONL because the orchestrator's decision log
is JSONL and because a partial write costs one line rather than the file.
*/
static const char* NA_LOG_PATH = "na-observations.jsonl";

static FILE* na_log = NULL;

static FILE* na_log_open() {
    if (!na_log) {
        na_log = fopen(NA_LOG_PATH, "at");
    }
    return na_log;
}

/*
Write s as a JSON string body, escaping what RFC 8259 requires.

Faction and base names are player-supplied (base names are editable in-game and
faction names come from a text file), so they are untrusted input as far as our
output format is concerned. An unescaped quote would produce a broken line that
the orchestrator silently drops — the sort of bug that shows up as "coverage is
mysteriously low" three weeks later.
*/
static void na_write_escaped(FILE* fp, const char* s) {
    if (!s) {
        return;
    }
    for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
        switch (*p) {
            case '"':  fputs("\\\"", fp); break;
            case '\\': fputs("\\\\", fp); break;
            case '\n': fputs("\\n", fp); break;
            case '\r': fputs("\\r", fp); break;
            case '\t': fputs("\\t", fp); break;
            default:
                if (*p < 0x20) {
                    fprintf(fp, "\\u%04x", *p);
                } else {
                    fputc(*p, fp);
                }
        }
    }
}

/*
Per-base-turn call counter.

The engine asks the same base for a production choice several times in one turn:
mod_base_reset is hooked at eleven call sites (patch.cpp:859-869) and each one runs
mod_base_build followed by mod_base_change — so every call *applies* its own answer,
and the last one to run is what the base actually builds.

Measured in real play: 21 of 24 base-turns fired twice, 11 of those pairs
disagreeing on the choice. has_gov does not separate them (it was 0 on all 45
samples), so the discriminator has to be positional.

call_seq is that discriminator, and it is what A1 needs: ask the orchestrator on
call_seq == 1 and reuse the answer for the rest of the base-turn. Then the first
and last call agree by construction, one decision produces one record, and the
"exactly one decision record per decision" invariant holds without having to
decide which of several engine paths is philosophically authoritative.

Indexed by base_id and invalidated on turn change, so it costs no bookkeeping
anywhere else. Base ids are stable within a turn, which is all this needs.
*/
static int na_seq_turn[MaxBaseNum];
static int na_seq_count[MaxBaseNum];
static bool na_seq_init = false;

static int na_next_call_seq(int base_id) {
    if (!na_seq_init) {
        for (int i = 0; i < MaxBaseNum; i++) {
            na_seq_turn[i] = -1;
            na_seq_count[i] = 0;
        }
        na_seq_init = true;
    }
    if (na_seq_turn[base_id] != *CurrentTurn) {
        na_seq_turn[base_id] = *CurrentTurn;
        na_seq_count[base_id] = 0;
    }
    return ++na_seq_count[base_id];
}

/*
The action space: every build this base may legally start, with what it costs.

Engine-authoritative by construction — membership comes from can_build_unit and
can_build (base.h:68-69), never from our own idea of what is buildable. That is what
makes an illegal order impossible rather than merely unlikely, and it is why this
enumerates and filters rather than constructing a list from rules.

Cost and effect are here because a name alone is not a decidable comparison.
"Formers" versus "Scout Patrol" cannot be weighed without cost, and a brain that
cannot compare cost will systematically over-pick expensive items
(docs/decision-inputs.md 1.1). Facilities carry the engine's own effect string,
which comes from alphax.txt — so the description is the game's, not ours.

Ids are "unit:<proto>" / "facility:<id>" rather than bare integers, mirroring the
engine's own encoding (item >= 0 is a unit proto, item < 0 is facility -item) while
keeping raw engine numbers out of the contract as opaque ints.
*/
static void na_write_action_space(FILE* fp, int base_id) {
    int count = 0;
    const int faction_id = Bases[base_id].faction_id;

    /*
    Costs are normalised to MINERALS here.

    The engine stores item cost in "rows"; actual minerals is cost * cost_factor, which
    varies by faction and difficulty (main.h:255 lists 13,12,11,10,8,7 by level). The base
    state reports minerals_accumulated in raw minerals, so shipping raw row costs
    alongside it would mix two units in one world view - and the brain would do confident
    arithmetic on incompatible numbers. Measured before this fix: Colony Pod cost 3
    against minerals_accumulated 4, which reads as almost affordable and is actually 30
    versus 4.
    */
    const int mineral_factor = mod_cost_factor(faction_id, RSC_MINERAL, -1);

    fputs(",\"action_space\":[", fp);

    for (int id = 0; id < MaxProtoNum; id++) {
        /*
        mod_veh_avail, NOT can_build_unit. can_build_unit checks only proto-slot
        ownership, the colony/nutrient rule, sea adjacency and the unit cap - it never
        checks whether the prerequisite tech is known. Using it yielded 125 options for a
        turn-35 base, including Alien Artifact, which is not buildable at all.
        */
        if (!mod_veh_avail(id, faction_id, base_id)) {
            continue;
        }
        if (!can_build_unit(base_id, id)) {
            continue;
        }
        if (count++) { fputs(",", fp); }
        fprintf(fp, "{\"id\":\"unit:%d\",\"name\":\"", id);
        na_write_escaped(fp, Units[id].name);
        int rows = mod_veh_cost(id, base_id, NULL);
        fprintf(fp, "\",\"cost\":%d,\"category\":\"unit\"}", rows * mineral_factor);
    }

    for (int id = 1; id <= SP_ID_Last; id++) {
        // Same reasoning: mod_facility_avail is the engine's own availability test.
        if (!mod_facility_avail((FacilityId)id, faction_id, base_id, 0)) {
            continue;
        }
        if (!can_build(base_id, id)) {
            continue;
        }
        if (count++) { fputs(",", fp); }
        fprintf(fp, "{\"id\":\"facility:%d\",\"name\":\"", id);
        na_write_escaped(fp, Facility[id].name);
        fputs("\",\"effect\":\"", fp);
        na_write_escaped(fp, Facility[id].effect);
        fprintf(fp, "\",\"cost\":%d,\"maint\":%d,\"category\":\"%s\"}",
            Facility[id].cost * mineral_factor, Facility[id].maint,
            id >= SP_ID_First ? "project" : "facility");
    }

    fprintf(fp, "],\"action_space_size\":%d,\"cost_unit\":\"minerals\"", count);
}

/*
The base's own state — category 1 of the input checklist.

Ships accumulated minerals and surplus separately rather than a pre-computed
"turns remaining". A partially built item makes that arithmetic subtly wrong, and a
number that is quietly wrong is worse than two numbers the brain can divide.
*/
static void na_write_base_state(FILE* fp, int base_id) {
    BASE& b = Bases[base_id];
    fputs(",\"base_state\":{", fp);
    fprintf(fp, "\"pop_size\":%d", (int)b.pop_size);
    fprintf(fp, ",\"minerals_accumulated\":%d", b.minerals_accumulated);
    fprintf(fp, ",\"mineral_surplus\":%d", b.mineral_surplus);
    fprintf(fp, ",\"nutrient_intake\":%d", b.nutrient_intake);
    fprintf(fp, ",\"mineral_intake\":%d", b.mineral_intake);
    fprintf(fp, ",\"energy_intake\":%d", b.energy_intake);
    fprintf(fp, ",\"eco_damage\":%d", b.eco_damage);
    fprintf(fp, ",\"worked_tiles\":%d", b.worked_tiles);
    fprintf(fp, ",\"specialists\":%d", b.specialist_total);
    fprintf(fp, ",\"queue_size\":%d", b.queue_size);
    fprintf(fp, ",\"current_item\":%d", b.item());
    fputs(",\"current_item_name\":\"", fp);
    na_write_escaped(fp, prod_name(b.item()));
    fputs("\"}", fp);
}

void na_observe_base_production(int base_id, int native_choice, int has_gov) {
    if (base_id < 0 || base_id >= *BaseCount || base_id >= MaxBaseNum) {
        return;
    }
    FILE* fp = na_log_open();
    if (!fp) {
        return;
    }
    BASE& base = Bases[base_id];
    int faction_id = base.faction_id;

    // surface_id is the contract's coverage key and must match the frozen
    // registry in the orchestrator (surfaces.py: "base.production").
    fprintf(fp, "{\"surface_id\":\"base.production\",\"engine\":\"thinker\"");
    fprintf(fp, ",\"turn\":%d", *CurrentTurn);
    fprintf(fp, ",\"faction_id\":%d", faction_id);
    fputs(",\"faction\":\"", fp);
    if (faction_id > 0 && faction_id < MaxPlayerNum) {
        na_write_escaped(fp, MFactions[faction_id].noun_faction);
    }
    fputs("\"", fp);
    fprintf(fp, ",\"base_id\":%d", base_id);
    fputs(",\"base\":\"", fp);
    na_write_escaped(fp, base.name);
    fputs("\"", fp);
    fprintf(fp, ",\"x\":%d,\"y\":%d", base.x, base.y);

    // Thinker's own pick. Recorded, not overridden: until A1 this is still what
    // the engine executes, so the log shows what the deterministic tier chose.
    fprintf(fp, ",\"native_choice\":%d", native_choice);
    // has_gov is mod_base_build's own second parameter. Kept because it is free and
    // documents the call's context, but it does NOT discriminate between repeated
    // calls — measured 0 on every sample. call_seq is what does.
    fprintf(fp, ",\"has_gov\":%d", has_gov);
    fprintf(fp, ",\"call_seq\":%d", na_next_call_seq(base_id));
    fputs(",\"native_choice_name\":\"", fp);
    na_write_escaped(fp, prod_name(native_choice));
    fputs("\"", fp);
    na_write_base_state(fp, base_id);
    na_write_action_space(fp, base_id);
    fputs(",\"tier\":\"deterministic\",\"applied\":\"native\"}\n", fp);

    // Flushed per line: a crash mid-turn is exactly when we most want the log,
    // and terranx.exe crashing is not hypothetical.
    fflush(fp);
}

/*
The menu-to-session transition, found by disassembling terranx.exe.

GameHalted (0x68F21C) is written in seven places; only 0x58F4D8 and 0x5ADCE4 clear
it. The 0x58F4D8 write is the last statement of a two-argument function at 0x58F450
which does teardown, calls 0x58F2F0(1, arg2), and un-halts only if that succeeds.
Poking GameHalted directly - which is what the first autoload attempt did - performs
the last line of that function and skips everything else, which is exactly why the
state loaded but the game stayed on the menu.

0x58F2F0 sets GameHalted=1 itself and does timing/seed initialisation; it does not
read a savegame. So the order is load_daemon first, then this.

Argument meanings are not yet known. arg1 gates a teardown block, so it plausibly
distinguishes starting fresh from replacing a running game. Exposed through the
command channel rather than hardcoded so the pair can be found by experiment against
a live game instead of a rebuild per guess.
*/
typedef int(__cdecl *Fna_enter_game)(int, int);
static Fna_enter_game na_enter_game = (Fna_enter_game)0x58F450;

/*
The real transition, read off the engine's own replay/undo path at 0x5ADCD0:

    load_daemon(filename, 0)   // note flag 0, not 1
    call 0x5FD120              // cdecl, no arguments
    GameHalted = 0

That is a complete load-and-resume the engine performs on itself, which makes it a
far better model than 0x58F450 - that one un-halts but also raises the engine's own
load prompt, because it expects to drive the load rather than be handed loaded state.

The instruction after the GameHalted write (call 0x616200 with ecx pointing at a stack
local) is a local object's cleanup, not part of the transition, so it is not
replicated here.
*/
// Set by the window procedure; used for posted input from anywhere in this file.
static volatile HWND na_input_hwnd = NULL;

typedef void(__cdecl *Fna_post_load)();
static Fna_post_load na_post_load = (Fna_post_load)0x5FD120;

/*
Display/subsystem init from 0x58F450's tail (0x50F440, 0x6169D0, 0x616950 with
ecx=0x9B90D8) was tried here and had no effect: state loaded, display stayed on the
startup screen. Removed rather than left dead. The working approach does not need it,
because the engine has already brought the display up by the time we load.
*/

/*
Startup screen button positions as fractions of the client area, measured at 2560x1440.
Fractions rather than pixels so a different window size does not silently click empty
space. QUICK START is the fourth of seven right-aligned buttons.
*/
static const double NA_QUICKSTART_FX = 2370.0 / 2560.0;
static const double NA_QUICKSTART_FY = 1034.0 / 1440.0;

static void na_post_click_frac(HWND hwnd, double fx, double fy) {
    RECT rc;
    if (!hwnd || !GetClientRect(hwnd, &rc)) {
        return;
    }
    int x = (int)((rc.right - rc.left) * fx);
    int y = (int)((rc.bottom - rc.top) * fy);
    LPARAM pos = MAKELPARAM(x, y);
    PostMessageA(hwnd, WM_MOUSEMOVE, 0, pos);
    PostMessageA(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, pos);
    PostMessageA(hwnd, WM_LBUTTONUP, 0, pos);
}

static void na_autoload_log(const char* event, const char* detail, int ok) {
    FILE* fp = na_log_open();
    if (!fp) {
        return;
    }
    fprintf(fp, "{\"surface_id\":\"na.autoload\",\"engine\":\"thinker\",\"event\":\"%s\"", event);
    fputs(",\"detail\":\"", fp);
    na_write_escaped(fp, detail);
    fprintf(fp, "\",\"halted\":%d,\"turn\":%d,\"ok\":%s}\n",
            *GameHalted, *CurrentTurn, ok ? "true" : "false");
    fflush(fp);
}

enum NaAutoState {
    NA_AS_WAIT_STARTUP = 0,
    NA_AS_WAIT_PLANETFALL,
    NA_AS_WAIT_SESSION,
    NA_AS_DONE,
};

void na_autoload_tick() {
    static int state = NA_AS_WAIT_STARTUP;
    static DWORD first_tick = 0;
    static DWORD state_since = 0;
    static bool announced = false;

    if (state == NA_AS_DONE) {
        return;
    }

    if (!announced) {
        announced = true;
        na_autoload_log("hook_alive", na_autoload.c_str(), 1);
    }
    if (na_autoload.empty()) {
        state = NA_AS_DONE;
        return;
    }

    DWORD now = GetTickCount();
    if (first_tick == 0) {
        first_tick = now;
        state_since = now;
    }
    DWORD in_state = now - state_since;

    switch (state) {
    case NA_AS_WAIT_STARTUP:
        /*
        Wait for the engine to FINISH starting, not merely to have started. Firing early
        produced a perfect log and a main menu, because the engine's own startup drew
        over everything we had just done - indistinguishable from doing nothing.
        */
        if (in_state < 12000 || !*GameHalted) {
            return;
        }
        na_post_click_frac(na_input_hwnd, NA_QUICKSTART_FX, NA_QUICKSTART_FY);
        na_autoload_log("click_quickstart", "", 1);
        state = NA_AS_WAIT_PLANETFALL;
        state_since = now;
        return;

    case NA_AS_WAIT_PLANETFALL:
        // Give the intro dialog time to appear, then confirm it.
        if (in_state < 8000) {
            return;
        }
        if (na_input_hwnd) {
            PostMessageA(na_input_hwnd, WM_KEYDOWN, VK_RETURN, 0);
            PostMessageA(na_input_hwnd, WM_KEYUP, VK_RETURN, 0);
        }
        na_autoload_log("confirm_planetfall", "VK_RETURN", 1);
        state = NA_AS_WAIT_SESSION;
        state_since = now;
        return;

    case NA_AS_WAIT_SESSION: {
        // GameHalted clearing is the real signal that a session exists.
        if (*GameHalted) {
            if (in_state > 40000) {
                na_autoload_log("give_up", "no session after quickstart", 0);
                state = NA_AS_DONE;
            }
            return;
        }
        char path[1024] = {};
        snprintf(path, sizeof(path), "%s", na_autoload.c_str());
        // flag 0 and the 0x5FD120 call, matching the engine's replay path exactly.
        int status = mod_load_daemon(path, 0);
        if (status == SAVE_LOAD_VALID) {
            na_post_load();
            *GameHalted = 0;
        }
        char detail[128];
        snprintf(detail, sizeof(detail), "%s status=%d", path, status);
        na_autoload_log("loaded", detail, status == SAVE_LOAD_VALID);
        state = NA_AS_DONE;
        return;
    }
    default:
        state = NA_AS_DONE;
        return;
    }
}

/*
=============================================================================
Command channel and in-process frame capture
=============================================================================

See neural.h for why this exists rather than driving the game from outside.
*/

static const char* NA_CMD_PATH = "na-command";
static const char* NA_CMD_RESULT = "na-command-result";

/*
Write the window's client area to a 24-bit BMP.

BMP rather than PNG because it needs no encoder: this has to work from a DLL
injected into a 1999 binary with nothing linked but user32/gdi32/winmm/psapi, and a
raw BMP is a header plus bottom-up BGR rows. Converting to PNG is the caller's job
and one ImageMagick invocation.

GetDIBits with a negative biHeight would give top-down rows, but not every driver
honours it, so this writes the natural bottom-up order that every BMP reader expects.
*/
static bool na_capture_bmp(HWND hwnd, const char* path) {
    if (!hwnd) {
        return false;
    }
    RECT rc;
    if (!GetClientRect(hwnd, &rc)) {
        return false;
    }
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) {
        return false;
    }

    HDC src = GetDC(hwnd);
    if (!src) {
        return false;
    }
    HDC mem = CreateCompatibleDC(src);
    HBITMAP bmp = CreateCompatibleBitmap(src, w, h);
    bool ok = false;

    if (mem && bmp) {
        HGDIOBJ old = SelectObject(mem, bmp);
        if (BitBlt(mem, 0, 0, w, h, src, 0, 0, SRCCOPY)) {
            BITMAPINFO bi = {};
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = w;
            bi.bmiHeader.biHeight = h;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 24;
            bi.bmiHeader.biCompression = BI_RGB;

            // Rows are padded to a 4-byte boundary — the single most common way a
            // hand-rolled BMP writer produces a skewed image.
            const int stride = ((w * 3) + 3) & ~3;
            const size_t bytes = (size_t)stride * (size_t)h;
            unsigned char* pixels = (unsigned char*)malloc(bytes);

            if (pixels && GetDIBits(mem, bmp, 0, h, pixels, &bi, DIB_RGB_COLORS)) {
                FILE* fp = fopen(path, "wb");
                if (fp) {
                    BITMAPFILEHEADER fh = {};
                    fh.bfType = 0x4D42; // "BM"
                    fh.bfOffBits = sizeof(fh) + sizeof(BITMAPINFOHEADER);
                    fh.bfSize = fh.bfOffBits + (DWORD)bytes;
                    fwrite(&fh, sizeof(fh), 1, fp);
                    fwrite(&bi.bmiHeader, sizeof(BITMAPINFOHEADER), 1, fp);
                    fwrite(pixels, 1, bytes, fp);
                    fclose(fp);
                    ok = true;
                }
            }
            free(pixels);
        }
        SelectObject(mem, old);
    }
    if (bmp) { DeleteObject(bmp); }
    if (mem) { DeleteDC(mem); }
    ReleaseDC(hwnd, src);
    return ok;
}

// Report the outcome so the caller can tell "not processed yet" from "failed".
static void na_cmd_result(const char* cmd, const char* detail, bool ok) {
    FILE* fp = fopen(NA_CMD_RESULT, "wt");
    if (!fp) {
        return;
    }
    fputs("{\"command\":\"", fp);
    na_write_escaped(fp, cmd);
    fputs("\",\"detail\":\"", fp);
    na_write_escaped(fp, detail);
    fprintf(fp, "\",\"ok\":%s", ok ? "true" : "false");
    fprintf(fp, ",\"turn\":%d", *CurrentTurn);
    fprintf(fp, ",\"halted\":%d}\n", *GameHalted);
    fclose(fp);
}

void na_command_tick(void* hwnd_raw) {
    HWND hwnd = (HWND)hwnd_raw;
    static DWORD last_poll = 0;

    /*
    The window procedure runs on every message — thousands per second while the game
    is animating. Statting a file that often is real overhead for no benefit, so poll
    at 4Hz, which is far faster than any human or agent needs.
    */
    DWORD now = GetTickCount();
    if (now - last_poll < 250) {
        return;
    }
    last_poll = now;

    FILE* fp = fopen(NA_CMD_PATH, "rt");
    if (!fp) {
        return;
    }
    char line[1024] = {};
    if (!fgets(line, sizeof(line)-1, fp)) {
        line[0] = '\0';
    }
    fclose(fp);

    // Consume the command before acting on it. If handling crashes the game, the
    // command must not re-run on the next launch and crash it again.
    remove(NA_CMD_PATH);

    // Trim trailing newline and whitespace.
    for (int i = (int)strlen(line) - 1; i >= 0; i--) {
        if (line[i] == '\n' || line[i] == '\r' || line[i] == ' ' || line[i] == '\t') {
            line[i] = '\0';
        } else {
            break;
        }
    }
    if (!line[0]) {
        return;
    }

    // "shot" or "shot <path>"
    if (strncmp(line, "shot", 4) == 0) {
        const char* path = "na-screen.bmp";
        if (line[4] == ' ' && line[5]) {
            path = line + 5;
        }
        bool ok = na_capture_bmp(hwnd, path);
        na_cmd_result("shot", path, ok);
        return;
    }

    /*
    "click <x> <y>" — client-relative, posted straight to our own window.

    This is the in-process counterpart to the external click that did not work.
    External injection has to guess which X window owns the coordinate space, and
    under a Wine virtual desktop it guesses wrong. Here the handle comes from the
    window procedure itself and the coordinates are client-relative, so there is
    nothing left to guess.

    PostMessage rather than SendMessage: we are *inside* the window procedure, and
    a synchronous send would re-enter it. Posting queues the click for the normal
    pump, which is also how a real click arrives.
    */
    if (strncmp(line, "click ", 6) == 0) {
        int x = 0, y = 0;
        if (sscanf(line + 6, "%d %d", &x, &y) == 2) {
            LPARAM pos = MAKELPARAM(x, y);
            PostMessageA(hwnd, WM_MOUSEMOVE, 0, pos);
            PostMessageA(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, pos);
            PostMessageA(hwnd, WM_LBUTTONUP, 0, pos);
            char detail[64];
            snprintf(detail, sizeof(detail), "%d,%d", x, y);
            na_cmd_result("click", detail, true);
        } else {
            na_cmd_result("click", "expected: click <x> <y>", false);
        }
        return;
    }

    /*
    "key <vk>" — a virtual-key press, by numeric VK code. Menus in this engine
    respond to the keyboard, and a keystroke needs no coordinates at all, which
    makes it far more robust than clicking if it works.
    */
    if (strncmp(line, "key ", 4) == 0) {
        int vk = 0;
        if (sscanf(line + 4, "%d", &vk) == 1 && vk > 0) {
            PostMessageA(hwnd, WM_KEYDOWN, (WPARAM)vk, 0);
            PostMessageA(hwnd, WM_KEYUP, (WPARAM)vk, 0);
            char detail[64];
            snprintf(detail, sizeof(detail), "vk=%d", vk);
            na_cmd_result("key", detail, true);
        } else {
            na_cmd_result("key", "expected: key <vk-code>", false);
        }
        return;
    }

    // "load <path>" — run the autoload loader on demand, so a save can be loaded
    // without restarting the process.
    if (strncmp(line, "load ", 5) == 0) {
        char path[1024] = {};
        snprintf(path, sizeof(path), "%s", line + 5);
        int status = mod_load_daemon(path, 1);
        char detail[128];
        snprintf(detail, sizeof(detail), "status=%d", status);
        na_cmd_result("load", detail, status == SAVE_LOAD_VALID);
        return;
    }

    // "enter <a> <b>" — call the transition at 0x58F450. Result reports GameHalted
    // afterwards, which is the whole signal: 0 means we are in a session.
    if (strncmp(line, "enter", 5) == 0) {
        int a = 1, b = 0;
        sscanf(line + 5, "%d %d", &a, &b);
        int rc = na_enter_game(a, b);
        char detail[128];
        snprintf(detail, sizeof(detail), "args=%d,%d rc=%d halted_after=%d",
                 a, b, rc, *GameHalted);
        na_cmd_result("enter", detail, *GameHalted == 0);
        return;
    }

    /*
    "observe <base_id>" — emit a base.production observation for one base without making
    a decision.

    A test hook, and deliberately side-effect free: it calls only the serializer, not
    mod_base_build, so it exercises the action-space enumeration and base-state block
    against a live game without perturbing production, minerals or the governor.

    It exists because in-game mouse and keyboard input do NOT reach the engine through
    posted messages - the startup screen and dialogs accept them, the map UI does not, it
    reads DirectInput. So there is no way to end a turn from outside, and waiting for
    natural turn upkeep is not a usable test loop.
    */
    if (strncmp(line, "observe ", 8) == 0) {
        int base_id = -1;
        if (sscanf(line + 8, "%d", &base_id) == 1
        && base_id >= 0 && base_id < *BaseCount) {
            na_observe_base_production(base_id, Bases[base_id].item(), 0);
            char detail[96];
            snprintf(detail, sizeof(detail), "base_id=%d of %d", base_id, *BaseCount);
            na_cmd_result("observe", detail, true);
        } else {
            char detail[96];
            snprintf(detail, sizeof(detail), "need 0 <= base_id < %d", *BaseCount);
            na_cmd_result("observe", detail, false);
        }
        return;
    }

    na_cmd_result(line, "unknown command", false);
}


/*
=============================================================================
Input channel — a worker thread, so it survives modal dialogs
=============================================================================

The command channel above is polled from the window procedure, which the engine stops
calling while a modal dialog runs its own nested message pump. That is exactly when we
most need to send input: the file picker is modal, and it is the one thing standing
between an unattended run and a loaded game.

So input gets its own thread. Three facts make this safe and sufficient:

  - The startup screen DOES respond to posted messages. Verified: an in-process
    click on LOAD GAME opened the picker. Driving menus was never the problem; the
    poller dying was.
  - PostMessage is thread-safe and merely queues a message. This thread never touches
    engine state, so the engine's own thread-unsafety is not in play.
  - A modal pump still dispatches queued messages, so input posted from here reaches
    the dialog exactly as a real click would.

Deliberately a SEPARATE file from the command channel. Sharing one file would mean two
readers racing to consume it, and the resulting double-dispatch would be intermittent
and horrible to debug. Two files, two owners, no race:

    na-input    -> this thread. Input only: click, key.
    na-command  -> the window procedure. Everything that touches engine state.

Anything that reads or mutates game state (shot, load, enter) must NOT be handled here:
those have to run on the engine's own thread.
*/

static const char* NA_INPUT_PATH = "na-input";
static const char* NA_INPUT_RESULT = "na-input-result";

// Set by the window procedure; read by the thread. Volatile because the thread has no
// other reason to re-read it, and a cached value would be a null handle forever.

/*
This thread must NOT use stdio. Thinker replaces the CRT's file locking with one global
FileLock mutex covering every stdio call (patch.cpp:14-20, 1148-1159), and our DLL shares
msvcrt with the game — so fopen from here takes the same lock the game holds. The file
picker does directory I/O and holds that lock across its modal wait, which deadlocked
this thread on the first fopen after the dialog opened. Measured: the thread served one
command, then stopped forever while the game stayed alive.

Win32 file APIs bypass the CRT and that lock entirely, so everything here uses
CreateFile / ReadFile / WriteFile / DeleteFile.
*/
static bool na_read_file_raw(const char* path, char* buf, DWORD cap) {
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD got = 0;
    BOOL ok = ReadFile(h, buf, cap - 1, &got, NULL);
    CloseHandle(h);
    if (!ok) {
        return false;
    }
    buf[got] = '\0';
    return true;
}

static void na_write_file_raw(const char* path, const char* text) {
    HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ,
                           NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD wrote = 0;
    WriteFile(h, text, (DWORD)strlen(text), &wrote, NULL);
    CloseHandle(h);
}

static void na_input_result(const char* cmd, const char* detail, bool ok) {
    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"channel\":\"input\",\"command\":\"%s\",\"detail\":\"%s\",\"ok\":%s,\"halted\":%d}\n",
        cmd, detail, ok ? "true" : "false", *GameHalted);
    na_write_file_raw(NA_INPUT_RESULT, buf);
}

static DWORD WINAPI na_input_thread(LPVOID) {
    unsigned long ticks = 0;
    for (;;) {
        Sleep(200);
        /*
        Heartbeat, so "the thread is dead or blocked" and "the thread is running but not
        seeing the command file" stop being the same observation. Two wrong diagnoses were
        made guessing at that difference; this settles it in one run.
        */
        ticks++;
        if (ticks % 5 == 0) {
            char hb[160];
            snprintf(hb, sizeof(hb), "{\"ticks\":%lu,\"hwnd\":%d,\"halted\":%d}\n",
                     ticks, na_input_hwnd ? 1 : 0, *GameHalted);
            na_write_file_raw("na-input-heartbeat", hb);
        }
        HWND hwnd = na_input_hwnd;
        if (!hwnd) {
            continue;
        }
        char line[512] = {};
        if (!na_read_file_raw(NA_INPUT_PATH, line, sizeof(line))) {
            continue;
        }
        DeleteFileA(NA_INPUT_PATH);
        // Only the first line matters.
        for (char* c = line; *c; c++) {
            if (*c == '\n' || *c == '\r') { *c = '\0'; break; }
        }

        for (int i = (int)strlen(line) - 1; i >= 0; i--) {
            if (line[i]=='\n'||line[i]=='\r'||line[i]==' '||line[i]=='\t') { line[i]='\0'; }
            else { break; }
        }
        if (!line[0]) {
            continue;
        }

        if (strncmp(line, "click ", 6) == 0) {
            int x = 0, y = 0;
            if (sscanf(line + 6, "%d %d", &x, &y) == 2) {
                LPARAM pos = MAKELPARAM(x, y);
                PostMessageA(hwnd, WM_MOUSEMOVE, 0, pos);
                PostMessageA(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, pos);
                PostMessageA(hwnd, WM_LBUTTONUP, 0, pos);
                char d[64]; snprintf(d, sizeof(d), "%d,%d", x, y);
                na_input_result("click", d, true);
            } else {
                na_input_result("click", "expected: click <x> <y>", false);
            }
        } else if (strncmp(line, "dclick ", 7) == 0) {
            // A double click, for list entries that need one to confirm.
            int x = 0, y = 0;
            if (sscanf(line + 7, "%d %d", &x, &y) == 2) {
                LPARAM pos = MAKELPARAM(x, y);
                PostMessageA(hwnd, WM_MOUSEMOVE, 0, pos);
                PostMessageA(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, pos);
                PostMessageA(hwnd, WM_LBUTTONUP, 0, pos);
                PostMessageA(hwnd, WM_LBUTTONDBLCLK, MK_LBUTTON, pos);
                PostMessageA(hwnd, WM_LBUTTONUP, 0, pos);
                char d[64]; snprintf(d, sizeof(d), "%d,%d", x, y);
                na_input_result("dclick", d, true);
            } else {
                na_input_result("dclick", "expected: dclick <x> <y>", false);
            }
        } else if (strncmp(line, "key ", 4) == 0) {
            int vk = 0;
            if (sscanf(line + 4, "%d", &vk) == 1 && vk > 0) {
                PostMessageA(hwnd, WM_KEYDOWN, (WPARAM)vk, 0);
                PostMessageA(hwnd, WM_KEYUP, (WPARAM)vk, 0);
                char d[64]; snprintf(d, sizeof(d), "vk=%d", vk);
                na_input_result("key", d, true);
            } else {
                na_input_result("key", "expected: key <vk-code>", false);
            }
        } else if (strncmp(line, "text ", 5) == 0) {
            // Type a string as WM_CHAR, for filling a filename field.
            const char* t = line + 5;
            for (const char* c = t; *c; c++) {
                PostMessageA(hwnd, WM_CHAR, (WPARAM)(unsigned char)*c, 0);
            }
            na_input_result("text", t, true);
        } else {
            na_input_result(line, "unknown input command", false);
        }
    }
    return 0;
}

// Start once, from the window procedure, so the handle is already valid.
void na_input_start(void* hwnd_raw) {
    static bool started = false;
    na_input_hwnd = (HWND)hwnd_raw;
    if (started) {
        return;
    }
    started = true;
    HANDLE h = CreateThread(NULL, 0, na_input_thread, NULL, 0, NULL);
    if (h) {
        CloseHandle(h);
    }
}
