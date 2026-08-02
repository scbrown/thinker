
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
Commit one finished record to the log.

Records are built in memory rather than printed straight to the file because A1
needs the same bytes as an HTTP body, and because the last two fields (`tier`,
`applied`) are only known after the decision returns — so the line cannot be
streamed out as it is composed.

A record that failed to build is dropped rather than written. A half-built world
view is not a smaller world view: it is an unparseable line that the orchestrator
silently skips, which shows up weeks later as unexplained low coverage.
*/
/*
Escape straight into a FILE*, for the log lines that are not world views.

The autoload trace and the command channel write small fixed records and have no
reason to allocate a buffer. Routed through na_buf_escaped rather than repeating
the escape table, because two copies of that table is exactly how one of them
ends up missing a case.
*/
static void na_write_escaped(FILE* fp, const char* s) {
    NaBuf tmp;
    na_buf_init(&tmp);
    na_buf_escaped(&tmp, s);
    if (!tmp.failed && tmp.data) {
        fputs(tmp.data, fp);
    }
    na_buf_free(&tmp);
}

static void na_log_record(NaBuf* w) {
    if (w->failed || !w->data) {
        return;
    }
    FILE* fp = na_log_open();
    if (!fp) {
        return;
    }
    fputs(w->data, fp);
    fputc('\n', fp);
    // Flushed per line: a crash mid-turn is exactly when we most want the log,
    // and terranx.exe crashing is not hypothetical.
    fflush(fp);
}

/*
The contract preamble every world view starts with.

`schema_version`, `engine`, `scope`, `turn` and `faction` are the contract's five
required fields (contract.py: WorldView); `surface_id` is invariant 5, the coverage
key, and must match the frozen registry in surfaces.py.

`trace.traceparent` makes the adapter the trace root, which is the right shape:
the game is the root of the causality, and the orchestrator continues the trace
rather than starting one (docs/observability.md §4). The DLL's entire telemetry
job is this field — export stays in the orchestrator because the DLL must not
block the game's message pump on a network write.

The trace id is built from turn, faction and a per-record counter rather than from
a random source. It has to be unique within a run and reproducible from the log
line, and rand() in a game DLL shares the engine's seed — drawing from it would
perturb map generation and combat, which is a genuinely terrible way to acquire
a trace id.
*/
static unsigned na_trace_seq = 0;

/*
Fixed per process, varying between runs. Wall-clock at first use: this separates two runs of
the same save, which the turn/faction/counter triple alone cannot — replay the same save and
every trace id repeats, so two runs collate into one trace and the spans interleave.

Deliberately not drawn from the engine's RNG, which is the *game's* RNG: this is a correlation
id, not a secret, and perturbing map generation to obtain one would be a bad trade.
*/
static unsigned int na_session_salt() {
    static unsigned int salt = 0;
    if (salt == 0) {
        salt = (unsigned int)time(NULL);
    }
    return salt;
}

/*
What the engine knows about the asymmetry this decision was made under.

**fairness** stamps only the two inputs the engine actually has: which slot this faction is,
and the difficulty. The ledger itself is derived in the orchestrator (fairness.py) because
three of its entries change which side they favour as difficulty moves and two are inert under
the fork's shipped defaults — a static list hardcoded here would declare handicaps that are not
in force and mislabel ones that are. Reporting the inputs and deriving the rules in one place
is the division that keeps the ledger honest.
*/
static void na_write_fairness(NaBuf* w, int faction_id) {
    static const char* levels[] = {
        "citizen", "specialist", "talent", "librarian", "thinker", "transcend"
    };
    const int level = *DiffLevel;
    na_buf_puts(w, ",\"fairness\":{\"slot\":\"");
    na_buf_puts(w, is_human(faction_id) ? "human" : "ai");
    na_buf_puts(w, "\",\"difficulty\":\"");
    na_buf_puts(w, level >= 0 && level < 6 ? levels[level] : "unknown");
    // handicaps is deliberately EMPTY, not absent: the orchestrator derives the ledger from
    // these two inputs. An adapter that knows asymmetries the ledger does not model may stamp
    // its own entries instead, and fairness.drift checks those rather than replacing them.
    na_buf_puts(w, "\",\"handicaps\":[]}");
}

static void na_write_head(NaBuf* w, const char* surface_id, const char* scope, int faction_id) {
    na_buf_printf(w, "{\"schema_version\":\"0.1\",\"engine\":\"thinker\"");
    na_buf_printf(w, ",\"scope\":\"%s\",\"surface_id\":\"%s\"", scope, surface_id);
    na_buf_printf(w, ",\"turn\":%d", *CurrentTurn);
    na_buf_printf(w, ",\"faction_id\":%d", faction_id);
    na_buf_puts(w, ",\"faction\":\"");
    if (faction_id > 0 && faction_id < MaxPlayerNum) {
        na_buf_escaped(w, MFactions[faction_id].noun_faction);
    }
    na_buf_puts(w, "\"");
    // W3C traceparent: version-traceid(16 bytes)-spanid(8 bytes)-flags, sampled.
    // The trailing constant keeps the trace id non-zero on turn 0, which the spec
    // forbids and collectors drop. The salt is folded into the turn word rather than
    // replacing that constant, so the non-zero guarantee does not come to depend on
    // what the clock happened to read.
    na_trace_seq++;
    na_buf_printf(w,
        ",\"trace\":{\"traceparent\":\"00-%08x%08x%08x%08x-%08x%08x-01\"}",
        (unsigned)*CurrentTurn ^ na_session_salt(), (unsigned)faction_id,
        na_trace_seq, 0x7a1c0de,
        na_trace_seq, (unsigned)(*CurrentTurn + 1));
    na_write_fairness(w, faction_id);
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

/*
What choosing this item immediately costs, in the metric vocabulary.

The contract computes a directive trade-off from `Action.effects` and nothing
else (contract.py: Tradeoff), so an effect that is not declared here cannot be
weighed against a standing plan — it just silently does not appear. Deriving it
downstream from `cost` plus `cost_unit` was the previous arrangement and it put
this adapter's field names inside the orchestrator, which invariant 2 forbids.

Only the immediate, known cost. What the item goes on to be worth is a prediction,
and predictions do not belong in a field the orchestrator does arithmetic on.
*/
static void na_write_effects(NaBuf* w, int minerals) {
    if (minerals <= 0) {
        return;
    }
    na_buf_printf(w, ",\"effects\":{\"minerals_remaining\":%d}", -minerals);
}

// Emit the two turn estimates for one candidate item. surplus <= 0 means never, which is
// real: a base with no mineral surplus genuinely cannot finish anything.
static void na_write_turns(NaBuf* w, int cost, int surplus, int banked, bool is_current) {
    if (surplus <= 0) {
        na_buf_puts(w, ",\"turns_if_switched\":null");
        return;
    }
    int t_switch = (cost + surplus - 1) / surplus;
    na_buf_printf(w, ",\"turns_if_switched\":%d", t_switch);
    if (is_current) {
        int left = cost - banked;
        if (left < 0) { left = 0; }
        na_buf_printf(w, ",\"turns_if_continued\":%d", (left + surplus - 1) / surplus);
    }
}

static void na_write_action_space(NaBuf* w, int base_id) {
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

    na_buf_puts(w, ",\"action_space\":[");

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
        if (count++) { na_buf_puts(w, ","); }
        na_buf_printf(w, "{\"id\":\"unit:%d\",\"action\":\"", id);
        na_buf_escaped(w, Units[id].name);
        int rows = mod_veh_cost(id, base_id, NULL);
        na_buf_printf(w, "\",\"cost\":%d,\"category\":\"unit\"", rows * mineral_factor);
        na_write_effects(w, rows * mineral_factor);
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
        na_buf_puts(w, ",\"role\":\"");
        if (plan >= 0 && plan < (int)(sizeof(plan_role)/sizeof(plan_role[0]))) {
            na_buf_escaped(w, plan_role[plan]);
        }
        int tri = Units[id].triad();
        na_buf_printf(w, "\",\"triad\":\"%s\"",
                tri == TRIAD_SEA ? "sea" : (tri == TRIAD_AIR ? "air" : "land"));
        na_write_turns(w, rows * mineral_factor, surplus, banked, current_item == id);
        na_buf_puts(w, "}");
    }

    for (int id = 1; id <= SP_ID_Last; id++) {
        // Same reasoning: mod_facility_avail is the engine's own availability test.
        if (!mod_facility_avail((FacilityId)id, faction_id, base_id, 0)) {
            continue;
        }
        if (!can_build(base_id, id)) {
            continue;
        }
        if (count++) { na_buf_puts(w, ","); }
        na_buf_printf(w, "{\"id\":\"facility:%d\",\"action\":\"", id);
        na_buf_escaped(w, Facility[id].name);
        na_buf_puts(w, "\",\"effect\":\"");
        na_buf_escaped(w, Facility[id].effect);
        na_buf_printf(w, "\",\"cost\":%d,\"maint\":%d,\"category\":\"%s\"",
            Facility[id].cost * mineral_factor, Facility[id].maint,
            id >= SP_ID_First ? "project" : "facility");
        na_write_effects(w, Facility[id].cost * mineral_factor);
        na_write_turns(w, Facility[id].cost * mineral_factor, surplus, banked,
                       current_item == -id);
        na_buf_puts(w, "}");
    }

    na_buf_printf(w, "],\"action_space_size\":%d,\"cost_unit\":\"minerals\"", count);
}

