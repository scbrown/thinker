#pragma once

#include <string>

/*
Neural Amplifier configuration, kept in its own file on purpose.

These 22 settings used to live as a 188-line block inside `struct Config` in
main.h — and main.h is touched by 244 of upstream's 435 commits, more than half.
Parking our largest single block in upstream's most-edited header was a standing
invitation to a conflict that had nothing to do with either side's intent.

Field names drop the `na_` prefix because the member carrying them is already
called `na`: it is conf.na.governor_policy, not conf.na.na_governor_policy. The
thinker.ini KEYS are unchanged and still read `na_governor_policy` — they are a
user-facing contract, and renaming them would silently reset every configured
option to its default. na_option_handler() in neural.cpp maps one to the other.
*/

struct NaConfig {
    /*
    Neural Amplifier: bitmask of faction ids whose decisions are routed to the
    LLM orchestrator instead of Thinker's native AI. 0 disables the bridge, so
    an unconfigured build behaves exactly like stock Thinker.
    Bit N = faction N, e.g. llm_factions=2 routes faction 1 only.
    */
    int llm_factions = 0;
    /*
    Neural Amplifier: how long a decision may wait on the orchestrator before the
    engine's own answer is applied instead (invariant 9 — the game never stalls
    waiting on the brain). Bounds the whole exchange, not each stage.

    2500ms is chosen to cover a Haiku call on a warm connection and to be short
    enough that a dead orchestrator costs a noticeable pause rather than a hung
    turn. A base is asked several times per turn but only the first call reaches
    the network, so this is the per-base-turn cost, not the per-call cost.
    */
    int llm_timeout_ms = 2500;
    /*
    Neural Amplifier: deterministic tier for base.governor_config, one of the 21
    surfaces the native AI never decides (surfaces.NO_AI_PATH). 0 keeps stock
    behaviour, so an unconfigured build is unchanged.

    Applies only to player-owned bases that have set NO governor priority bit --
    see na_governor_policy() in plan.cpp for why that is the whole point.
    */
    int governor_policy = 0;
    /*
    Neural Amplifier: deterministic tier for base.abandon, the second of the 21
    surfaces the native AI never decides (surfaces.NO_AI_PATH). 0 keeps stock
    behaviour, so an unconfigured build is unchanged.

    The surface is narrower than its name: it is the size-1 base that has a colony
    pod ready, where completing the pod spends the last population and destroys the
    base. See na_should_abandon_base() in base.cpp for why the answer is almost
    always no, and why saying no silently is the actual defect.
    */
    int abandon_policy = 0;
    /*
    Neural Amplifier: deterministic tier for base.hq_escape, the third of the 21
    surfaces the native AI never decides (surfaces.NO_AI_PATH). 0 keeps stock
    behaviour, so an unconfigured build is unchanged.

    Unlike the other two this does NOT change the answer — see na_should_escape_hq()
    in base.cpp. It names the answer, records it, and repairs an asymmetry that
    penalised player-owned bases receiving the relocated headquarters.
    */
    int hq_escape_policy = 0;
    /*
    Neural Amplifier: deterministic tier for unit.odp_attack. One conservative
    orbital strike per faction-turn, and only against an existing vendetta.
    0 preserves stock behaviour (AI factions never launch ODP attacks).
    */
    int odp_attack_policy = 0;
    /* Default-off deterministic response for an offered technology purchase. */
    int tech_trade_policy = 0;
    /* Default-off deterministic response for an offered energy loan. */
    int energy_loan_policy = 0;
    /* Default-off deterministic response for a priced base purchase. */
    int base_swap_policy = 0;
    /*
    Neural Amplifier: publish this faction's own bases in every world view, so the
    orchestrator's board guard has entities to evaluate policies over. 0 keeps the
    payload exactly as it was, which is why it is off by default -- the array rides
    on every decision and the world view IS the prompt.

    Own bases only. Publishing another faction's would be an information cheat of
    the same kind the diplomacy fog gate exists to stop, and a quieter one, because
    nothing downstream would flag it.

    The fields are the ENGINE'S OWN numbers, never a derived boolean. "Is this a
    border base" is a judgement with a threshold in it, and a threshold compiled into
    the DLL is one nobody can change without a rebuild; published as defend_range it
    is a fact, and the policy that reads it owns the threshold.
    */
    int board_state = 0;
    /*
    Neural Amplifier: emit a base.retool observation when a production switch would cost
    banked minerals. 0 keeps stock behaviour, so an unconfigured build is unchanged.

    Observation only — this surface's deterministic tier already exists inside select_build,
    which is what na-lnv established. What was missing was the record, and without one the
    surface is invisible to coverage and has no baseline for na-6db to A/B the brain against.
    */
    int retool_observe = 0;
    /*
    Neural Amplifier: record in-game dialogs as decision points (invariant 7, na-4lr). 0 keeps
    stock behaviour.

    Observation only, and it NEVER suppresses — the engine's answer is passed through unchanged
    on every path, including for a dialog the table does not recognise. Invariant 7 is explicit
    that dialogs are decision points to be intercepted, not hidden.
    */
    int dialog_observe = 0;
    /*
    Neural Amplifier: answer one-button NOTICE dialogs in an unattended run (invariant 7,
    na-4lr). 0 keeps stock behaviour.

    HEADLESS ONLY, enforced in code and not merely documented. Answering while a human is there
    would take their decision away — which is what invariant 7 forbids. With nobody there the
    alternative is not "the human decides", it is "the run hangs forever", so this is the same
    policy na_message_box already applies to the Win32 path.

    Never answers a real question and never answers an unrecognised dialog: their button indices
    live in game text files this project does not ship, and picking one would be inventing an
    answer.
    */
    int dialog_auto = 0;
    /*
    Neural Amplifier: route decidable dialogs to the brain (invariant 7, na-4lr). 0 keeps stock
    behaviour.

    Enabling this is currently a NO-OP by design: routing needs each dialog's affirm/decline
    button indices, those live in game text files this project does not ship, and a dialog with
    no mapping is never routed. The mechanism is built and refuses rather than guesses — see
    NaDialogEntry in neural.cpp. dialog-stats reports the refusals, so a run says plainly that
    routing did nothing rather than appearing to work.
    */
    int dialog_route = 0;
    /*
    Neural Amplifier: record base.staple decisions — nerve stapling (na-yd4). 0 keeps stock
    behaviour.

    Observation only. consider_staple already decides and keeps deciding; this writes down what
    it chose. Records only when its eligibility gate opened, so a row is always a decision that
    was actually available.
    */
    int staple_observe = 0;
    /*
    Neural Amplifier: record econ.corner_market and council.call — two AI-only, very
    low-frequency, very high-stakes turn-scope decisions (na-yd4). 0 keeps stock behaviour.

    One flag for both because they sit in the same function, fire on the same cadence, and
    neither is useful without the other when reading a turn: a game where the council convened
    and the market was cornered is a different game from one where only one happened.
    */
    int endgame_observe = 0;
    /*
    Neural Amplifier: record base.satellite — which orbital a base builds (na-yd4). 0 keeps
    stock behaviour.

    Observation only. Recorded even when the chooser declines, because by the time
    find_satellite runs the gate has already opened, so "no orbital this turn" is an answer
    rather than an absent decision.
    */
    int satellite_observe = 0;
    /*
    Neural Amplifier: record base.project — which secret project a base starts (na-yd4). 0
    keeps stock behaviour.

    Observation only, and the richest action space of the bucket: every buildable project with
    the engine's own facility_score under this base's governor weights.
    */
    int project_observe = 0;
    /*
    Neural Amplifier: record faction.tech_steal — which technology a probe team or a base
    capture takes (na-yd4). 0 keeps stock behaviour.

    Observation only. The action space is what the TARGET holds and we do not, which is a
    different set from what we could research.
    */
    int tech_steal_observe = 0;
    /*
    Neural Amplifier: record base.defend_goal — how many defenders a base should hold (na-yd4).
    0 keeps stock behaviour.

    Observation only. Fires once per base per faction-turn, so it is the highest-volume surface
    instrumented so far; the tier is assigned by percentile across the faction's whole base
    list, and the record carries the score and cohort that produced it.
    */
    int defend_goal_observe = 0;
    /*
    Neural Amplifier: record base.name naming events (na-yd4). 0 keeps stock behaviour.

    The payload is which name POOL was used, not the name. "sector_fallback" means every named
    pool was exhausted — a content problem that otherwise shows up only as bases called
    "Sector 41".
    */
    int name_observe = 0;
    /*
    Neural Amplifier: record the base.workers + base.specialists allocation (na-yd4). 0 keeps
    stock behaviour.

    One record for two registry ids, because mod_base_yield answers both at once: the
    specialists are whatever population the tile loop did not consume. Highest-volume record in
    the fork — once per base per recompute — so it is off by default and stays that way.
    */
    int yield_observe = 0;
};

