
#include "neural.h"
#include "savegame.h"
#include "veh.h"
#include "tech.h"
#include "build.h"

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

// Emit the two turn estimates for one candidate item. surplus <= 0 means never, which is
// real: a base with no mineral surplus genuinely cannot finish anything.
static void na_write_turns(FILE* fp, int cost, int surplus, int banked, bool is_current) {
    if (surplus <= 0) {
        fputs(",\"turns_if_switched\":null", fp);
        return;
    }
    int t_switch = (cost + surplus - 1) / surplus;
    fprintf(fp, ",\"turns_if_switched\":%d", t_switch);
    if (is_current) {
        int left = cost - banked;
        if (left < 0) { left = 0; }
        fprintf(fp, ",\"turns_if_continued\":%d", (left + surplus - 1) / surplus);
    }
}

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

    /*
    Turns are computed here rather than left to the brain.

    The original reasoning was to ship cost and accumulated minerals and let the brain
    divide, on the grounds that a pre-computed figure would be subtly wrong for a
    partially-built item. Measurement changed the calculus: across two runs on the same
    world view, Haiku computed (33-4)/2 correctly once and then 22/2 - silently dropping the
    4 banked minerals - the next time. An arithmetic slip in the input to a strategic
    judgement is worse than a documented approximation.

    So both numbers are given explicitly, and they are DIFFERENT numbers on purpose:

      turns_if_switched   ceil(cost / surplus), ignoring accumulated minerals. Switching
                          item category in this engine forfeits progress, so ignoring the
                          bank is the conservative and usually correct reading.
      turns_if_continued  only on the item currently in production, where the bank does
                          apply: ceil((cost - accumulated) / surplus).

    Naming them separately is the point. A single "turns" field would have to pick one
    meaning and would be wrong half the time.
    */
    BASE& b = Bases[base_id];
    const int surplus = b.mineral_surplus;
    const int banked = b.minerals_accumulated;
    const int current_item = b.item();

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
        fprintf(fp, "\",\"cost\":%d,\"category\":\"unit\"", rows * mineral_factor);
        /*
        A one-line role, because facilities carry CFacility.effect and units carried nothing
        — so a unit's purpose had to come from the model's own recollection of a 1999 game.
        Measured: Haiku, given this action space, picked Colony Pod and justified it as
        raising THIS base's population. A Colony Pod founds a new base; the arithmetic was
        right and the mechanic was wrong, and the world view gave it nothing to check against.

        Derived from UNIT.plan, which is the engine's own classification, plus triad so a
        sea unit is not proposed as a land defender.
        */
        static const char* plan_role[] = {
            "attacks enemy units and bases", "general combat unit",
            "defends a base or position", "explores and scouts terrain",
            "intercepts enemy aircraft", "destroys a base outright",
            "controls sea zones", "carries land units across water",
            "FOUNDS A NEW BASE elsewhere - does not grow this base",
            "terraforms terrain to improve tile yields",
            "ferries minerals or nutrients to another base",
            "infiltrates and sabotages rival factions",
        };
        int plan = Units[id].plan;
        fputs(",\"role\":\"", fp);
        if (plan >= 0 && plan < (int)(sizeof(plan_role)/sizeof(plan_role[0]))) {
            na_write_escaped(fp, plan_role[plan]);
        }
        int tri = Units[id].triad();
        fprintf(fp, "\",\"triad\":\"%s\"",
                tri == TRIAD_SEA ? "sea" : (tri == TRIAD_AIR ? "air" : "land"));
        na_write_turns(fp, rows * mineral_factor, surplus, banked, current_item == id);
        fputs("}", fp);
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
        fprintf(fp, "\",\"cost\":%d,\"maint\":%d,\"category\":\"%s\"",
            Facility[id].cost * mineral_factor, Facility[id].maint,
            id >= SP_ID_First ? "project" : "facility");
        na_write_turns(fp, Facility[id].cost * mineral_factor, surplus, banked,
                       current_item == -id);
        fputs("}", fp);
    }

    fprintf(fp, "],\"action_space_size\":%d,\"cost_unit\":\"minerals\"", count);
}

/*
Recent build history per base — `WorldView.history` in the contract.

Production is re-decided every turn, and several times per turn at the engine level. A brain
with no memory of its own last answer re-argues the case from nothing each time, so every
individual choice can be defensible while the sequence accumulates nothing: a base that
switches target every few turns finishes neither, and switching away from a partly built item
usually forfeits the progress.

A short ring buffer per base is the cheapest thing that makes a stable choice possible, and it
is nearly free — 512 bases by four entries, no allocation, no bookkeeping anywhere else.

Two details carry the weight:

**One entry per turn, last write wins.** mod_base_reset is hooked at eleven call sites and each
one applies its own answer, so a turn produces several choices for the same base and only the
last is what the base actually builds. Appending per call would fill the window with one turn of
churn and hide the very thing it exists to show.

**The tier travels with the item.** A decision re-reading its own earlier reasoning is in a
different position from one reading the deterministic tier's default: the first can ask whether
it still believes the argument, the second has no argument to weigh. Collapsing them would have
the brain defer to a choice nobody made.
*/
static const int NA_HISTORY = 4;

