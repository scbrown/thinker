
#include "neural.h"
#include "savegame.h"

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
    fputs(",\"tier\":\"deterministic\",\"applied\":\"native\"}\n", fp);

    // Flushed per line: a crash mid-turn is exactly when we most want the log,
    // and terranx.exe crashing is not hypothetical.
    fflush(fp);
}

/*
Autoload. See neural.h for why this hangs off the GUI timer.

The status code is logged unconditionally, because the failure modes are silent and
individually plausible: a path the game cannot open, a save from an older format, a
save whose header does not match the current modify_unit_limit setting. Without the
code in a log, all three present identically as "it just sat at the menu".
*/
void na_autoload_tick() {
    static bool attempted = false;
    static bool announced = false;
    static DWORD first_tick = 0;

    if (attempted) {
        return;
    }

    /*
    Announce once, whatever happens next. Without this, "the flag never parsed"
    and "the hook never ran" are the same observation: an empty log and a main
    menu. One line separates them.
    */
    if (!announced) {
        announced = true;
        FILE* dfp = na_log_open();
        if (dfp) {
            fprintf(dfp, "{\"surface_id\":\"na.autoload\",\"engine\":\"thinker\"");
            fputs(",\"event\":\"hook_alive\",\"configured\":\"", dfp);
            na_write_escaped(dfp, na_autoload.c_str());
            fputs("\"}\n", dfp);
            fflush(dfp);
        }
    }

    if (na_autoload.empty()) {
        attempted = true;
        return;
    }

    /*
    Do not load during window creation. This runs from the window procedure, so the
    first messages arrive before the engine has finished starting, and loading a
    savegame into a half-initialised game is how you get a crash that looks like a
    bad save. Wait for the app to be up and parked at the menu instead:
    GameHalted set means no game is running, and a short delay means the message
    pump is idling rather than still building the UI.
    */
    if (first_tick == 0) {
        first_tick = GetTickCount();
    }
    if (GetTickCount() - first_tick < 3000 || !*GameHalted) {
        return;
    }
    attempted = true;

    // Thinker's loader wants a mutable buffer.
    char path[1024] = {};
    snprintf(path, sizeof(path), "%s", na_autoload.c_str());

    int status = mod_load_daemon(path, 1);

    FILE* fp = na_log_open();
    if (fp) {
        fprintf(fp, "{\"surface_id\":\"na.autoload\",\"engine\":\"thinker\"");
        fputs(",\"save\":\"", fp);
        na_write_escaped(fp, path);
        fputs("\"", fp);
        fprintf(fp, ",\"status\":%d", status);
        fputs(",\"ok\":", fp);
        fputs(status == SAVE_LOAD_VALID ? "true" : "false", fp);
        fputs("}\n", fp);
        fflush(fp);
    }

    if (status == SAVE_LOAD_VALID) {
        // Resume the engine's loop. Without this the state is loaded but the game
        // stays parked at the menu, which looks exactly like a failed load.
        *GameHalted = 0;
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

    na_cmd_result(line, "unknown command", false);
}