/*
Neural Amplifier: process exit codes for an unattended run.

The harness has to distinguish three outcomes without parsing anything, because
the run it is grading may have produced no parseable output at all. EXIT_FAILURE
is left to exit_fail(), which every fatal path in the mod already uses, so a
distinct code is only needed for the two outcomes this fork adds.

NA_EXIT_UNANSWERABLE is deliberately not EXIT_FAILURE: "the run asked a question"
is an operator-fixable configuration problem, not a broken engine, and a harness
that cannot tell them apart will retry the one that can never succeed.
*/
enum NaExitCode {
    NA_EXIT_TURN_LIMIT = 0,     // -na-exit-turn reached; the run did what it was asked
    NA_EXIT_UNANSWERABLE = 3,   // a dialog asked something no unattended run may answer
};

/*
Neural Amplifier: base URL of the orchestrator that answers /decide.
A std::string rather than a Config member because Config is all scalars.
*/
extern std::string llm_endpoint;

/*
Neural Amplifier: savegame to load automatically at startup, from -na-autoload.
Empty means boot to the main menu as normal.

A path rather than a save slot because the file we want is Thinker's own
saves/auto/Autosave_<year>.sav, which is not addressable as a slot.
*/
extern std::string na_autoload;

/*
Neural Amplifier: stop the process once this many turns have been played, from
-na-exit-turn. Zero — the default — means play until somebody quits, which is the
only sane default for a game and the reason an unattended run needs the flag at
all.

An int rather than a Config member for the same reason as the two above: these
are properties of one launch, not of the mod's configuration, and putting them in
thinker.ini would make an unattended run's terminating condition survive into the
next interactive one.
*/
extern int na_exit_turn;

/*
Neural Amplifier: seconds of a live session with no turn change after which the
run ends its own turn, from -na-auto-turn. Zero — the default — never does.

This is the OTHER thing the human at the keyboard was doing. -na-autoload
replaces the human who starts the game and -na-headless the human who dismisses
its errors, but a loaded save resumes at the player's turn and simply waits, so
an unattended run without this advances no turns at all — which also means
mod_turn_upkeep never runs and -na-exit-turn can never fire. Measured 2026-08-01:
a 7-minute headless session at turn 44 produced no autosave, no decision record
and no exit record, because nothing ever ended turn 44.

A STALL threshold rather than a period, because "the turn has not changed in N
seconds" is an observation about the game and "end a turn every N seconds" is an
assumption about it. The engine spends real time on the other factions' turns and
on its own animations; a periodic timer would fire during those, whereas a stall
timer re-arms whenever the turn number moves and so only ever speaks when nothing
else is happening.
*/
extern int na_auto_turn;

extern int na_enter_arg;