struct NaChoice {
    int turn;
    char item[48];
    const char* tier;
};

static NaChoice na_history[MaxBaseNum][NA_HISTORY];
static int na_history_len[MaxBaseNum];
static bool na_history_init = false;

static void na_record_choice(int base_id, const char* item, const char* tier) {
    if (base_id < 0 || base_id >= MaxBaseNum) {
        return;
    }
    if (!na_history_init) {
        for (int i = 0; i < MaxBaseNum; i++) {
            na_history_len[i] = 0;
        }
        na_history_init = true;
    }
    NaChoice entry;
    entry.turn = *CurrentTurn;
    entry.tier = tier;
    snprintf(entry.item, sizeof(entry.item), "%s", item ? item : "");

    int& len = na_history_len[base_id];
    if (len > 0 && na_history[base_id][len - 1].turn == entry.turn) {
        na_history[base_id][len - 1] = entry;   // same turn: replace, do not append
        return;
    }
    if (len == NA_HISTORY) {
        for (int i = 1; i < NA_HISTORY; i++) {
            na_history[base_id][i - 1] = na_history[base_id][i];
        }
        len--;
    }
    na_history[base_id][len++] = entry;
}

// Oldest first, which is the order the contract specifies and the order a reader expects.
// The CURRENT turn's entry is excluded: this decision is being made now, and showing the
// engine's provisional answer for this same turn as "history" would invite the brain to
// ratify a default it was asked to replace.
static void na_write_history(FILE* fp, int base_id) {
    if (base_id < 0 || base_id >= MaxBaseNum || !na_history_init) {
        return;
    }
    fputs(",\"history\":[", fp);
    int written = 0;
    for (int i = 0; i < na_history_len[base_id]; i++) {
        const NaChoice& c = na_history[base_id][i];
        if (c.turn >= *CurrentTurn) {
            continue;
        }
        if (written++) {
            fputs(",", fp);
        }
        fprintf(fp, "{\"turn\":%d,\"item\":\"", c.turn);
        na_write_escaped(fp, c.item);
        fprintf(fp, "\",\"tier\":\"%s\"}", c.tier);
    }
    fputs("]", fp);
}


/*
The measured economy, on every surface. This is `WorldView.metrics` in the contract.

A base deciding whether to spend 81 of 82 energy credits cannot judge that from the price
alone: the question is what else that energy buys and whether it comes back. Measured, this
was the whole gap on base.hurry - the surface split 6/4 across ten identical prompts until
the faction's standing plan and its economy were in front of it.

Three things about this block are load-bearing:

**The key is "metrics", not "faction_state".** The orchestrator reads measurements by name
from exactly one place (contract.py, WorldView.metrics) and directives.py evaluates every
standing plan against it. Shipping the same numbers under any other key leaves every
directive permanently UNMEASURABLE - which reads in a record as a plan being served rather
than as the plan never having been checked at all.

**The names are the orchestrator's vocabulary** (orchestrator/src/neural_amplifier/
metrics.py), not engine field names. A directive may only be written against a name the
world view actually reports, so a name we emit under the engine's spelling is a name no
directive can use.

**Every number lives here and nowhere else.** The engine blocks alongside this one carry
what the vocabulary has no name for. Two copies of one measurement is not a token cost, it
is a correctness one: base.hurry observes *after* mod_base_hurry() has run, so the faction's
live energy_credits is already the post-purchase figure while the hook's snapshot is the
pre-decision truth. Emitting both produced a record that disagreed with itself about what
the decision was made on.

That is what the two override parameters are for. Pass -1 to read the live engine field;
pass the hook's own snapshot where the engine has already moved on. `base_id` < 0 emits the
faction half only, for the faction-scope surfaces.
*/
static void na_write_metrics(FILE* fp, int faction_id, int base_id,
                             int energy_reserves_at_hook, int minerals_remaining_at_hook) {
    Faction& plr = Factions[faction_id];
    fputs(",\"metrics\":{", fp);

    fprintf(fp, "\"energy_reserves\":%d",
            energy_reserves_at_hook >= 0 ? energy_reserves_at_hook : plr.energy_credits);
    fprintf(fp, ",\"energy_income\":%d", plr.energy_surplus_total);
    fprintf(fp, ",\"labs_output\":%d", plr.labs_total);
    fprintf(fp, ",\"base_count\":%d", plr.base_count);
    fprintf(fp, ",\"pop_total\":%d", plr.pop_total);
    fprintf(fp, ",\"military_units\":%d", plr.total_combat_units);

    /*
    The one metric the engine does not maintain at faction level: drones and superdrones are
    per-base counters, so this is a sweep. A sweep is not the approximation this file
    otherwise refuses - it is the exact figure, and *BaseCount is small. What was refused,
    and still is, is inventing a number the engine does not have.
    */
    int drones = 0;
    for (int i = 0; i < *BaseCount && i < MaxBaseNum; i++) {
        BASE& b = Bases[i];
        if (b.faction_id == faction_id) {
            drones += b.drone_total + b.superdrone_total;
        }
    }
    fprintf(fp, ",\"drone_total\":%d", drones);

    if (base_id >= 0 && base_id < *BaseCount && base_id < MaxBaseNum) {
        BASE& b = Bases[base_id];
        fprintf(fp, ",\"pop_size\":%d", (int)b.pop_size);
        fprintf(fp, ",\"mineral_surplus\":%d", b.mineral_surplus);

        int remaining = minerals_remaining_at_hook;
        if (remaining < 0) {
            remaining = mineral_cost(base_id, b.item()) - b.minerals_accumulated;
        }
        fprintf(fp, ",\"minerals_remaining\":%d", remaining > 0 ? remaining : 0);
    }

    fputs("}", fp);
}


