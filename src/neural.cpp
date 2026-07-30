
#include "neural.h"

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

void na_observe_base_production(int base_id, int native_choice) {
    if (base_id < 0 || base_id >= *BaseCount) {
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
    fputs(",\"native_choice_name\":\"", fp);
    na_write_escaped(fp, prod_name(native_choice));
    fputs("\"", fp);
    fputs(",\"tier\":\"deterministic\",\"applied\":\"native\"}\n", fp);

    // Flushed per line: a crash mid-turn is exactly when we most want the log,
    // and terranx.exe crashing is not hypothetical.
    fflush(fp);
}