/*
Faction-level economy, on every surface.

A base deciding whether to spend 81 of 82 energy credits cannot judge that from the price
alone: the question is what else that energy buys and whether it comes back. Measured, this
was the whole gap on base.hurry - the surface split 6/4 across ten identical prompts until
the faction's standing plan and its economy were in front of it.

These names are the orchestrator's metric vocabulary (orchestrator/src/neural_amplifier/
metrics.py), not engine field names, because a directive may only be written against a name
the world view actually reports. Shipping them under the engine's own names would leave every
faction-scope directive permanently unmeasurable - which reads in a record as compliance
rather than as a gap.

What is excluded is *approximation*, not aggregation. A metric that is quietly wrong is worse
than one that is absent, because absent reads as "cannot be checked" and wrong reads as fact —
so nothing here is estimated. Summing a field the engine maintains per base is not an estimate,
it is arithmetic, and drone_total is exactly that: the engine keeps no faction-level count, so
the only honest way to report a name the vocabulary already contains is to add the bases up.

Emitted under the contract's `metrics` key, which is the one place the orchestrator reads
numbers by name (contract.py: WorldView.metrics). It used to be `faction_state`, which meant
the values reached the prompt but not the directive evaluator — every faction-scope directive
came back UNMEASURABLE while the numbers it wanted were sitting in the same payload.
*/
static void na_write_metrics(NaBuf* w, int faction_id, int base_id) {
    Faction& plr = Factions[faction_id];
    na_buf_puts(w, ",\"metrics\":{");
    na_buf_printf(w, "\"energy_reserves\":%d", plr.energy_credits);
    na_buf_printf(w, ",\"energy_income\":%d", plr.energy_surplus_total);
    na_buf_printf(w, ",\"labs_output\":%d", plr.labs_total);
    na_buf_printf(w, ",\"base_count\":%d", plr.base_count);
    na_buf_printf(w, ",\"pop_total\":%d", plr.pop_total);
    na_buf_printf(w, ",\"military_units\":%d", plr.total_combat_units);

    /*
    Drones, sweept from the bases because the engine keeps no faction-level total.

    metrics.py has carried this name since before anything reported it, which is the failure
    that vocabulary is supposed to prevent: a directive written against an unreported name is
    accepted at issue time and then reads UNMEASURABLE forever, which in a record looks like
    compliance rather than a gap. That mattered little while directives were hand-written; an
    agent can now issue one through issue_directive, so the name had to start meaning something
    or stop existing.

    Superdrones counted alongside drones, matching the vocabulary's own wording. They are
    separate citizens in separate engine fields, so this is a count of unhappy citizens rather
    than the weighted number the riot check uses (drone_riots compares drone_total against
    talent_total per base) — worth knowing when reading a directive written against it.
    */
    int drones = 0;
    for (int i = 0; i < *BaseCount && i < MaxBaseNum; i++) {
        if (Bases[i].faction_id == faction_id) {
            drones += Bases[i].drone_total + Bases[i].superdrone_total;
        }
    }
    na_buf_printf(w, ",\"drone_total\":%d", drones);
    /*
    Base-scope metrics on base-scope surfaces only.

    A faction-scope decision has no single base to report these for, and emitting
    one base's numbers on a faction decision would be worse than omitting them:
    a directive would evaluate against an arbitrary base and read as satisfied.
    metrics.py marks the scope of every name for exactly this reason.
    */
    if (base_id >= 0 && base_id < *BaseCount && base_id < MaxBaseNum) {
        BASE& b = Bases[base_id];
        const int total = mineral_cost(base_id, b.item());
        const int left = total - b.minerals_accumulated;
        const int remaining = left > 0 ? left : 0;
        na_buf_printf(w, ",\"mineral_surplus\":%d", b.mineral_surplus);
        na_buf_printf(w, ",\"minerals_remaining\":%d", remaining);
        na_buf_printf(w, ",\"pop_size\":%d", (int)b.pop_size);
        // Omitted rather than guessed when the base cannot finish anything: a
        // zero here would read as "completes this turn", which is the opposite
        // of the truth (metrics.py: absent must never read as satisfied).
        if (b.mineral_surplus > 0) {
            na_buf_printf(w, ",\"turns_to_completion\":%d",
                (remaining + b.mineral_surplus - 1) / b.mineral_surplus);
        }
    }
    na_buf_puts(w, "}");
}


/*
The base's own state — category 1 of the input checklist.

Ships accumulated minerals and surplus separately rather than a pre-computed
"turns remaining". A partially built item makes that arithmetic subtly wrong, and a
number that is quietly wrong is worse than two numbers the brain can divide.
*/
static void na_write_base_state(NaBuf* w, int base_id) {
    BASE& b = Bases[base_id];
    na_buf_puts(w, ",\"base_state\":{");
    na_buf_printf(w, "\"pop_size\":%d", (int)b.pop_size);
    na_buf_printf(w, ",\"minerals_accumulated\":%d", b.minerals_accumulated);
    na_buf_printf(w, ",\"mineral_surplus\":%d", b.mineral_surplus);
    na_buf_printf(w, ",\"nutrient_intake\":%d", b.nutrient_intake);
    na_buf_printf(w, ",\"mineral_intake\":%d", b.mineral_intake);
    na_buf_printf(w, ",\"energy_intake\":%d", b.energy_intake);
    na_buf_printf(w, ",\"eco_damage\":%d", b.eco_damage);
    na_buf_printf(w, ",\"worked_tiles\":%d", b.worked_tiles);
    na_buf_printf(w, ",\"specialists\":%d", b.specialist_total);
    na_buf_printf(w, ",\"queue_size\":%d", b.queue_size);
    na_buf_printf(w, ",\"current_item\":%d", b.item());
    na_buf_puts(w, ",\"current_item_name\":\"");
    na_buf_escaped(w, prod_name(b.item()));
    na_buf_puts(w, "\"}");
}

/*
What this base has recently been told to build, and who decided it.

Production is re-evaluated from scratch every turn. Nothing in the world view said what the
base was building last turn or why, so a brain reasoning only from the current snapshot will
flip between two defensible options turn after turn and accumulate nothing — each individual
choice arguable, the sequence useless. Half-built Recycling Tanks abandoned for a Scout Patrol
abandoned for Recycling Tanks is a decision process working exactly as designed and playing
badly.

An agent has a session and could in principle remember. It must not be asked to. Sessions
compact, reconnect, and get swapped for a different harness entirely, and the whole discipline
elsewhere in this adapter is that the payload wins over anything the brain believes it recalls.
History belongs in the world view for the same reason the board does.

`tier` on each entry is what lets the brain tell its own past reasoning from Thinker's — a
run that mixes both is otherwise a sequence of choices with no attribution, and "why did I
pick that" has no answer.

A ring buffer per base, eight deep: enough to show a pattern, small enough that 512 bases cost
about 25 KB and no allocation. Oldest is overwritten, which is the right end to lose.
*/
static const int NA_HISTORY = 8;

struct NaBuilt {
    int turn;
    int item;
    // 'l' llm, 'd' deterministic, anything else unattributed — one byte beats a pointer per
    // entry. No 'p' probe value: the observe probe reads history and never writes it, and the
    // contract has no "probe" tier to receive one.
    char tier;
};

static NaBuilt na_built[MaxBaseNum][NA_HISTORY];
static int na_built_next[MaxBaseNum];
static bool na_built_init = false;

static void na_history_reset() {
    for (int b = 0; b < MaxBaseNum; b++) {
        na_built_next[b] = 0;
        for (int i = 0; i < NA_HISTORY; i++) {
            na_built[b][i].turn = -1;
            na_built[b][i].item = 0;
            na_built[b][i].tier = '?';
        }
    }
    na_built_init = true;
}

/*
Record one base-turn's applied choice.

Called once per base-turn, from the same place the per-turn cache is written, so the engine's
several calls per base cannot inflate the history into a row of duplicates.

Loading a savegame is the case that needs guarding: base ids are reused, so a fresh game would
inherit the previous one's history and show the brain a past that never happened for these
bases. A recorded turn at or beyond the current one can only mean the clock went backwards, so
the whole buffer is dropped rather than partially trusted.
*/
static void na_history_put(int base_id, int item, char tier) {
    if (!na_built_init) {
        na_history_reset();
    }
    if (base_id < 0 || base_id >= MaxBaseNum) {
        return;
    }
    for (int i = 0; i < NA_HISTORY; i++) {
        if (na_built[base_id][i].turn >= *CurrentTurn) {
            na_history_reset();
            break;
        }
    }
    int slot = na_built_next[base_id];
    na_built[base_id][slot].turn = *CurrentTurn;
    na_built[base_id][slot].item = item;
    na_built[base_id][slot].tier = tier;
    na_built_next[base_id] = (slot + 1) % NA_HISTORY;
}

/*
The contract's `history` (WorldView.history / PriorChoice), OLDEST FIRST.

This block used to be emitted as `recent_builds`, newest first, on the reasoning that a brain
reading top-down should meet the most relevant entry first. Both halves of that were wrong in
the same way, and it cost the whole feature rather than merely some of it:

  - The orchestrator declares `history` and the system prompt explains `history`. Nothing ever
    mapped `recent_builds` onto it, so `WorldView.history` was None on every real decision and
    the prompt's continuity guidance gated on a field that was never present. The measured
    three-arm result behind that guidance (docs/decision-inputs.md §2) was obtained by setting
    `history` directly on a synthetic world view, so none of it reached a live game.
  - The payload still arrived, because WorldView allows extras and the whole object is dumped
    into the prompt. So the model was reading an unexplained block newest-first while holding a
    documented mental model that said oldest-first — which is worse than not sending it. A model
    applying the documented reading to the undocumented field takes the OLDEST entry for the most
    recent choice, and that is the na-eaa failure exactly: state handed over correctly and misread.

`item` is the action-space id ("unit:0" / "facility:4"), not the raw engine int it used to be.
The contract types it as a string, and the point of the field is that the brain can match a past
choice against an option in front of it exactly rather than by comparing display names.

The display name still rides along as `action`, matching what the action space calls it.
*/
static void na_write_history(NaBuf* w, int base_id) {
    if (!na_built_init) {
        na_history_reset();
    }
    na_buf_puts(w, ",\"history\":[");
    int written = 0;
    // Oldest first: walk the ring from the far end back to the newest entry.
    for (int back = NA_HISTORY; back >= 1; back--) {
        int slot = (na_built_next[base_id] - back + NA_HISTORY * 2) % NA_HISTORY;
        const NaBuilt& e = na_built[base_id][slot];
        if (e.turn < 0) {
            continue;
        }
        /*
        History is the PAST. mod_base_build fires several times per base-turn, and the first
        call records this turn's choice before the later ones serialise their own world view —
        so without this guard call_seq >= 2 emits a history containing the answer to the
        question it is asking. Measured: Zoloto-Gold turn 36 seq 2 carried a turn-36 entry.

        The decision path never saw it, because seq >= 2 is served from the per-turn cache and
        never reaches the brain. The emitted record is the problem: decision_stability.py takes
        the LAST row for a surface and re-decides it, which would hand the brain its own prior
        answer as history and report the resulting agreement as stability.
        */
        if (e.turn >= *CurrentTurn) {
            continue;
        }
        if (written++) {
            na_buf_puts(w, ",");
        }
        na_buf_printf(w, "{\"turn\":%d,\"item\":\"%s:%d\",\"action\":\"", e.turn,
            e.item >= 0 ? "unit" : "facility", e.item >= 0 ? e.item : -e.item);
        na_buf_escaped(w, prod_name(e.item));
        /*
        PriorChoice.tier is llm | deterministic | null, and null means "this adapter does not
        track authorship" — so anything we cannot attribute is emitted as null rather than
        flattened into "deterministic". The old code emitted "probe" here, which no call site
        ever writes and which the contract would have rejected outright, failing the whole
        world view rather than one field.
        */
        if (e.tier == 'l' || e.tier == 'd') {
            na_buf_printf(w, "\",\"tier\":\"%s\"}", e.tier == 'l' ? "llm" : "deterministic");
        } else {
            na_buf_puts(w, "\",\"tier\":null}");
        }
    }
    na_buf_puts(w, "]");
}