/*
The base's own state — category 1 of the input checklist.

What the vocabulary has a name for (pop_size, mineral_surplus, minerals_remaining) is in
`metrics` and deliberately not repeated here. This block is the remainder: the engine's own
reading of the base, for the model to read as prose.

Still no pre-computed "turns remaining". A partially built item makes that arithmetic subtly
wrong, and a number that is quietly wrong is worse than two numbers the brain can divide.
*/
static void na_write_base_state(FILE* fp, int base_id) {
    BASE& b = Bases[base_id];
    fputs(",\"base_state\":{", fp);
    fprintf(fp, "\"minerals_accumulated\":%d", b.minerals_accumulated);
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
    na_write_metrics(fp, faction_id, base_id, -1, -1);
    // Written BEFORE recording this turn's answer, so the block the brain reads is strictly
    // the past. Recording first would show it the engine's provisional pick for the very
    // turn it is being asked to decide.
    na_write_history(fp, base_id);
    na_write_action_space(fp, base_id);
    fputs(",\"tier\":\"deterministic\",\"applied\":\"native\"}\n", fp);
    na_record_choice(base_id, prod_name(native_choice), "deterministic");

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
The researchable-tech action space.

tech_avail is the engine's OWN availability test (tech.h:10) - deliberately not a
hand-rolled prerequisite check. base.production shipped with can_build_unit, which looks
like an availability test and is not, and offered 125 impossible options as a result. Use
what the engine uses.

CTech carries the AI's own valuation weights (AI_growth / AI_tech / AI_wealth / AI_power).
Those are included on purpose: they are what the deterministic tier would use, so exposing
them lets the brain see the native reasoning it is being asked to improve on, and lets a
reviewer tell a considered disagreement from a coin flip.
*/
static void na_write_tech_action_space(FILE* fp, int faction_id) {
    int count = 0;
    fputs(",\"action_space\":[", fp);
    for (int id = 0; id < MaxTechnologyNum; id++) {
        if (!tech_avail(id, faction_id)) {
            continue;
        }
        if (count++) { fputs(",", fp); }
        fprintf(fp, "{\"id\":\"tech:%d\",\"name\":\"", id);
        na_write_escaped(fp, Tech[id].name);
        fputs("\",\"category\":\"tech\"", fp);
        fprintf(fp, ",\"ai_weights\":{\"growth\":%d,\"tech\":%d,\"wealth\":%d,\"power\":%d}",
                Tech[id].AI_growth, Tech[id].AI_tech, Tech[id].AI_wealth, Tech[id].AI_power);
        fputs("}", fp);
    }
    fprintf(fp, "],\"action_space_size\":%d", count);
}

void na_observe_faction_tech(int faction_id, int native_choice) {
    if (faction_id <= 0 || faction_id >= MaxPlayerNum) {
        return;
    }
    FILE* fp = na_log_open();
    if (!fp) {
        return;
    }
    Faction& plr = Factions[faction_id];

    fprintf(fp, "{\"surface_id\":\"faction.tech\",\"engine\":\"thinker\"");
    fprintf(fp, ",\"turn\":%d", *CurrentTurn);
    fprintf(fp, ",\"faction_id\":%d", faction_id);
    fputs(",\"faction\":\"", fp);
    na_write_escaped(fp, MFactions[faction_id].noun_faction);
    fputs("\"", fp);

    // Faction research and economic state - categories 1 and 4 of the input checklist.
    na_write_metrics(fp, faction_id, -1, -1, -1);
    fprintf(fp, ",\"tech_accumulated\":%d", plr.tech_accumulated);
    fprintf(fp, ",\"tech_rate\":%d", mod_tech_rate(faction_id));

    fprintf(fp, ",\"native_choice\":%d", native_choice);
    fputs(",\"native_choice_name\":\"", fp);
    if (native_choice >= 0 && native_choice < MaxTechnologyNum) {
        na_write_escaped(fp, Tech[native_choice].name);
    }
    fputs("\"", fp);

    na_write_tech_action_space(fp, faction_id);
    fputs(",\"tier\":\"deterministic\",\"applied\":\"native\"}\n", fp);
    fflush(fp);
}

/*
Social engineering. field/model describe the change the deterministic tier settled on, or
field < 0 for "no change this turn" — which is a real decision and is recorded as one
rather than as an absence.
*/
void na_observe_faction_se(int faction_id, int field, int model, int cost) {
    if (faction_id <= 0 || faction_id >= MaxPlayerNum) {
        return;
    }
    FILE* fp = na_log_open();
    if (!fp) {
        return;
    }
    Faction& plr = Factions[faction_id];
    const int* current = &plr.SE_Politics;

    fprintf(fp, "{\"surface_id\":\"faction.se\",\"engine\":\"thinker\"");
    fprintf(fp, ",\"turn\":%d", *CurrentTurn);
    fprintf(fp, ",\"faction_id\":%d", faction_id);
    fputs(",\"faction\":\"", fp);
    na_write_escaped(fp, MFactions[faction_id].noun_faction);
    fputs("\"", fp);

    // Current settings, by name rather than index — an index means nothing in a prompt.
    fputs(",\"current\":{", fp);
    for (int f = 0; f < MaxSocialCatNum; f++) {
        int m = current[f];
        if (f) { fputs(",", fp); }
        fputs("\"", fp);
        na_write_escaped(fp, SocialField[f].field_name);
        fputs("\":\"", fp);
        if (m >= 0 && m < MaxSocialModelNum) {
            na_write_escaped(fp, SocialField[f].soc_name[m]);
        }
        fputs("\"", fp);
    }
    fputs("}", fp);

    na_write_metrics(fp, faction_id, -1, -1, -1);

    /*
    The action space: every (field, model) the faction may legally adopt.

    Excluded, not flagged: models whose prerequisite tech is unresearched, and the model
    already in force for that field. Faction-prohibited models are excluded too — factions
    are forbidden certain values, and that is a rule the action space must enforce because
    grounding only advises.
    */
    int count = 0;
    fputs(",\"action_space\":[{\"id\":\"se:none\",\"name\":\"No change\",\"category\":\"se\"}", fp);
    count++;
    for (int f = 0; f < MaxSocialCatNum; f++) {
        for (int m = 0; m < MaxSocialModelNum; m++) {
            if (m == current[f]) {
                continue;
            }
            int preq = SocialField[f].soc_preq_tech[m];
            if (preq >= 0 && !has_tech(preq, faction_id)) {
                continue;
            }
            // Factions are forbidden specific values; the action space must enforce that,
            // because grounding advises and only the action space binds.
            if (MFactions[faction_id].soc_opposition_category == f
            && MFactions[faction_id].soc_opposition_model == m) {
                continue;
            }
            count++;
            /*
            "name" is the MODEL name alone — "Police State", not "Politics -> Police State".

            It has to match the label in the datalinks graph, because retrieval looks facts up by
            action name. The composed form read better in a log and silently matched nothing, so
            this surface had no grounding in the real pipeline even though hand-feeding the model
            name proved the facts were there. The stability harness caught it; a per-decision
            record could not, because "no facts retrieved" looks identical to "nothing to retrieve".

            The category travels as its own field, which is where a consumer that wants the
            composed form should build it from.
            */
            fprintf(fp, ",{\"id\":\"se:%d:%d\",\"name\":\"", f, m);
            na_write_escaped(fp, SocialField[f].soc_name[m]);
            fputs("\",\"field\":\"", fp);
            na_write_escaped(fp, SocialField[f].field_name);
            fputs("\",\"category\":\"se\",\"effects\":{", fp);
            /*
            The effect deltas, without which this is not a decidable comparison — the same
            mistake as shipping build options without cost. "Democratic" tells the brain
            nothing; "+2 efficiency, +2 growth, -1 support" is the actual choice.

            Only non-zero entries are emitted: eleven fields per model, mostly zero, and a
            wall of zeros costs tokens while hiding the two numbers that matter.
            */
            static const char* eff_names[MaxSocialEffectNum] = {
                "economy", "efficiency", "support", "talent", "morale", "police",
                "growth", "planet", "probe", "industry", "research"
            };
            int shown = 0;
            for (int e = 0; e < MaxSocialEffectNum; e++) {
                int v = SocialField[f].soc_effect[m].values[e];
                if (!v) {
                    continue;
                }
                if (shown++) { fputs(",", fp); }
                fprintf(fp, "\"%s\":%d", eff_names[e], v);
            }
            fputs("}", fp);

            /*
            Per-option upheaval cost, and affordability derived from it.

            Without this the action space was not decidable: every option looked free, so the only
            thing distinguishing "adopt Free Market" from "adopt Green" was the effect deltas, and
            a faction with 60 credits could be told to buy a 200-credit change. The engine computes
            the cost from the WHOLE proposed category set, not from the single field being changed,
            so it has to be asked once per candidate rather than derived from a table.

            Built the same way mod_social_ai builds its own candidate (faction.cpp:1348): copy the
            current category block, then overwrite the one field.

            Computed here rather than left to the brain, on the same reasoning as the production
            turn estimates: a model that misreads a state value still decides correctly when the
            derived number is handed to it, and reasons confidently from a fabricated one when it
            is not.
            */
            CSocialCategory candidate;
            memcpy(&candidate, &plr.SE_Politics, sizeof(candidate));
            candidate.models[f] = m;
            int upheaval = social_upheaval(faction_id, &candidate);
            fprintf(fp, ",\"cost\":%d,\"cost_unit\":\"credits\"", upheaval);
            fprintf(fp, ",\"affordable\":%s",
                    upheaval <= plr.energy_credits ? "true" : "false");
            fputs("}", fp);
        }
    }
    fprintf(fp, "],\"action_space_size\":%d", count);

    if (field >= 0 && field < MaxSocialCatNum && model >= 0 && model < MaxSocialModelNum) {
        fprintf(fp, ",\"native_choice\":\"se:%d:%d\"", field, model);
        fputs(",\"native_choice_name\":\"", fp);
        na_write_escaped(fp, SocialField[field].field_name);
        fputs(" -> ", fp);
        na_write_escaped(fp, SocialField[field].soc_name[model]);
        fprintf(fp, "\",\"upheaval_cost\":%d", cost);
    } else {
        fputs(",\"native_choice\":\"se:none\",\"native_choice_name\":\"No change\"", fp);
        fputs(",\"upheaval_cost\":0", fp);
    }

    fputs(",\"tier\":\"deterministic\",\"applied\":\"native\"}\n", fp);
    fflush(fp);
}

/*
Hurry production: spend energy credits to finish the current item immediately.

The action space is two options, so the whole decision quality lives in the numbers attached to
them. Reports the cost in credits, what is left to build, the reserve available, and how long the
item takes if left alone — because "hurry for 84 credits" is only decidable against "or wait three
turns" and "we hold 139".

Cost is recomputed from the PRE-decision minerals, passed in by the caller. Recomputing after the
fact would be wrong whenever the item was actually hurried: hurry_item() moves
minerals_accumulated, so the cost of a purchase already made would come back as zero.
*/
void na_observe_base_hurry(int base_id, int item, int minerals_before, int credits_before,
                           int native_hurried) {
    if (base_id < 0 || base_id >= *BaseCount || base_id >= MaxBaseNum) {
        return;
    }
    FILE* fp = na_log_open();
    if (!fp) {
        return;
    }
    BASE& base = Bases[base_id];
    const int faction_id = base.faction_id;

    const int total = mineral_cost(base_id, item);
    const int remaining = total - minerals_before;
    const int cost = hurry_cost(base_id, item, remaining > 0 ? remaining : 0);
    const int surplus = base.mineral_surplus;
    const int turns_if_waiting =
        surplus > 0 && remaining > 0 ? (remaining + surplus - 1) / surplus : 0;

    fprintf(fp, "{\"surface_id\":\"base.hurry\",\"engine\":\"thinker\"");
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

    fputs(",\"item\":\"", fp);
    na_write_escaped(fp, prod_name(item));
    fputs("\"", fp);
    fputs(",\"base_state\":{", fp);
    fprintf(fp, "\"minerals_accumulated\":%d", minerals_before);
    fprintf(fp, ",\"mineral_cost_total\":%d", total);
    fprintf(fp, ",\"turns_if_waiting\":%d", turns_if_waiting);
    fputs("}", fp);
    /*
    Snapshots, not live fields: this hook runs after mod_base_hurry(), so the faction's
    energy_credits and the base's minerals_accumulated may already reflect a purchase the
    brain is being asked to decide on.
    */
    na_write_metrics(fp, faction_id, base_id, credits_before, remaining > 0 ? remaining : 0);

    /*
    Affordability is part of the action space, not advice. An option the faction cannot pay for
    should not be offered: the engine would refuse it, and offering it invites the brain to
    "decide" something that was never available.
    */
    const bool affordable = base.can_hurry_item() && cost > 0 && cost <= credits_before;
    fputs(",\"action_space\":[{\"id\":\"hurry:none\",\"name\":\"Do not hurry\"", fp);
    fputs(",\"cost\":0,\"cost_unit\":\"credits\"}", fp);
    if (affordable) {
        fprintf(fp, ",{\"id\":\"hurry:now\",\"name\":\"Hurry production\",\"cost\":%d", cost);
        fprintf(fp, ",\"cost_unit\":\"credits\",\"saves_turns\":%d}", turns_if_waiting);
    }
    fprintf(fp, "],\"action_space_size\":%d", affordable ? 2 : 1);

    fprintf(fp, ",\"native_choice\":\"%s\"", native_hurried ? "hurry:now" : "hurry:none");
    fputs(",\"tier\":\"deterministic\",\"applied\":\"native\"}\n", fp);
    fflush(fp);
}

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

    /*
    "observe-tech <faction_id>" — the faction.tech probe.

    Same contract as "observe": serialiser only, no decision, no side effects. Needed
    because tech selection fires once every five to ten turns, so waiting for it naturally
    is not a test loop. Passes the faction's CURRENT research target as native_choice, which
    is what the deterministic tier last picked.
    */
    if (strncmp(line, "observe-tech ", 13) == 0) {
        int fid = -1;
        if (sscanf(line + 13, "%d", &fid) == 1 && fid > 0 && fid < MaxPlayerNum) {
            na_observe_faction_tech(fid, Factions[fid].tech_research_id);
            char detail[96];
            snprintf(detail, sizeof(detail), "faction_id=%d", fid);
            na_cmd_result("observe-tech", detail, true);
        } else {
            na_cmd_result("observe-tech", "need 0 < faction_id < 8", false);
        }
        return;
    }

    // "observe-se <faction_id>" — the faction.se probe. Serialiser only; reports the
    // current settings and a "no change" native choice, since no decision is being made.
    if (strncmp(line, "observe-se ", 11) == 0) {
        int fid = -1;
        if (sscanf(line + 11, "%d", &fid) == 1 && fid > 0 && fid < MaxPlayerNum) {
            na_observe_faction_se(fid, -1, -1, 0);
            char detail[96];
            snprintf(detail, sizeof(detail), "faction_id=%d", fid);
            na_cmd_result("observe-se", detail, true);
        } else {
            na_cmd_result("observe-se", "need 0 < faction_id < 8", false);
        }
        return;
    }

    /*
    "apply <base_id> <action_id>" — set a base's production to a chosen action.

    This is the act half of the loop, and it is what lets an agent outside the process be
    the brain: observe, decide, apply. No orchestrator and no transport required.

    action_id uses the same encoding the observation emits, "unit:<proto>" or
    "facility:<id>", so a decision can be copied straight back from a world view without
    translation.

    The choice is VALIDATED against the engine's own availability tests before being
    applied, not merely parsed. That is the same guarantee the contract makes: an order can
    only name something the engine actually offered, so an illegal order is impossible
    rather than unlikely. A rejected action leaves production untouched and says why.
    */
    if (strncmp(line, "apply ", 6) == 0) {
        int base_id = -1;
        char kind[32] = {};
        int id = -1;
        if (sscanf(line + 6, "%d %31[^:]:%d", &base_id, kind, &id) != 3
        || base_id < 0 || base_id >= *BaseCount) {
            na_cmd_result("apply", "expected: apply <base_id> unit:<n>|facility:<n>", false);
            return;
        }
        const int fid = Bases[base_id].faction_id;
        int item = 99999;
        if (strcmp(kind, "unit") == 0) {
            if (id < 0 || id >= MaxProtoNum
            || !mod_veh_avail(id, fid, base_id) || !can_build_unit(base_id, id)) {
                na_cmd_result("apply", "unit not available for this base", false);
                return;
            }
            item = id;              // item >= 0 encodes a unit prototype
        } else if (strcmp(kind, "facility") == 0) {
            if (id < 1 || id > SP_ID_Last
            || !mod_facility_avail((FacilityId)id, fid, base_id, 0) || !can_build(base_id, id)) {
                na_cmd_result("apply", "facility not available for this base", false);
                return;
            }
            item = -id;             // item < 0 encodes a facility
        } else {
            na_cmd_result("apply", "kind must be unit or facility", false);
            return;
        }

        mod_base_change(base_id, item);
        char detail[160];
        snprintf(detail, sizeof(detail), "base=%d item=%d now=%s",
                 base_id, item, prod_name(Bases[base_id].item()));
        na_cmd_result("apply", detail, true);

        // Record the applied decision so the log shows the LLM tier acting, not just watching.
        FILE* lf = na_log_open();
        if (lf) {
            fprintf(lf, "{\"surface_id\":\"base.production\",\"engine\":\"thinker\"");
            fprintf(lf, ",\"turn\":%d,\"base_id\":%d", *CurrentTurn, base_id);
            fputs(",\"base\":\"", lf);
            na_write_escaped(lf, Bases[base_id].name);
            fprintf(lf, "\",\"chosen\":\"%s:%d\"", kind, id);
            fputs(",\"chosen_name\":\"", lf);
            na_write_escaped(lf, prod_name(item));
            fputs("\",\"tier\":\"llm\",\"applied\":\"llm\"}\n", lf);
            fflush(lf);
        }
        // Supersedes any deterministic entry logged for this turn — same-turn writes replace,
        // and this is the choice the base actually ends up building.
        na_record_choice(base_id, prod_name(item), "llm");
        return;
    }

    /*
    "apply-tech <faction_id> <tech_id>" — set the faction's research target.

    Validated with tech_avail, which is the same test the observation's action space is built
    from, so an id the brain was not offered cannot be applied.

    **Switching mid-research is allowed and is not free.** The engine keeps tech_accumulated
    against the faction, not against the target, so changing target does not zero it here — but
    a brain that re-targets every few turns still finishes nothing, which is the same trap
    base.production has and the reason WorldView.history exists. The response reports the
    accumulated points so a caller can see what is in flight rather than having to assume.
    */
    if (strncmp(line, "apply-tech ", 11) == 0) {
        int fid = -1;
        int tech_id = -1;
        if (sscanf(line + 11, "%d tech:%d", &fid, &tech_id) != 2
        || fid <= 0 || fid >= MaxPlayerNum) {
            na_cmd_result("apply-tech", "expected: apply-tech <faction_id> tech:<n>", false);
            return;
        }
        if (tech_id < 0 || tech_id >= MaxTechnologyNum || !tech_avail(tech_id, fid)) {
            na_cmd_result("apply-tech", "technology not available to this faction", false);
            return;
        }
        Faction& plr = Factions[fid];
        const int previous = plr.tech_research_id;
        plr.tech_research_id = tech_id;

        char detail[192];
        snprintf(detail, sizeof(detail), "faction=%d tech=%d now=%s accumulated=%d was=%s",
                 fid, tech_id, Tech[tech_id].name, plr.tech_accumulated,
                 previous >= 0 && previous < MaxTechnologyNum ? Tech[previous].name : "none");
        na_cmd_result("apply-tech", detail, true);

        FILE* lf = na_log_open();
        if (lf) {
            fprintf(lf, "{\"surface_id\":\"faction.tech\",\"engine\":\"thinker\"");
            fprintf(lf, ",\"turn\":%d,\"faction_id\":%d", *CurrentTurn, fid);
            fputs(",\"faction\":\"", lf);
            na_write_escaped(lf, MFactions[fid].noun_faction);
            fprintf(lf, "\",\"chosen\":\"tech:%d\"", tech_id);
            fputs(",\"chosen_name\":\"", lf);
            na_write_escaped(lf, Tech[tech_id].name);
            fprintf(lf, "\",\"tech_accumulated\":%d", plr.tech_accumulated);
            fputs(",\"tier\":\"llm\",\"applied\":\"llm\"}\n", lf);
            fflush(lf);
        }
        return;
    }

    /*
    "apply-se <faction_id> <field> <model>" — change one social-engineering value.

    Legality comes from society_avail, the engine's own test, NOT from the checks the
    observation's action space happens to make. Those two are supposed to agree and this is the
    one that binds: an order can only do what the engine would have allowed.

    The upheaval cost is charged exactly as faction.cpp does it — compute against the proposed
    category, refuse if the faction cannot afford it, then debit and commit through social_set.
    Reimplementing the debit would be how a faction ends up with free social change.

    "se:none" is a legal answer and means keep the current values. It is recorded rather than
    ignored, because a decision to hold is still a decision and coverage counts what fired.
    */
    if (strncmp(line, "apply-se ", 9) == 0) {
        int fid = -1;
        int field = -1;
        int model = -1;
        const bool hold = (sscanf(line + 9, "%d se:none", &fid) == 1);
        if (!hold && (sscanf(line + 9, "%d se:%d:%d", &fid, &field, &model) != 3)) {
            na_cmd_result("apply-se",
                          "expected: apply-se <faction_id> se:<field>:<model> | se:none", false);
            return;
        }
        if (fid <= 0 || fid >= MaxPlayerNum) {
            na_cmd_result("apply-se", "need 0 < faction_id < 8", false);
            return;
        }
        Faction& plr = Factions[fid];

        if (hold) {
            na_cmd_result("apply-se", "no change", true);
            FILE* lf = na_log_open();
            if (lf) {
                fprintf(lf, "{\"surface_id\":\"faction.se\",\"engine\":\"thinker\"");
                fprintf(lf, ",\"turn\":%d,\"faction_id\":%d", *CurrentTurn, fid);
                fputs(",\"chosen\":\"se:none\",\"chosen_name\":\"No change\"", lf);
                fputs(",\"tier\":\"llm\",\"applied\":\"llm\"}\n", lf);
                fflush(lf);
            }
            return;
        }

        if (field < 0 || field >= MaxSocialCatNum || model < 0 || model >= MaxSocialModelNum
        || !society_avail(field, model, fid)) {
            na_cmd_result("apply-se", "social model not available to this faction", false);
            return;
        }

        CSocialCategory soc;
        memcpy(&soc, &plr.SE_Politics, sizeof(soc));
        soc.models[field] = model;
        const int cost = social_upheaval(fid, &soc);
        if (plr.energy_credits <= cost) {
            char detail[128];
            snprintf(detail, sizeof(detail), "upheaval costs %d, reserves %d",
                     cost, plr.energy_credits);
            na_cmd_result("apply-se", detail, false);
            return;
        }

        auto pending = (CSocialCategory*)&plr.SE_Politics_pending;
        pending->models[field] = model;
        plr.energy_credits -= cost;
        plr.SE_upheaval_cost_paid += cost;
        social_set(fid);

        char detail[192];
        snprintf(detail, sizeof(detail), "faction=%d %s -> %s cost=%d reserves=%d",
                 fid, SocialField[field].field_name, SocialField[field].soc_name[model],
                 cost, plr.energy_credits);
        na_cmd_result("apply-se", detail, true);

        FILE* lf = na_log_open();
        if (lf) {
            fprintf(lf, "{\"surface_id\":\"faction.se\",\"engine\":\"thinker\"");
            fprintf(lf, ",\"turn\":%d,\"faction_id\":%d", *CurrentTurn, fid);
            fputs(",\"faction\":\"", lf);
            na_write_escaped(lf, MFactions[fid].noun_faction);
            fprintf(lf, "\",\"chosen\":\"se:%d:%d\"", field, model);
            fputs(",\"chosen_name\":\"", lf);
            na_write_escaped(lf, SocialField[field].soc_name[model]);
            fprintf(lf, "\",\"upheaval_cost\":%d", cost);
            fputs(",\"tier\":\"llm\",\"applied\":\"llm\"}\n", lf);
            fflush(lf);
        }
        return;
    }

    /*
    "apply-hurry <base_id> <hurry|none>" — spend energy credits to finish production early.

    The first apply path that can lose something irreversibly, so it is the strictest. Three
    checks, all the engine's own: can_hurry_item (the engine's rule about what may be rushed),
    a positive hurry_cost, and enough reserves to pay it. An unaffordable order is refused with
    the numbers rather than partially applied.

    hurry_item does the debit and the mineral credit together; doing either by hand would be
    how a faction gets free production.
    */
    if (strncmp(line, "apply-hurry ", 12) == 0) {
        int base_id = -1;
        char what[16] = {};
        if (sscanf(line + 12, "%d hurry:%15s", &base_id, what) != 2
        || base_id < 0 || base_id >= *BaseCount || base_id >= MaxBaseNum) {
            na_cmd_result("apply-hurry",
                          "expected: apply-hurry <base_id> hurry:now|hurry:none", false);
            return;
        }
        BASE& base = Bases[base_id];
        Faction& plr = Factions[base.faction_id];

        if (strcmp(what, "none") == 0) {
            na_cmd_result("apply-hurry", "did not hurry", true);
            FILE* lf = na_log_open();
            if (lf) {
                fprintf(lf, "{\"surface_id\":\"base.hurry\",\"engine\":\"thinker\"");
                fprintf(lf, ",\"turn\":%d,\"base_id\":%d", *CurrentTurn, base_id);
                fputs(",\"chosen\":\"hurry:none\",\"chosen_name\":\"Do not hurry\"", lf);
                fputs(",\"tier\":\"llm\",\"applied\":\"llm\"}\n", lf);
                fflush(lf);
            }
            return;
        }
        if (strcmp(what, "now") != 0) {
            na_cmd_result("apply-hurry", "expected hurry:now or hurry:none", false);
            return;
        }

        const int item = base.item();
        const int total = mineral_cost(base_id, item);
        int remaining = total - base.minerals_accumulated;
        if (remaining < 0) { remaining = 0; }
        const int cost = hurry_cost(base_id, item, remaining);

        if (!base.can_hurry_item() || cost <= 0 || cost > plr.energy_credits) {
            char detail[160];
            snprintf(detail, sizeof(detail), "cannot hurry: cost=%d reserves=%d remaining=%d",
                     cost, plr.energy_credits, remaining);
            na_cmd_result("apply-hurry", detail, false);
            return;
        }

        const int credits_before = plr.energy_credits;
        hurry_item(base_id, remaining, cost);

        char detail[192];
        snprintf(detail, sizeof(detail), "base=%d item=%s cost=%d reserves %d -> %d",
                 base_id, prod_name(item), cost, credits_before, plr.energy_credits);
        na_cmd_result("apply-hurry", detail, true);

        FILE* lf = na_log_open();
        if (lf) {
            fprintf(lf, "{\"surface_id\":\"base.hurry\",\"engine\":\"thinker\"");
            fprintf(lf, ",\"turn\":%d,\"base_id\":%d", *CurrentTurn, base_id);
            fputs(",\"base\":\"", lf);
            na_write_escaped(lf, base.name);
            fputs("\",\"chosen\":\"hurry:now\",\"chosen_name\":\"", lf);
            na_write_escaped(lf, prod_name(item));
            fprintf(lf, "\",\"credits_spent\":%d,\"energy_reserves\":%d",
                    cost, plr.energy_credits);
            fputs(",\"tier\":\"llm\",\"applied\":\"llm\"}\n", lf);
            fflush(lf);
        }
        return;
    }

    /*
    "observe-hurry <base_id>" — the base.hurry probe.

    Serialiser only: reports the current numbers with native_hurried = 0, so it never spends
    credits. Reads live minerals and reserves, which for a probe is exactly right — the point is
    to see what the brain would see right now.
    */
    if (strncmp(line, "observe-hurry ", 14) == 0) {
        int base_id = -1;
        if (sscanf(line + 14, "%d", &base_id) == 1
        && base_id >= 0 && base_id < *BaseCount) {
            BASE& b = Bases[base_id];
            na_observe_base_hurry(base_id, b.item(), b.minerals_accumulated,
                                  Factions[b.faction_id].energy_credits, 0);
            char detail[96];
            snprintf(detail, sizeof(detail), "base_id=%d of %d", base_id, *BaseCount);
            na_cmd_result("observe-hurry", detail, true);
        } else {
            char detail[96];
            snprintf(detail, sizeof(detail), "need 0 <= base_id < %d", *BaseCount);
            na_cmd_result("observe-hurry", detail, false);
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
