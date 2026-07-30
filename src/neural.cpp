
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