// Build the contract world view for one base's production decision.
static void na_build_base_production(NaBuf* w, int base_id, int native_choice,
                                     int has_gov, int call_seq) {
    BASE& base = Bases[base_id];
    int faction_id = base.faction_id;

    na_write_head(w, "base.production", "base", faction_id);
    na_buf_printf(w, ",\"base_id\":%d", base_id);
    na_buf_puts(w, ",\"base\":\"");
    na_buf_escaped(w, base.name);
    na_buf_puts(w, "\"");
    na_buf_printf(w, ",\"x\":%d,\"y\":%d", base.x, base.y);

    /*
    The engine's own pick, always recorded.

    It is no longer merely a record: it is the fallback this decision degrades to
    (invariant 9), so the log line shows both what the deterministic tier would
    have done and what actually ran. Losing that comparison is how an A/B against
    the deterministic tier (na-6db) stops being possible.
    */
    na_buf_printf(w, ",\"native_choice\":%d", native_choice);
    // has_gov is mod_base_build's own second parameter. Kept because it is free and
    // documents the call's context, but it does NOT discriminate between repeated
    // calls — measured 0 on every sample. call_seq is what does.
    na_buf_printf(w, ",\"has_gov\":%d", has_gov);
    na_buf_printf(w, ",\"call_seq\":%d", call_seq);
    na_buf_puts(w, ",\"native_choice_name\":\"");
    na_buf_escaped(w, prod_name(native_choice));
    na_buf_puts(w, "\"");
    na_write_base_state(w, base_id);
    na_write_metrics(w, faction_id, base_id);
    na_write_history(w, base_id);
    na_write_action_space(w, base_id);
}

/*
base.governor_config — the build priorities in force for one player-owned base.

Strictly an observation: it reports the weights governor_priorities resolved and
never changes them. The decision itself lives in na_governor_policy (plan.cpp),
where it belongs — this file serializes, it does not decide.

Deduplicated to one line per base per turn because governor_priorities has two
call sites (build.cpp:121 and :933) and fires on every production re-evaluation.
Without the guard the same unchanged configuration would emit several times a
turn, and the orchestrator's "one record per decision" invariant cannot tell a
fresh decision from a re-read. Solved here by turn-stamping rather than by a
discriminator field, because unlike production this surface genuinely decides at
most once per turn.

`source` is the field worth reading: "player" means the human set priorities and
the deterministic tier stood aside, "deterministic" means the tier supplied them,
and "unset" means the tier is off and the base is running the flat all-ones
weighting that motivated the surface in the first place.

The opt-in gate and the dedup both live here rather than at the call site because
the in-game hook and the probe have to disagree about exactly these two rules —
the probe must emit on demand and in a stock build — and splitting them across
two files is how they drift apart.
*/
static int na_gov_seen_turn[MaxBaseNum];
static int na_gov_seen_init = 0;
static int na_gov_probing = 0;

void na_observe_base_governor_config(int base_id, int applied, int growth,
                                     int tech, int wealth, int power, int fight) {
    if (base_id < 0 || base_id >= *BaseCount || base_id >= MaxBaseNum) {
        return;
    }
    BASE& b = Bases[base_id];
    if (!na_gov_probing) {
        // Opt-in only: an unconfigured build writes nothing at all.
        if (!conf.na_governor_policy && !llm_enabled(b.faction_id)) {
            return;
        }
        if (!na_gov_seen_init) {
            for (int i = 0; i < MaxBaseNum; i++) {
                na_gov_seen_turn[i] = -1;
            }
            na_gov_seen_init = 1;
        }
        if (na_gov_seen_turn[base_id] == *CurrentTurn) {
            return;
        }
        na_gov_seen_turn[base_id] = *CurrentTurn;
    }
    uint32_t gov = (uint32_t)b.governor_flags;

    NaBuf w;
    na_buf_init(&w);
    na_write_head(&w, "base.governor_config", "base", b.faction_id);
    na_buf_printf(&w, ",\"base_id\":%d", base_id);
    na_buf_puts(&w, ",\"base\":\"");
    na_buf_escaped(&w, b.name);
    na_buf_puts(&w, "\"");

    na_write_base_state(&w, base_id);
    na_write_metrics(&w, b.faction_id, base_id);

    // The raw bits, so a reader can confirm the policy fired for the stated reason.
    na_buf_printf(&w, ",\"governor_flags\":%u", gov);
    na_buf_printf(&w, ",\"governor_active\":%d", (gov & GOV_ACTIVE) ? 1 : 0);
    na_buf_puts(&w, ",\"priorities\":{");
    na_buf_printf(&w, "\"explore\":%d", (gov & GOV_PRIORITY_EXPLORE) ? 1 : 0);
    na_buf_printf(&w, ",\"discover\":%d", (gov & GOV_PRIORITY_DISCOVER) ? 1 : 0);
    na_buf_printf(&w, ",\"build\":%d", (gov & GOV_PRIORITY_BUILD) ? 1 : 0);
    na_buf_printf(&w, ",\"conquer\":%d", (gov & GOV_PRIORITY_CONQUER) ? 1 : 0);
    na_buf_puts(&w, "}");
    na_buf_printf(&w, ",\"defend_goal\":%d", (int)b.defend_goal);

    // The resolved weights are the actual outcome of this surface.
    na_buf_puts(&w, ",\"weights\":{");
    na_buf_printf(&w, "\"growth\":%d", growth);
    na_buf_printf(&w, ",\"tech\":%d", tech);
    na_buf_printf(&w, ",\"wealth\":%d", wealth);
    na_buf_printf(&w, ",\"power\":%d", power);
    na_buf_printf(&w, ",\"fight\":%d", fight);
    na_buf_puts(&w, "}");

    na_buf_puts(&w, ",\"tier\":\"deterministic\",\"source\":\"");
    na_buf_puts(&w, applied == 1 ? "deterministic" : (applied == 2 ? "player" : "unset"));
    na_buf_puts(&w, "\",\"applied\":\"native\"}");
    na_log_record(&w);
    na_buf_free(&w);
}

/*
base.abandon — the size-1 base whose colony pod would destroy it.

No dedup guard here, unlike base.governor_config: this fires only when a paid-for
pod is sitting in a size-1 base, which is rare, and the answer is acted on
immediately. Turn-stamping a decision that CHANGES the base would suppress the
record of a second, genuinely different decision after a reset.

Carries the growth numbers the verdict turns on — nutrient surplus and box, pop
size, whether this is the HQ, whether expansion is allowed — because "abandon: no"
is only checkable against the reason it was no.
*/
static int na_abandon_probing = 0;

void na_observe_base_abandon(int base_id, int abandon, int item_id) {
    if (base_id < 0 || base_id >= *BaseCount || base_id >= MaxBaseNum) {
        return;
    }
    BASE& b = Bases[base_id];
    if (!na_abandon_probing && !conf.na_abandon_policy && !llm_enabled(b.faction_id)) {
        return;
    }
    NaBuf w;
    na_buf_init(&w);
    na_write_head(&w, "base.abandon", "base", b.faction_id);
    na_buf_printf(&w, ",\"base_id\":%d", base_id);
    na_buf_puts(&w, ",\"base\":\"");
    na_buf_escaped(&w, b.name);
    na_buf_puts(&w, "\"");

    na_write_base_state(&w, base_id);
    na_write_metrics(&w, b.faction_id, base_id);

    na_buf_printf(&w, ",\"item_id\":%d", item_id);
    na_buf_puts(&w, ",\"item_name\":\"");
    na_buf_escaped(&w, prod_name(item_id));
    na_buf_puts(&w, "\"");

    // The inputs the verdict actually turns on.
    na_buf_puts(&w, ",\"inputs\":{");
    na_buf_printf(&w, "\"pop_size\":%d", (int)b.pop_size);
    na_buf_printf(&w, ",\"nutrient_surplus\":%d", b.nutrient_surplus);
    na_buf_printf(&w, ",\"nutrients_accumulated\":%d", b.nutrients_accumulated);
    na_buf_printf(&w, ",\"is_headquarters\":%d",
        has_fac_built(FAC_HEADQUARTERS, base_id) ? 1 : 0);
    na_buf_printf(&w, ",\"allow_expand\":%d", allow_expand(b.faction_id) ? 1 : 0);
    na_buf_puts(&w, "}");

    na_buf_printf(&w, ",\"abandon\":%d", abandon ? 1 : 0);
    /*
    outcome reports what was DONE, which is not the same as what was decided. The
    probe takes no action at all, so it must not claim the reset or the spend — a
    record that says kept_and_reselected when nothing was reselected is exactly the
    kind of line that gets believed later.
    */
    na_buf_puts(&w, ",\"outcome\":\"");
    if (na_abandon_probing) {
        na_buf_puts(&w, "none_probe");
    } else {
        na_buf_puts(&w, abandon ? "base_spent" : "kept_and_reselected");
    }
    na_buf_puts(&w, "\",\"tier\":\"deterministic\",\"applied\":\"");
    na_buf_puts(&w, na_abandon_probing ? "none" : "native");
    na_buf_puts(&w, "\"}");
    na_log_record(&w);
    na_buf_free(&w);
}

/*
The side-effect-free probe: emit a world view for one base and decide nothing.

Kept as a separate entry point from na_decide_base_production rather than a flag
on it, because the two differ in every way that matters — the probe does not
touch the network, does not apply anything, and must not consume a call_seq or
prime the per-turn cache. A flag would leave one function whose contract is "does
nothing, unless" and one line of the caller deciding which.

call_seq is emitted as 0, which no real decision ever uses, so a probe line can
never be mistaken for a decision when the log is analysed.
*/
void na_observe_base_production(int base_id, int native_choice, int has_gov) {
    if (base_id < 0 || base_id >= *BaseCount || base_id >= MaxBaseNum) {
        return;
    }
    NaBuf w;
    na_buf_init(&w);
    na_build_base_production(&w, base_id, native_choice, has_gov, 0);
    na_buf_puts(&w, ",\"tier\":\"probe\",\"applied\":\"none\"}");
    na_log_record(&w);
    na_buf_free(&w);
}

/*
Map a contract action id back onto the engine's item encoding.

The inverse of the ids na_write_action_space emits. Returns false for anything
that is not one of ours, which includes the case that matters: a model inventing
a plausible-looking id. That is invariant 1 — an illegal order must be impossible,
not merely unlikely — and the second half of it is the caller's membership check
against the action space, because a well-formed "unit:99" is still not buildable
in this base.
*/
static bool na_parse_item_id(const char* id, int* item) {
    if (!id) {
        return false;
    }
    int n = 0;
    if (sscanf(id, "unit:%d", &n) == 1 && n >= 0 && n < MaxProtoNum) {
        *item = n;
        return true;
    }
    if (sscanf(id, "facility:%d", &n) == 1 && n > 0 && n <= SP_ID_Last) {
        *item = -n;  // the engine encodes a facility as its negated id
        return true;
    }
    return false;
}

/*
Is `item` actually in this base's action space?

Re-runs the engine's own availability tests rather than re-parsing the JSON we
just built. The action space is generated from these calls, so asking them again
is asking the same authority the same question — and it costs a few hundred
integer comparisons, which is nothing against a network round trip.
*/
static bool na_item_is_legal(int base_id, int item) {
    int faction_id = Bases[base_id].faction_id;
    if (item >= 0) {
        return mod_veh_avail(item, faction_id, base_id) && can_build_unit(base_id, item);
    }
    int id = -item;
    return id > 0 && id <= SP_ID_Last
        && mod_facility_avail((FacilityId)id, faction_id, base_id, 0)
        && can_build(base_id, id);
}

/*
One base-turn, one decision.

The engine asks the same base several times per turn (see na_next_call_seq), and
each call applies its own answer. Asking the orchestrator every time would mean
paying for several model calls to settle one build and letting the last one win —
so the answer from call_seq 1 is cached and replayed for the rest of the turn.
That is also what makes "one decision record per decision" true rather than
aspirational.
*/
static int na_cached_turn[MaxBaseNum];
static int na_cached_item[MaxBaseNum];
static bool na_cached_init = false;

static bool na_cache_get(int base_id, int* item) {
    if (!na_cached_init) {
        for (int i = 0; i < MaxBaseNum; i++) {
            na_cached_turn[i] = -1;
            na_cached_item[i] = 0;
        }
        na_cached_init = true;
    }
    if (na_cached_turn[base_id] == *CurrentTurn) {
        *item = na_cached_item[base_id];
        return true;
    }
    return false;
}

static void na_cache_put(int base_id, int item) {
    na_cached_turn[base_id] = *CurrentTurn;
    na_cached_item[base_id] = item;
}

/*
A1: the first decision the brain actually gets to make.

Returns the item this base should build. Every failure path returns
`native_choice` — unreachable orchestrator, timeout, malformed reply, an id we
cannot parse, an id that is not legal here. That is invariant 9 stated as
control flow: there is exactly one `return` that is not the native choice, and it
is guarded by both a parse and a legality check.

The call is synchronous on the engine thread, bounded by conf.llm_timeout_ms.
Synchronous because the engine is not thread-safe and mod_base_build's signature
(one int in, one int out) has nowhere to park a decision and resume it later;
bounded because a turn-based game can afford a short pause and cannot afford an
open-ended one.
*/
int na_decide_base_production(int base_id, int native_choice, int has_gov) {
    if (base_id < 0 || base_id >= *BaseCount || base_id >= MaxBaseNum) {
        return native_choice;
    }
    const int call_seq = na_next_call_seq(base_id);

    int cached = 0;
    if (call_seq > 1 && na_cache_get(base_id, &cached)) {
        /*
        Re-verified, because the board moves between the engine's calls.

        The decision was taken at call_seq 1; by the time it is replayed the engine
        has processed other bases, spent minerals and possibly finished a secret
        project we picked. na_item_is_legal asks the engine's own availability
        tests again, which is the only authority worth asking.

        A replay that applies what was decided needs no second record — one
        decision, one record. A replay that DIVERGES absolutely does: without this
        the log asserted "llm chose X, applied X" while the base quietly built the
        deterministic tier's answer instead, and nothing anywhere could tell you.
        That is the exact failure the state guard exists to make visible, and it
        was invisible on the adapter side.
        */
        if (na_item_is_legal(base_id, cached)) {
            return cached;
        }
        NaBuf d;
        na_buf_init(&d);
        na_build_base_production(&d, base_id, native_choice, has_gov, call_seq);
        na_buf_puts(&d, ",\"tier\":\"deterministic\",\"applied\":\"native\"");
        na_buf_printf(&d, ",\"applied_item\":%d", native_choice);
        na_buf_puts(&d, ",\"applied_item_name\":\"");
        na_buf_escaped(&d, prod_name(native_choice));
        na_buf_printf(&d, "\",\"superseded_item\":%d", cached);
        na_buf_puts(&d, ",\"fallback_reason\":\"cached choice became illegal before replay\"}");
        na_log_record(&d);
        na_buf_free(&d);
        // Forget it, so the remaining calls this turn do not each re-discover and
        // re-report the same divergence.
        na_cache_put(base_id, native_choice);
        na_history_put(base_id, native_choice, 'd');
        return native_choice;
    }

    NaBuf w;
    na_buf_init(&w);
    na_build_base_production(&w, base_id, native_choice, has_gov, call_seq);

    int applied = native_choice;
    const char* tier = "deterministic";
    const char* how = "native";
    char detail[192];
    detail[0] = '\0';

    if (w.failed) {
        snprintf(detail, sizeof(detail), "world view could not be built");
    } else {
        /*
        The record and the request body are the same bytes, and they differ only
        by the outcome fields appended after the call returns. So the object is
        closed in place, sent, then reopened by rewinding the length — rather than
        copying a world view that runs to tens of kilobytes just to add two fields.
        */
        const size_t open_len = w.len;
        na_buf_puts(&w, "}");
        NaBuf body;
        if (!w.failed && na_http_post(llm_endpoint.c_str(), "/decide", w.data,
                                      conf.llm_timeout_ms, &body)) {
            char action_id[64];
            int item = 0;
            if (!na_json_string(body.data, "action_id", action_id, sizeof(action_id))) {
                snprintf(detail, sizeof(detail), "no action_id in reply");
            } else if (!na_parse_item_id(action_id, &item)) {
                snprintf(detail, sizeof(detail), "unparseable action_id %.48s", action_id);
            } else if (!na_item_is_legal(base_id, item)) {
                // The invariant-1 backstop. Worth its own detail string: an id that
                // parses but is not legal means the action space and the reply
                // disagree, which is a bug in us, not a bad model day.
                snprintf(detail, sizeof(detail), "illegal action_id %.48s", action_id);
            } else {
                applied = item;
                tier = "llm";
                how = na_json_true(body.data, "degraded") ? "llm_degraded" : "llm";
            }
            na_buf_free(&body);
        } else {
            snprintf(detail, sizeof(detail), "orchestrator unreachable or slow");
        }
        if (!w.failed) {
            w.len = open_len;
            w.data[w.len] = '\0';
        }
    }

    na_cache_put(base_id, applied);
    na_history_put(base_id, applied, tier[0] == 'l' ? 'l' : 'd');

    na_buf_printf(&w, ",\"tier\":\"%s\",\"applied\":\"%s\"", tier, how);
    na_buf_printf(&w, ",\"applied_item\":%d", applied);
    na_buf_puts(&w, ",\"applied_item_name\":\"");
    na_buf_escaped(&w, prod_name(applied));
    na_buf_puts(&w, "\"");
    if (detail[0]) {
        na_buf_puts(&w, ",\"fallback_reason\":\"");
        na_buf_escaped(&w, detail);
        na_buf_puts(&w, "\"");
    }
    na_buf_puts(&w, "}");
    na_log_record(&w);
    na_buf_free(&w);
    return applied;
}

/*
Did the engine actually keep what we handed it?

Every gate in this file is a check we thought to write: the id parses, tech_avail says yes,
society_avail says yes, the faction can afford it. That catches the failures we anticipated. It
cannot catch the ones we did not — a rule nobody encoded here, an engine path that overwrites
queue_items[0] after mod_base_change, a retool interaction, a later hook with its own opinion.
Those are exactly the cases where the record says "llm chose X, applied X" while the base builds
something else, and nothing anywhere can tell you.

So this asks a different question from all the gates: not "should the engine accept this" but
"did it". Read the state back after the apply and compare it against what was decided. That
needs no knowledge of WHY a choice was dropped, which is the point — it is the one check that
covers rules we have not learned yet.

Silent when they agree, which is almost always. A divergence gets its own compact record rather
than being folded into the decision record, because by the time this runs that record is already
written and sent — and because a divergence genuinely is a second event: the decision happened,
then something undid it.

The cache is updated to what the engine actually has, so the remaining calls this base-turn do
not each rediscover and re-report the same divergence. mod_base_reset is hooked at eleven call
sites; without that, one dropped choice would be eleven identical records.
*/
void na_verify_base_production(int base_id) {
    if (base_id < 0 || base_id >= *BaseCount || base_id >= MaxBaseNum) {
        return;
    }
    int intended = 0;
    if (!na_cache_get(base_id, &intended)) {
        // Nothing was decided for this base this turn, so there is nothing to have diverged
        // from. A base the LLM tier never touched is not this function's business.
        return;
    }
    BASE& base = Bases[base_id];
    const int actual = base.item();
    if (actual == intended) {
        return;
    }

    FILE* lf = na_log_open();
    if (lf) {
        fprintf(lf, "{\"surface_id\":\"base.production\",\"engine\":\"thinker\"");
        fprintf(lf, ",\"scope\":\"base\",\"turn\":%d,\"base_id\":%d", *CurrentTurn, base_id);
        fputs(",\"base\":\"", lf);
        na_write_escaped(lf, base.name);
        fprintf(lf, "\",\"event\":\"divergence\",\"intended_item\":%d", intended);
        fputs(",\"intended_item_name\":\"", lf);
        na_write_escaped(lf, prod_name(intended));
        fprintf(lf, "\",\"applied_item\":%d", actual);
        fputs(",\"applied_item_name\":\"", lf);
        na_write_escaped(lf, prod_name(actual));
        // Deliberately not a "reason": we do not know one. Naming a cause we have not
        // established is how a guess becomes a fact in someone's analysis three months later.
        fputs("\",\"fallback_reason\":\"engine did not keep the applied item\"}\n", lf);
        fflush(lf);
    }

    na_cache_put(base_id, actual);
    na_history_put(base_id, actual, 'd');
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
static void na_write_tech_action_space(NaBuf* w, int faction_id) {
    int count = 0;
    /*
    Cost is per-tech ONLY under Thinker's revised_tech_cost house rule (tech.cpp:294); stock
    SMAC charges the same for whichever tech is next (tech.cpp:308 asserts exactly that). So a
    per-option turns figure is emitted when it can differ and omitted when it cannot — a column
    of identical numbers invites a brain to compare options on a difference that does not exist.
    */
    const int rate = mod_tech_rate(faction_id);
    const int banked = Factions[faction_id].tech_accumulated;
    const bool per_tech = conf.revised_tech_cost && !*MultiplayerActive;
    na_buf_puts(w, ",\"action_space\":[");
    for (int id = 0; id < MaxTechnologyNum; id++) {
        if (!tech_avail(id, faction_id)) {
            continue;
        }
        if (count++) { na_buf_puts(w, ","); }
        na_buf_printf(w, "{\"id\":\"tech:%d\",\"action\":\"", id);
        na_buf_escaped(w, Tech[id].name);
        na_buf_puts(w, "\",\"category\":\"tech\"");
        na_buf_printf(w, ",\"ai_weights\":{\"growth\":%d,\"tech\":%d,\"wealth\":%d,\"power\":%d}",
                Tech[id].AI_growth, Tech[id].AI_tech, Tech[id].AI_wealth, Tech[id].AI_power);
        if (per_tech && rate > 0) {
            int cost = tech_alt_cost(id, faction_id);
            int left = cost - banked;
            if (left < 0) { left = 0; }
            na_buf_printf(w, ",\"cost\":%d,\"turns_to_complete\":%d",
                          cost, (left + rate - 1) / rate);
        }
        na_buf_puts(w, "}");
    }
    na_buf_printf(w, "],\"action_space_size\":%d", count);
}

/*
The faction.tech world view, up to but not including the outcome fields.

Split out of na_observe_faction_tech for the same reason na_build_base_production is split
out: the observe path and the decide path must send byte-identical world views, or a record
written by one is not comparable with a record written by the other. One builder, two callers,
and the only difference between their records is the tier/applied tail each appends.

Leaves the JSON object OPEN. The caller closes it, because the decide path sends the open
buffer as a request body and then reopens it to append what came back.
*/
static void na_build_faction_tech(NaBuf* w, int faction_id, int native_choice) {
    Faction& plr = Factions[faction_id];
    na_write_head(w, "faction.tech", "turn", faction_id);

    // Faction research and economic state - categories 1 and 4 of the input checklist.
    na_write_metrics(w, faction_id, -1);
    const int rate = mod_tech_rate(faction_id);
    na_buf_printf(w, ",\"tech_accumulated\":%d", plr.tech_accumulated);
    na_buf_printf(w, ",\"tech_rate\":%d", rate);

    /*
    Whether this is a fresh choice or a probe of one already running — and how long it binds.

    Research is not production. Production is re-decided every turn, so a choice there is
    cheap to revisit. tech_selection fires only when tech_research_id < 0, i.e. when nothing
    is being researched (tech.cpp:233), so a real selection COMMITS the faction until the
    tech completes. Switching mid-research is possible and is not what players normally do.

    None of that was in the world view. It carried accumulated points and a rate and no cost,
    so a decision that binds the faction for several turns looked indistinguishable from a
    one-turn pick — and a brain asked whether this "genuinely sets direction for future turns"
    had nothing to answer with. Measured: it issued no directive on ten consecutive runs.

    research_state is the honest signal. The probe passes the CURRENT target as native_choice,
    so an in_progress record is a serialiser test rather than a decision, and a reader has to
    be able to tell those apart.
    */
    const bool idle = plr.tech_research_id < 0;
    na_buf_printf(w, ",\"research_state\":\"%s\"", idle ? "idle" : "in_progress");
    if (!idle && plr.tech_research_id < MaxTechnologyNum) {
        na_buf_printf(w, ",\"current_research\":%d,\"current_research_name\":\"",
                      plr.tech_research_id);
        na_buf_escaped(w, Tech[plr.tech_research_id].name);
        na_buf_puts(w, "\"");
    }
    na_buf_printf(w, ",\"tech_cost\":%d", plr.tech_cost);
    if (rate > 0 && plr.tech_cost > 0) {
        int left = plr.tech_cost - plr.tech_accumulated;
        if (left < 0) { left = 0; }
        na_buf_printf(w, ",\"turns_to_complete\":%d", (left + rate - 1) / rate);
    }

    na_buf_printf(w, ",\"native_choice\":%d", native_choice);
    na_buf_puts(w, ",\"native_choice_name\":\"");
    if (native_choice >= 0 && native_choice < MaxTechnologyNum) {
        na_buf_escaped(w, Tech[native_choice].name);
    }
    na_buf_puts(w, "\"");

    na_write_tech_action_space(w, faction_id);
}

void na_observe_faction_tech(int faction_id, int native_choice) {
    if (faction_id <= 0 || faction_id >= MaxPlayerNum) {
        return;
    }
    NaBuf wb;
    NaBuf* w = &wb;
    na_buf_init(w);
    na_build_faction_tech(w, faction_id, native_choice);
    na_buf_puts(w, ",\"tier\":\"deterministic\",\"applied\":\"native\"}");
    na_log_record(w);
    na_buf_free(w);
}

/*
Is `tech_id` actually in this faction's action space?

tech_avail is the same call na_write_tech_action_space filters on, so this asks the engine the
same question that produced the options — the invariant-1 backstop, not a reconstruction of it.
*/
static bool na_tech_is_legal(int faction_id, int tech_id) {
    return tech_id >= 0 && tech_id < MaxTechnologyNum && tech_avail(tech_id, faction_id);
}

static bool na_parse_tech_id(const char* id, int* tech) {
    int n = 0;
    if (id && sscanf(id, "tech:%d", &n) == 1 && n >= 0 && n < MaxTechnologyNum) {
        *tech = n;
        return true;
    }
    return false;
}

/*
faction.tech, decided rather than observed.

Same three gates as base.production, and they are the whole of what makes this safe: the reply
must parse as an id this adapter mints, the tech must pass the engine's OWN availability test,
and the exchange is bounded by llm_timeout_ms. Every failure applies the deterministic tier's
answer and records why, so an unreachable orchestrator costs a research choice, never a turn
(invariant 9).

No call-seq cache here, unlike base.production, and that is a real difference rather than an
omission. The engine asks a base for production several times per turn, so that path needs a
cache to keep one decision from becoming several records. mod_tech_selection fires only when
tech_research_id < 0 (tech.cpp:233) — once per research cycle, not once per turn — so the
question is asked once and answered once by construction.

That same fact is why this decision is worth more than a production pick: it COMMITS the
faction until the tech completes. The world view says so via research_state and
turns_to_complete, and this is the call that makes the commitment real.
*/
int na_decide_faction_tech(int faction_id, int native_choice) {
    if (faction_id <= 0 || faction_id >= MaxPlayerNum) {
        return native_choice;
    }

    NaBuf w;
    na_buf_init(&w);
    na_build_faction_tech(&w, faction_id, native_choice);

    int applied = native_choice;
    const char* tier = "deterministic";
    const char* how = "native";
    char detail[192];
    detail[0] = '\0';

    if (w.failed) {
        snprintf(detail, sizeof(detail), "world view could not be built");
    } else {
        // Closed in place, sent, then reopened by rewinding the length — the record and the
        // request body are the same bytes and differ only by the tail appended after the call.
        const size_t open_len = w.len;
        na_buf_puts(&w, "}");
        NaBuf body;
        if (!w.failed && na_http_post(llm_endpoint.c_str(), "/decide", w.data,
                                      conf.llm_timeout_ms, &body)) {
            char action_id[64];
            int tech_id = 0;
            if (!na_json_string(body.data, "action_id", action_id, sizeof(action_id))) {
                snprintf(detail, sizeof(detail), "no action_id in reply");
            } else if (!na_parse_tech_id(action_id, &tech_id)) {
                snprintf(detail, sizeof(detail), "unparseable action_id %.48s", action_id);
            } else if (!na_tech_is_legal(faction_id, tech_id)) {
                snprintf(detail, sizeof(detail), "illegal action_id %.48s", action_id);
            } else {
                applied = tech_id;
                tier = "llm";
                how = na_json_true(body.data, "degraded") ? "llm_degraded" : "llm";
            }
            na_buf_free(&body);
        } else {
            snprintf(detail, sizeof(detail), "orchestrator unreachable or slow");
        }
        if (!w.failed) {
            w.len = open_len;
            w.data[w.len] = '\0';
        }
    }

    na_buf_printf(&w, ",\"tier\":\"%s\",\"applied\":\"%s\"", tier, how);
    na_buf_printf(&w, ",\"applied_item\":%d", applied);
    na_buf_puts(&w, ",\"applied_item_name\":\"");
    if (applied >= 0 && applied < MaxTechnologyNum) {
        na_buf_escaped(&w, Tech[applied].name);
    }
    na_buf_puts(&w, "\"");
    if (detail[0]) {
        na_buf_puts(&w, ",\"fallback_reason\":\"");
        na_buf_escaped(&w, detail);
        na_buf_puts(&w, "\"");
    }
    na_buf_puts(&w, "}");
    na_log_record(&w);
    na_buf_free(&w);
    return applied;
}

/*
Social engineering. field/model describe the change the deterministic tier settled on, or
field < 0 for "no change this turn" — which is a real decision and is recorded as one
rather than as an absence.
*/
static void na_build_faction_se(NaBuf* w, int faction_id, int field, int model, int cost) {
    Faction& plr = Factions[faction_id];
    const int* current = &plr.SE_Politics;
    na_write_head(w, "faction.se", "turn", faction_id);

    // Current settings, by name rather than index — an index means nothing in a prompt.
    na_buf_puts(w, ",\"current\":{");
    for (int f = 0; f < MaxSocialCatNum; f++) {
        int m = current[f];
        if (f) { na_buf_puts(w, ","); }
        na_buf_puts(w, "\"");
        na_buf_escaped(w, SocialField[f].field_name);
        na_buf_puts(w, "\":\"");
        if (m >= 0 && m < MaxSocialModelNum) {
            na_buf_escaped(w, SocialField[f].soc_name[m]);
        }
        na_buf_puts(w, "\"");
    }
    na_buf_puts(w, "}");

    na_write_metrics(w, faction_id, -1);

    /*
    The action space: every (field, model) the faction may legally adopt.

    Excluded, not flagged: models whose prerequisite tech is unresearched, and the model
    already in force for that field. Faction-prohibited models are excluded too — factions
    are forbidden certain values, and that is a rule the action space must enforce because
    grounding only advises.
    */
    int count = 0;
    na_buf_puts(w, ",\"action_space\":[{\"id\":\"se:none\",\"action\":\"No change\",\"category\":\"se\"}");
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
            na_buf_printf(w, ",{\"id\":\"se:%d:%d\",\"action\":\"", f, m);
            na_buf_escaped(w, SocialField[f].soc_name[m]);
            na_buf_puts(w, "\",\"field\":\"");
            na_buf_escaped(w, SocialField[f].field_name);
            na_buf_puts(w, "\",\"category\":\"se\",\"effects\":{");
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
                if (shown++) { na_buf_puts(w, ","); }
                na_buf_printf(w, "\"%s\":%d", eff_names[e], v);
            }
            na_buf_puts(w, "}");

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
            na_buf_printf(w, ",\"cost\":%d,\"cost_unit\":\"credits\"", upheaval);
            na_buf_printf(w, ",\"affordable\":%s",
                    upheaval <= plr.energy_credits ? "true" : "false");
            na_buf_puts(w, "}");
        }
    }
    na_buf_printf(w, "],\"action_space_size\":%d", count);

    if (field >= 0 && field < MaxSocialCatNum && model >= 0 && model < MaxSocialModelNum) {
        na_buf_printf(w, ",\"native_choice\":\"se:%d:%d\"", field, model);
        na_buf_puts(w, ",\"native_choice_name\":\"");
        na_buf_escaped(w, SocialField[field].field_name);
        na_buf_puts(w, " -> ");
        na_buf_escaped(w, SocialField[field].soc_name[model]);
        na_buf_printf(w, "\",\"upheaval_cost\":%d", cost);
    } else {
        na_buf_puts(w, ",\"native_choice\":\"se:none\",\"native_choice_name\":\"No change\"");
        na_buf_puts(w, ",\"upheaval_cost\":0");
    }
}

void na_observe_faction_se(int faction_id, int field, int model, int cost) {
    if (faction_id <= 0 || faction_id >= MaxPlayerNum) {
        return;
    }
    NaBuf wb;
    NaBuf* w = &wb;
    na_buf_init(w);
    na_build_faction_se(w, faction_id, field, model, cost);
    na_buf_puts(w, ",\"tier\":\"deterministic\",\"applied\":\"native\"}");
    na_log_record(w);
    na_buf_free(w);
}

static bool na_parse_se_id(const char* id, int* field, int* model) {
    if (!id) {
        return false;
    }
    if (strcmp(id, "se:none") == 0) {
        *field = -1;
        *model = -1;
        return true;
    }
    int f = 0;
    int m = 0;
    if (sscanf(id, "se:%d:%d", &f, &m) == 2
    && f >= 0 && f < MaxSocialCatNum && m >= 0 && m < MaxSocialModelNum) {
        *field = f;
        *model = m;
        return true;
    }
    return false;
}

/*
faction.se, decided rather than observed.

Four gates here rather than base.production's three, because this surface SPENDS.

Parse, then society_avail — the engine's own test, and deliberately not the checks the action
space makes for itself. Those two are supposed to agree; this is the one that binds, so an
order can only do what the engine would have allowed.

Then affordability, which is the gate the other surfaces do not need. social_upheaval is
computed against the WHOLE proposed category set rather than the single field being changed,
so it has to be asked about the brain's choice specifically — the cost of the deterministic
tier's candidate says nothing about the cost of a different one. A change the faction cannot
afford is one the engine would refuse, and letting it through would produce a record saying
"llm chose X, applied X" while the faction quietly changed nothing. That divergence is exactly
what base.production's re-verification exists to catch, and it is cheaper to prevent here.

The caller applies. This function decides and records; it does not touch energy_credits or
pending. Debiting in two places is how a faction ends up paying twice or paying nothing, and
faction.cpp already does it correctly for whatever choice it is handed.

"se:none" is a legal answer and a real decision, not an absence — a brain that judges the
upheaval not worth paying this turn has decided something, and coverage counts what fired.
*/
void na_decide_faction_se(int faction_id, int* field, int* model) {
    if (faction_id <= 0 || faction_id >= MaxPlayerNum || !field || !model) {
        return;
    }
    Faction& plr = Factions[faction_id];

    const int native_field = *field;
    const int native_model = *model;
    int native_cost = 0;
    if (native_field >= 0 && native_field < MaxSocialCatNum) {
        CSocialCategory native_soc;
        memcpy(&native_soc, &plr.SE_Politics, sizeof(native_soc));
        native_soc.models[native_field] = native_model;
        native_cost = social_upheaval(faction_id, &native_soc);
    }

    NaBuf w;
    na_buf_init(&w);
    na_build_faction_se(&w, faction_id, native_field, native_model, native_cost);

    int applied_field = native_field;
    int applied_model = native_model;
    const char* tier = "deterministic";
    const char* how = "native";
    char detail[192];
    detail[0] = '\0';

    if (w.failed) {
        snprintf(detail, sizeof(detail), "world view could not be built");
    } else {
        const size_t open_len = w.len;
        na_buf_puts(&w, "}");
        NaBuf body;
        if (!w.failed && na_http_post(llm_endpoint.c_str(), "/decide", w.data,
                                      conf.llm_timeout_ms, &body)) {
            char action_id[64];
            int f = -1;
            int m = -1;
            if (!na_json_string(body.data, "action_id", action_id, sizeof(action_id))) {
                snprintf(detail, sizeof(detail), "no action_id in reply");
            } else if (!na_parse_se_id(action_id, &f, &m)) {
                snprintf(detail, sizeof(detail), "unparseable action_id %.48s", action_id);
            } else if (f >= 0 && !society_avail(f, m, faction_id)) {
                snprintf(detail, sizeof(detail), "illegal action_id %.48s", action_id);
            } else {
                bool ok = true;
                if (f >= 0) {
                    CSocialCategory soc;
                    memcpy(&soc, &plr.SE_Politics, sizeof(soc));
                    soc.models[f] = m;
                    const int cost = social_upheaval(faction_id, &soc);
                    if (plr.energy_credits <= cost) {
                        ok = false;
                        snprintf(detail, sizeof(detail),
                                 "upheaval costs %d, reserves %d", cost, plr.energy_credits);
                    }
                }
                if (ok) {
                    applied_field = f;
                    applied_model = m;
                    tier = "llm";
                    how = na_json_true(body.data, "degraded") ? "llm_degraded" : "llm";
                }
            }
            na_buf_free(&body);
        } else {
            snprintf(detail, sizeof(detail), "orchestrator unreachable or slow");
        }
        if (!w.failed) {
            w.len = open_len;
            w.data[w.len] = '\0';
        }
    }

    na_buf_printf(&w, ",\"tier\":\"%s\",\"applied\":\"%s\"", tier, how);
    if (applied_field >= 0 && applied_field < MaxSocialCatNum) {
        na_buf_printf(&w, ",\"applied_item\":\"se:%d:%d\"", applied_field, applied_model);
        na_buf_puts(&w, ",\"applied_item_name\":\"");
        na_buf_escaped(&w, SocialField[applied_field].field_name);
        na_buf_puts(&w, " -> ");
        na_buf_escaped(&w, SocialField[applied_field].soc_name[applied_model]);
        na_buf_puts(&w, "\"");
    } else {
        na_buf_puts(&w, ",\"applied_item\":\"se:none\",\"applied_item_name\":\"No change\"");
    }
    if (detail[0]) {
        na_buf_puts(&w, ",\"fallback_reason\":\"");
        na_buf_escaped(&w, detail);
        na_buf_puts(&w, "\"");
    }
    na_buf_puts(&w, "}");
    na_log_record(&w);
    na_buf_free(&w);

    *field = applied_field;
    *model = applied_model;
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
/*
`native_hurried` is a tri-state here: > 0 hurried, 0 declined, and < 0 "not yet asked".

The decide path runs BEFORE the deterministic tier, so at that point there is no native choice
to report. It passes < 0 and native_choice is omitted rather than defaulted, because defaulting
it to "hurry:none" would put a fabricated answer in front of the brain — absent reads as "not
determined", false reads as "the engine decided not to".
*/
/*
The hurry terms, derived in exactly one place.

What may be rushed and what it costs was worked out independently in three functions — the
emitter that offers the option, the gate that accepts it, and briefly an audit that compared
the two. They were not even written the same way: the emitter measured from the caller's
`minerals_before`, the gate from the live `minerals_accumulated`. Those agree at the instant a
decision is made and there is no reason they had to.

Auditing for that drift was the wrong instinct. A check that the two agree can only ever fail
after someone has already introduced the bug, and only if the audit happens to run; deriving it
once means they cannot disagree. Prefer the version with no failure mode over the version with
a detector.

`remaining` comes back clamped at zero. Every caller guards on it being positive anyway, and a
negative "minerals still needed" is not a quantity anyone should have to reason about.
*/
static bool na_hurry_terms(int base_id, int item, int minerals, int credits,
                           int* total_out, int* remaining_out, int* cost_out) {
    const int total = mineral_cost(base_id, item);
    int remaining = total - minerals;
    if (remaining < 0) {
        remaining = 0;
    }
    const int cost = hurry_cost(base_id, item, remaining);
    if (total_out) {
        *total_out = total;
    }
    if (remaining_out) {
        *remaining_out = remaining;
    }
    if (cost_out) {
        *cost_out = cost;
    }
    return Bases[base_id].can_hurry_item() && cost > 0 && cost <= credits;
}

static void na_build_base_hurry(NaBuf* w, int base_id, int item, int minerals_before,
                                int credits_before, int native_hurried) {
    BASE& base = Bases[base_id];
    const int faction_id = base.faction_id;

    int total = 0;
    int remaining = 0;
    int cost = 0;
    const bool affordable_terms = na_hurry_terms(base_id, item, minerals_before,
                                                 credits_before, &total, &remaining, &cost);
    const int surplus = base.mineral_surplus;
    const int turns_if_waiting =
        surplus > 0 && remaining > 0 ? (remaining + surplus - 1) / surplus : 0;

    na_write_head(w, "base.hurry", "base", faction_id);
    na_buf_printf(w, ",\"base_id\":%d", base_id);
    na_buf_puts(w, ",\"base\":\"");
    na_buf_escaped(w, base.name);
    na_buf_puts(w, "\"");

    na_buf_puts(w, ",\"item\":\"");
    na_buf_escaped(w, prod_name(item));
    na_buf_puts(w, "\"");

    /*
    The entity this decision is ABOUT, as opposed to the entities it chooses between.

    Retrieval keys off action labels, which works for base.production ("Colony Pod" is
    a node in the datalinks) and retrieves nothing at all here: this surface offers
    "Hurry production" and "Do not hurry", and neither is in any graph. Measured, that
    left base.hurry the least grounded surface we have. The item being hurried is the
    subject, and naming it is the adapter's job — the orchestrator must not go digging
    through an engine's own field layout to guess (invariant 2).
    */
    na_buf_puts(w, ",\"subjects\":[\"");
    na_buf_escaped(w, prod_name(item));
    na_buf_puts(w, "\"]");
    na_buf_puts(w, ",\"base_state\":{");
    na_buf_printf(w, "\"minerals_accumulated\":%d", minerals_before);
    na_buf_printf(w, ",\"mineral_cost_total\":%d", total);
    na_buf_printf(w, ",\"minerals_remaining\":%d", remaining > 0 ? remaining : 0);
    na_buf_printf(w, ",\"mineral_surplus\":%d", surplus);
    na_buf_printf(w, ",\"turns_if_waiting\":%d", turns_if_waiting);
    na_buf_printf(w, ",\"energy_reserves\":%d", credits_before);
    na_buf_printf(w, ",\"pop_size\":%d", (int)base.pop_size);
    na_buf_puts(w, "}");
    na_write_metrics(w, faction_id, base_id);

    /*
    Affordability is part of the action space, not advice. An option the faction cannot pay for
    should not be offered: the engine would refuse it, and offering it invites the brain to
    "decide" something that was never available.
    */
    const bool affordable = affordable_terms;
    na_buf_puts(w, ",\"action_space\":[{\"id\":\"hurry:none\",\"action\":\"Do not hurry\"");
    na_buf_puts(w, ",\"cost\":0,\"cost_unit\":\"credits\"}");
    if (affordable) {
        na_buf_printf(w, ",{\"id\":\"hurry:now\",\"action\":\"Hurry production\",\"cost\":%d", cost);
        na_buf_printf(w, ",\"cost_unit\":\"credits\",\"saves_turns\":%d", turns_if_waiting);
        // Spends credits and completes the item, so it moves two metrics. Both are
        // declared: the orchestrator computes a directive trade-off from `effects`
        // and nothing else, so an undeclared effect is an invisible one.
        na_buf_printf(w, ",\"effects\":{\"energy_reserves\":%d,\"minerals_remaining\":%d}}",
                      -cost, -(remaining > 0 ? remaining : 0));
    }
    na_buf_printf(w, "],\"action_space_size\":%d", affordable ? 2 : 1);

    if (native_hurried >= 0) {
        na_buf_printf(w, ",\"native_choice\":\"%s\"", native_hurried ? "hurry:now" : "hurry:none");
    }
}

void na_observe_base_hurry(int base_id, int item, int minerals_before, int credits_before,
                           int native_hurried) {
    if (base_id < 0 || base_id >= *BaseCount || base_id >= MaxBaseNum) {
        return;
    }
    NaBuf wb;
    NaBuf* w = &wb;
    na_buf_init(w);
    na_build_base_hurry(w, base_id, item, minerals_before, credits_before,
                        native_hurried ? 1 : 0);
    na_buf_puts(w, ",\"tier\":\"deterministic\",\"applied\":\"native\"}");
    na_log_record(w);
    na_buf_free(w);
}

/*
base.hurry, decided rather than observed.

The strictest of the four, because it is the one that can lose something irreversibly, and the
only one where deciding meant moving the hook rather than assigning a return value.
mod_base_hurry both decides AND spends on its way out through hurry_item, so there is no point
after it at which a different answer can still be given. This runs first instead, and calls the
deterministic tier only when it is standing down.

Three engine checks before any purchase, all the engine's own: can_hurry_item, a positive
hurry_cost, and enough reserves to pay it. hurry_item does the credit debit and the mineral
credit together — doing either half here would be how a faction gets free production.

Returns what the engine hook should return: 1 hurried, 0 did not.

Exactly one record on every path, which is why this owns the fallback rather than letting the
caller handle it. Falling back means running mod_base_hurry and reporting what IT did, and a
caller that wrote its own record for that case would either duplicate this one or build the
world view twice to avoid it.

"hurry:none" is a real decision, not an absence. A brain that judges the credits better kept is
answering the question, and the deterministic tier is deliberately NOT consulted afterwards —
asking it to second-guess a considered "no" would make the surface unfalsifiable.
*/
int na_decide_base_hurry(int base_id, int item, int minerals_before, int credits_before) {
    if (base_id < 0 || base_id >= *BaseCount || base_id >= MaxBaseNum) {
        return mod_base_hurry();
    }
    BASE& base = Bases[base_id];
    Faction& plr = Factions[base.faction_id];

    NaBuf w;
    na_buf_init(&w);
    na_build_base_hurry(&w, base_id, item, minerals_before, credits_before, -1);

    int rc = 0;
    bool decided = false;
    const char* tier = "deterministic";
    const char* how = "native";
    const char* chosen = "hurry:none";
    int spent = 0;
    char detail[192];
    detail[0] = '\0';

    if (w.failed) {
        snprintf(detail, sizeof(detail), "world view could not be built");
    } else {
        const size_t open_len = w.len;
        na_buf_puts(&w, "}");
        NaBuf body;
        if (!w.failed && na_http_post(llm_endpoint.c_str(), "/decide", w.data,
                                      conf.llm_timeout_ms, &body)) {
            char action_id[64];
            if (!na_json_string(body.data, "action_id", action_id, sizeof(action_id))) {
                snprintf(detail, sizeof(detail), "no action_id in reply");
            } else if (strcmp(action_id, "hurry:none") == 0) {
                decided = true;
                rc = 0;
                tier = "llm";
                how = na_json_true(body.data, "degraded") ? "llm_degraded" : "llm";
            } else if (strcmp(action_id, "hurry:now") != 0) {
                snprintf(detail, sizeof(detail), "unparseable action_id %.48s", action_id);
            } else {
                int remaining = 0;
                int cost = 0;
                // Same derivation the action space used, because it is the same function —
                // an option offered and then refused on a re-derived number would be a
                // divergence invented by us rather than one the engine caused.
                if (!na_hurry_terms(base_id, item, base.minerals_accumulated,
                                    plr.energy_credits, NULL, &remaining, &cost)) {
                    snprintf(detail, sizeof(detail),
                             "cannot hurry: cost=%d reserves=%d remaining=%d",
                             cost, plr.energy_credits, remaining);
                } else {
                    hurry_item(base_id, remaining, cost);
                    decided = true;
                    rc = 1;
                    spent = cost;
                    chosen = "hurry:now";
                    tier = "llm";
                    how = na_json_true(body.data, "degraded") ? "llm_degraded" : "llm";
                }
            }
            na_buf_free(&body);
        } else {
            snprintf(detail, sizeof(detail), "orchestrator unreachable or slow");
        }
        if (!w.failed) {
            w.len = open_len;
            w.data[w.len] = '\0';
        }
    }

    if (!decided) {
        // Standing down: the deterministic tier decides and spends, and the record reports
        // what it did rather than what was asked for.
        rc = mod_base_hurry();
        chosen = rc ? "hurry:now" : "hurry:none";
        spent = rc ? credits_before - plr.energy_credits : 0;
    }

    na_buf_printf(&w, ",\"tier\":\"%s\",\"applied\":\"%s\"", tier, how);
    na_buf_printf(&w, ",\"applied_item\":\"%s\"", chosen);
    na_buf_printf(&w, ",\"credits_spent\":%d", spent);
    na_buf_printf(&w, ",\"energy_reserves\":%d", plr.energy_credits);
    if (detail[0]) {
        na_buf_puts(&w, ",\"fallback_reason\":\"");
        na_buf_escaped(&w, detail);
        na_buf_puts(&w, "\"");
    }
    na_buf_puts(&w, "}");
    na_log_record(&w);
    na_buf_free(&w);
    return rc;
}

/*
The action-space audit: does every option we offer survive the path a real answer takes?

An offered id goes emitter -> string -> parser -> apply gate. Each audit below walks that whole
path for every option on a surface, rather than asking one predicate about itself.

Mismatches are listed, not just counted, and the list is capped — an audit that printed four
hundred broken ids would be as unreadable as one that printed none. When the cap bites it says
so in the record, because a truncated list that looks complete is worse than an obvious one.
*/
static const int NA_AUDIT_LIST_CAP = 12;

struct NaAudit {
    int offered;
    int rejected;      // offered, but the gate that applies it would refuse
    int hidden;        // the gate would accept it, but it was never offered
};

static void na_audit_open(NaBuf* w, const char* surface_id, int faction_id) {
    na_buf_printf(w, "{\"surface_id\":\"%s\",\"engine\":\"thinker\"", surface_id);
    na_buf_printf(w, ",\"event\":\"audit\",\"turn\":%d,\"faction_id\":%d", *CurrentTurn, faction_id);
}

static void na_audit_close(NaBuf* w, const NaAudit* a) {
    na_buf_printf(w, ",\"offered\":%d,\"rejected\":%d,\"hidden\":%d}",
                  a->offered, a->rejected, a->hidden);
    na_log_record(w);
}

/*
base.production. The predicates match by construction, so what this really exercises is the id
encoding — in particular the facility negation, where the emitter writes a positive id and the
parser stores it negated. A sign error there offers the brain an id that parses to a different
item entirely, and every predicate on both sides would still agree.
*/
static int na_audit_base_production(int base_id) {
    BASE& base = Bases[base_id];
    const int faction_id = base.faction_id;
    NaAudit a = {0, 0, 0};
    NaBuf w;
    na_buf_init(&w);
    na_audit_open(&w, "base.production", faction_id);
    na_buf_printf(&w, ",\"base_id\":%d", base_id);
    na_buf_puts(&w, ",\"rejected_ids\":[");
    int listed = 0;

    for (int pass = 0; pass < 2; pass++) {
        const int last = pass == 0 ? MaxProtoNum - 1 : SP_ID_Last;
        for (int id = pass == 0 ? 0 : 1; id <= last; id++) {
            const bool offered = pass == 0
                ? (mod_veh_avail(id, faction_id, base_id) && can_build_unit(base_id, id))
                : (mod_facility_avail((FacilityId)id, faction_id, base_id, 0)
                   && can_build(base_id, id));
            char action_id[64];
            snprintf(action_id, sizeof(action_id), pass == 0 ? "unit:%d" : "facility:%d", id);
            int item = 0;
            const bool parses = na_parse_item_id(action_id, &item);
            const bool accepted = parses && na_item_is_legal(base_id, item);

            if (offered) {
                a.offered++;
                if (!accepted) {
                    a.rejected++;
                    if (listed < NA_AUDIT_LIST_CAP) {
                        if (listed++) { na_buf_puts(&w, ","); }
                        na_buf_printf(&w, "\"%s\"", action_id);
                    }
                }
            } else if (accepted) {
                a.hidden++;
            }
        }
    }
    na_buf_puts(&w, "]");
    if (a.rejected > listed) {
        na_buf_printf(&w, ",\"rejected_ids_truncated\":%d", a.rejected - listed);
    }
    na_audit_close(&w, &a);
    na_buf_free(&w);
    return a.rejected + a.hidden;
}

static int na_audit_faction_tech(int faction_id) {
    NaAudit a = {0, 0, 0};
    NaBuf w;
    na_buf_init(&w);
    na_audit_open(&w, "faction.tech", faction_id);
    na_buf_puts(&w, ",\"rejected_ids\":[");
    int listed = 0;

    for (int id = 0; id < MaxTechnologyNum; id++) {
        const bool offered = tech_avail(id, faction_id);
        char action_id[64];
        snprintf(action_id, sizeof(action_id), "tech:%d", id);
        int tech_id = 0;
        const bool accepted = na_parse_tech_id(action_id, &tech_id)
                              && na_tech_is_legal(faction_id, tech_id);
        if (offered) {
            a.offered++;
            if (!accepted) {
                a.rejected++;
                if (listed < NA_AUDIT_LIST_CAP) {
                    if (listed++) { na_buf_puts(&w, ","); }
                    na_buf_printf(&w, "\"%s\"", action_id);
                }
            }
        } else if (accepted) {
            a.hidden++;
        }
    }
    na_buf_puts(&w, "]");
    if (a.rejected > listed) {
        na_buf_printf(&w, ",\"rejected_ids_truncated\":%d", a.rejected - listed);
    }
    na_audit_close(&w, &a);
    na_buf_free(&w);
    return a.rejected + a.hidden;
}

/*
faction.se — the audit that can actually find something.

The action space and the apply gate reason about legality differently here, and the difference
is not a refactor away: the action space filters on the model in force, the prerequisite tech
and the faction's forbidden value, while society_avail is the engine's own answer to the same
question. Any disagreement is a real defect in one of them, and which one is not obvious from
here — so both directions are reported separately rather than summed.
*/
static int na_audit_faction_se(int faction_id) {
    const int* current = &Factions[faction_id].SE_Politics;
    NaAudit a = {0, 0, 0};
    NaBuf w;
    na_buf_init(&w);
    na_audit_open(&w, "faction.se", faction_id);
    na_buf_puts(&w, ",\"rejected_ids\":[");
    int listed = 0;
    int hidden_listed = 0;
    char hidden_ids[512];
    hidden_ids[0] = '\0';

    for (int f = 0; f < MaxSocialCatNum; f++) {
        for (int m = 0; m < MaxSocialModelNum; m++) {
            // The action space's own three exclusions, in the order na_build_faction_se applies
            // them. Kept literal rather than factored out: the point is to compare what that
            // emitter actually does against the engine, and a shared helper would make the two
            // agree by construction and test nothing.
            const int preq = SocialField[f].soc_preq_tech[m];
            const bool offered =
                m != current[f]
                && (preq < 0 || has_tech(preq, faction_id))
                && !(MFactions[faction_id].soc_opposition_category == f
                     && MFactions[faction_id].soc_opposition_model == m);

            char action_id[64];
            snprintf(action_id, sizeof(action_id), "se:%d:%d", f, m);
            int pf = -1;
            int pm = -1;
            const bool accepted = na_parse_se_id(action_id, &pf, &pm)
                                  && society_avail(pf, pm, faction_id);

            if (offered) {
                a.offered++;
                if (!accepted) {
                    a.rejected++;
                    if (listed < NA_AUDIT_LIST_CAP) {
                        if (listed++) { na_buf_puts(&w, ","); }
                        na_buf_printf(&w, "\"%s\"", action_id);
                    }
                }
            } else if (accepted) {
                a.hidden++;
                if (hidden_listed < NA_AUDIT_LIST_CAP) {
                    const size_t used = strlen(hidden_ids);
                    snprintf(hidden_ids + used, sizeof(hidden_ids) - used, "%s\"%s\"",
                             hidden_listed ? "," : "", action_id);
                    hidden_listed++;
                }
            }
        }
    }
    na_buf_puts(&w, "]");
    if (a.rejected > listed) {
        na_buf_printf(&w, ",\"rejected_ids_truncated\":%d", a.rejected - listed);
    }
    // Reported separately from `rejected`: an offered option the engine refuses is an illegal
    // move waiting to happen, a legal option never offered is a choice the brain cannot make.
    // Same count, opposite defects.
    na_buf_printf(&w, ",\"hidden_ids\":[%s]", hidden_ids);
    if (a.hidden > hidden_listed) {
        na_buf_printf(&w, ",\"hidden_ids_truncated\":%d", a.hidden - hidden_listed);
    }
    na_audit_close(&w, &a);
    na_buf_free(&w);
    return a.rejected + a.hidden;
}


int na_audit(int faction_id) {
    if (faction_id <= 0 || faction_id >= MaxPlayerNum) {
        return 0;
    }
    int bad = 0;
    bad += na_audit_faction_tech(faction_id);
    bad += na_audit_faction_se(faction_id);
    for (int i = 0; i < *BaseCount && i < MaxBaseNum; i++) {
        if (Bases[i].faction_id != faction_id) {
            continue;
        }
        bad += na_audit_base_production(i);
        // base.hurry is deliberately absent. Its action space and its apply gate now call one
        // function (na_hurry_terms), so they cannot disagree and a check would be a check that
        // can never fail — which reads as coverage while testing nothing. The surfaces audited
        // here are the ones where two pieces of code independently decide what is legal.
    }
    return bad;
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
    "audit <faction_id>" — check every offered option against the gate that would apply it.

    The divergence check (na_verify_base_production) is reactive: it notices after a choice was
    dropped, in a real game, once. This is the proactive half — walk the whole action space now
    and find the options that WOULD be refused, before a decision rides on one.

    Deliberately does not apply anything. Attempting each option would spend credits, retarget
    research and hurry production, so an audit would cost more than the bug it was looking for;
    and it would only ever test the option the engine happened to accept first. Every check here
    is a predicate, so the audit is free and total rather than expensive and partial.

    What it actually tests is the PIPELINE, not one predicate against itself. For three surfaces
    the action space and the apply gate call the same engine function, so comparing them is a
    tautology — but the id makes a round trip in between, formatted into a string by the emitter
    and read back by the parser, and those are separate code. An emitter that writes
    "facility:%d" where the parser negates, an off-by-one in a bound, a range the parser rejects:
    all of that lives between two identical predicates and none of it shows up in either.

    faction.se is the one where the predicates genuinely differ. The action space hand-rolls
    three exclusions — the model already in force, an unresearched prerequisite, the faction's
    forbidden value — while the apply gate binds on society_avail. game-surface.md says those
    "are supposed to agree", which is an assumption, and this is the only thing that checks it.
    Both directions matter and mean different things: offered-but-refused is an illegal move
    waiting to happen, refused-but-offered is a legal option being hidden from the brain.
    */
    if (strncmp(line, "audit ", 6) == 0) {
        int fid = -1;
        if (sscanf(line + 6, "%d", &fid) != 1 || fid <= 0 || fid >= MaxPlayerNum) {
            na_cmd_result("audit", "expected: audit <faction_id>", false);
            return;
        }
        const int bad = na_audit(fid);
        char detail[160];
        snprintf(detail, sizeof(detail), "faction=%d mismatches=%d (see %s)",
                 fid, bad, NA_LOG_PATH);
        na_cmd_result("audit", detail, bad == 0);
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

    /*
    "observe-gov <base_id>" — the base.governor_config probe.

    Serialiser only. governor_priorities() writes to the WItem it is handed and
    touches no game state, so resolving the weights here costs nothing and cannot
    change the base, which is what makes this safe to run mid-game at any time.

    Emits whether or not the deterministic tier is enabled, on purpose: the case
    worth being able to inspect is precisely the one where it is OFF and the base
    is running the flat all-ones weighting, because that is the state the surface
    exists to expose.
    */
    if (strncmp(line, "observe-gov ", 12) == 0) {
        int base_id = -1;
        if (sscanf(line + 12, "%d", &base_id) == 1
        && base_id >= 0 && base_id < *BaseCount) {
            BASE& b = Bases[base_id];
            /*
            An AI-owned base has no governor config to report: gov_config() hands
            it ~0u and governor_priorities takes the faction-character branch, so
            probing one would write no line. Say so rather than return a success
            the log cannot corroborate.
            */
            if (!is_human(b.faction_id)) {
                char detail[96];
                snprintf(detail, sizeof(detail),
                    "base_id=%d is AI-owned (faction %d); surface is player-only",
                    base_id, b.faction_id);
                na_cmd_result("observe-gov", detail, false);
                return;
            }
            WItem Wgov = {};
            na_gov_probing = 1;
            governor_priorities(b, Wgov);
            na_gov_probing = 0;
            char detail[96];
            snprintf(detail, sizeof(detail), "base_id=%d of %d", base_id, *BaseCount);
            na_cmd_result("observe-gov", detail, true);
        } else {
            char detail[96];
            snprintf(detail, sizeof(detail), "need 0 <= base_id < %d", *BaseCount);
            na_cmd_result("observe-gov", detail, false);
        }
        return;
    }

    /*
    "observe-abandon <base_id>" — the base.abandon probe.

    Serialiser only: it asks na_should_abandon_base what the tier WOULD answer and
    records that, without resetting production and without completing any pod. The
    verdict comes from the real function rather than a copy of the rule.

    Unlike the in-game hook this does not require a colony pod to be queued, and
    that is deliberate — the surface fires so rarely that waiting for the natural
    conditions is exactly the unverifiability the probes exist to fix. It reports
    item_id -1 when nothing is queued, so a reader can tell a hypothetical from a
    live decision.
    */
    if (strncmp(line, "observe-abandon ", 16) == 0) {
        int base_id = -1;
        if (sscanf(line + 16, "%d", &base_id) == 1
        && base_id >= 0 && base_id < *BaseCount) {
            BASE& b = Bases[base_id];
            int item = b.item();
            int pod = (item >= 0 && item < MaxProtoNum
                && Units[item].plan == PLAN_COLONY) ? item : -1;
            na_abandon_probing = 1;
            na_observe_base_abandon(base_id, na_should_abandon_base(base_id) ? 1 : 0, pod);
            na_abandon_probing = 0;
            char detail[96];
            snprintf(detail, sizeof(detail), "base_id=%d pod=%d", base_id, pod);
            na_cmd_result("observe-abandon", detail, true);
        } else {
            char detail[96];
            snprintf(detail, sizeof(detail), "need 0 <= base_id < %d", *BaseCount);
            na_cmd_result("observe-abandon", detail, false);
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
