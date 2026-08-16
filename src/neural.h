#pragma once

#include "main.h"
#include "na_http.h"

/*
Neural Amplifier bridge.

Everything the orchestrator needs to see a decision point, and nothing more. The
adapter stays thin on purpose: it serializes engine state and applies a returned
choice. It does not decide, and it does not build the decision record — that
belongs to the orchestrator, which owns the record of truth.

Every surface emits the contract (docs/contract.md) directly rather than a shape
of its own. Three surfaces are still observe-only: they emit a world view, record
what the deterministic tier chose, and change nothing. base.production is the one
that closes the loop.

A faction that is not LLM-routed never reaches any of this code.
*/

/*
A1: decide what this base builds, and return the item to build.

The one function here that changes the game. Posts the world view to the
orchestrator and applies the choice that comes back, subject to two gates that
make invariant 1 structural rather than hopeful: the id must parse as one of ours,
and the item must pass the engine's own availability tests for this base.

Returns `native_choice` unchanged on every failure — unreachable orchestrator,
timeout, malformed reply, unparseable or illegal id. That is invariant 9: a slow,
broken or over-budget model costs a decision, never a turn.

Called once per base-turn on the network. mod_base_build fires MORE THAN ONCE per
base per turn — mod_base_reset is hooked at eleven engine call sites
(patch.cpp:859-869), and measured play shows two calls per base per turn returning
*different* choices — so the answer is cached by turn and replayed for the
remaining calls. Without that, one build decision would cost several model calls
and the last caller would silently win.
*/
int na_decide_base_production(int base_id, int native_choice, int has_gov);

/*
Check that the engine kept the item that was applied, after it has been applied.

Call once per base-turn, immediately after mod_base_change. Silent when the
engine agrees, which is the normal case; on disagreement it writes a compact
`"event":"divergence"` record naming both items.

This is deliberately NOT another legality gate. The gates test what we thought
to check; this tests what actually happened, so it is the only mechanism that
catches a rule nobody encoded — the case where the decision record claims a
choice the base is not building and nothing else can tell you.

Does nothing for a base the LLM tier did not decide this turn.
*/
void na_verify_base_production(int base_id);

/*
Audit every offered option against the gate that would apply it, for one
faction and all its bases. Returns the total mismatch count; 0 means every
surface agrees with itself.

The proactive counterpart to na_verify_base_production, which only notices a
dropped choice after a real decision rode on one. This walks the whole action
space and finds the options that WOULD be refused, before that happens.

Applies nothing and spends nothing — every check is a predicate, which is what
makes auditing every option affordable. Attempting them would cost credits,
research and production, and would still only test whichever option the engine
accepted first.

Writes one `"event":"audit"` record per surface into the observation log,
listing the mismatching ids rather than only counting them (capped, and the
record says when the cap bit).
*/
int na_audit(int faction_id);

/*
Emit one base.production world view without deciding anything.

The `observe <base_id>` command-channel probe. Side-effect free by construction:
no network call, no application, no call_seq consumed and no per-turn cache
primed — so running it never changes what the base goes on to build.

It exists because a production decision fires on the engine's schedule and
in-game input cannot be driven at all, which would otherwise make the whole
serializer unverifiable without playing until a decision happened to occur.
*/
void na_observe_base_production(int base_id, int native_choice, int has_gov);

/*
Emit one base.governor_config observation: the build priorities in force for a
player-owned base, and where they came from.

The first of the 21 NO_AI_PATH surfaces to carry a deterministic tier. `applied`
is 1 when na_governor_policy (plan.cpp) supplied the weights, 2 when the player's
own GOV_PRIORITY_* bits did, and 0 when neither did and the base is running the
flat all-ones weighting. Deduplicated to one line per base per turn.

Observation only — it reports the weights, it does not choose them.
*/
void na_observe_base_governor_config(int base_id, int applied, int growth,
                                     int tech, int wealth, int power, int fight);

/*
Emit one base.abandon observation: a size-1 base holding a finished colony pod,
where completing it spends the last population and destroys the base.

`abandon` is the deterministic tier's answer (1 = spend the base, 0 = keep it and
re-select production). `item_id` is the pod that prompted the question. Records
the growth numbers the answer turns on, so a reader can check the reasoning
rather than take the verdict.

Observation only — na_should_abandon_base (base.cpp) decides.
*/
void na_observe_base_abandon(int base_id, int abandon, int item_id);

/*
Emit one base.hq_escape observation: the headquarters base is being captured and
the HQ can move to another base for 1000 energy credits.

`dest_base_id` is the destination the engine already scored and chose; `relocate`
is the tier's answer. Records the reserve and base count because those are what
make the answer arguable at all, even though the tier currently always says yes.

Observation only — na_should_escape_hq (base.cpp) decides.
*/
void na_observe_base_hq_escape(int base_id, int dest_base_id, int relocate);

/* Emit the native unit.odp_attack choice. applied is false for the probe. */
void na_observe_unit_odp_attack(
    int faction_id, int faction_id_tgt, int target_id, int base_id, int applied);
void na_observe_diplo_tech_trade(
    int faction_id, int faction_id_tgt, int tech_id, int price, int accept, int applied);
void na_observe_diplo_energy_loan(int faction_id, int faction_id_tgt,
    int score, int amount, int turns, int payment, int available_income, int accept, int applied);
void na_observe_diplo_base_swap(int faction_id, int faction_id_tgt,
    int base_id, int cost, int committed_hurry, int accept, int applied);

/*
Load the -na-autoload savegame, once, as soon as the game is idle at the menu.

Called from the GUI timer (mod_blink_timer) because that is the only thing that
runs while the main menu is waiting for a human. terranx.exe boots to a menu and
blocks on input; there is no "game started" hook to attach to, because no game has
started.

Loading is Thinker's own mod_load_daemon, which reads the file and rebuilds all
game state. Clearing GameHalted is what resumes the engine's loop afterwards —
together they are the menu-to-session transition the engine normally performs
inside load_game, which is not usable here because it addresses saves by slot.

No-op when na_autoload is empty, and no-op after the first attempt whether or not
it succeeded. A failed autoload must leave a usable main menu rather than retry
forever.
*/
/*
Emit one faction.tech observation: which technology to research next.

The second LLM-tier surface, and the first where a language model should beat the
deterministic tier rather than merely match it. Thinker picks with weighted tables
(CTech.AI_growth / AI_tech / AI_wealth / AI_power); it cannot reason "we are boxed in on a
small continent, so a naval-plus-ecology path suits both our terrain and our character".
That is a path argument over many turns, which is exactly what weight tables cannot do.

Fires once per tech completion - every five to ten turns - so it can afford a much richer
world view than base.production, which fires per base per turn.

surface_id is "faction.tech", matching the frozen registry in surfaces.py. Not
"tech.choose": renaming a surface invalidates every recorded run.
*/
void na_observe_faction_tech(int faction_id, int native_choice);

/*
Decide which technology to research next: post the world view, apply the answer.

Returns the tech id to research. `native_choice` unchanged on every failure —
unreachable orchestrator, timeout, malformed reply, unparseable or illegal id —
so a broken model costs a research choice and never a turn (invariant 9).

Unlike base.production this needs no per-turn cache. mod_tech_selection fires
only while tech_research_id < 0 (tech.cpp:233), so the engine asks once per
research cycle rather than repeatedly per turn, and one question yields one
record by construction.

The decision also binds for longer than a build does: research commits the
faction until the tech completes, which is why the world view carries
research_state and turns_to_complete and why this surface is worth deciding at
all.
*/
int na_decide_faction_tech(int faction_id, int native_choice);

/*
Emit one faction.se observation: the social-engineering choice.

Low frequency, high leverage, and a strong LLM fit — it is a values trade-off with faction
character attached, which is what a weight table handles worst.

The engine models this as "change one field to one model, if we can afford the upheaval",
so the action space is the legal (field, model) pairs plus the option to change nothing.
Prohibited and un-researched models are excluded from the space rather than merely flagged:
the action space BINDS, grounding only advises, and an option the brain must not pick should
not be offered.

LIMITATION worth knowing before relying on this: mod_social_ai returns immediately for human
factions, so this never fires in the recommended Mode B+ configuration (a human slot with
manage_player_bases). It covers AI factions only. Routing a human slot's SE decision needs a
different hook — the choice is made through the UI, not here.
*/
void na_observe_faction_se(int faction_id, int field, int model, int cost);

/*
Decide one social-engineering change: post the world view, apply the answer.

In/out. `field` and `model` carry the deterministic tier's choice in and the
decision out — `field < 0` means "no change", which is a real answer both ways.
On any failure they come back exactly as they went in, so a broken model costs a
social choice and never a turn (invariant 9).

Decides and records only. It does NOT debit energy_credits or touch
SE_Politics_pending: the caller applies, because faction.cpp already does that
correctly for whatever choice it is handed, and debiting in two places is how a
faction pays twice or pays nothing.

Must be called BEFORE the caller applies anything. The upheaval cost is computed
against the whole proposed category set, so the brain's choice has to be costed
as its own candidate rather than inheriting the deterministic tier's figure —
and affordability is checked here so the record cannot claim a change the
faction could not pay for.
*/
void na_decide_faction_se(int faction_id, int* field, int* model);

/*
Emit one base.hurry observation: whether to spend energy credits to finish production now.

A binary decision with a price, which makes it a good fit for the LLM tier — the question is not
"can we" but "is this the best use of the reserve", and that is a judgement about the whole
faction's position rather than a rule lookup.

Called from a wrapper around mod_base_hurry rather than from inside it. That function has five
separate return points and mutates base state through hurry_item() on the way out, so the only
place both the decision AND the pre-decision numbers are available is outside it.
*/
void na_observe_base_hurry(int base_id, int item, int minerals_before, int credits_before,
                           int native_hurried);

/*
Install the in-game dialog hook — invariant 7, na-4lr.

`popp` is a function POINTER the engine binds and this fork calls through, so writing a wrapper
into it intercepts every `popp(...)` in Thinker's source at once, with no per-site patch and no
binary detour. Idempotent; call it once at init.

Does NOT reach dialogs the engine raises from its own code — those call the real function
directly. That needs per-call-site addresses, which needs the game binary.

Never suppresses: the engine's return is passed through unchanged on every path.
*/
void na_install_dialog_hook();

/*
Record a dialog the BRAIN answered, tier llm / applied llm.

Distinct from the observation and auto-answer records because three different parties can answer
a dialog — the engine, the harness, and the model — and a log that cannot say which is not a
record of what happened.
*/
void na_observe_dialog_routed(const char* file, const char* label, int chosen);

/*
Record a dialog that was AUTO-ANSWERED on the game's behalf, flagged as such.

A separate entry point from na_observe_dialog rather than a parameter, so the ordinary path
cannot accidentally claim a dialog was auto-answered and so a log grep for who answered is
unambiguous.
*/
void na_observe_dialog_auto(const char* file, const char* label, int chosen);

/*
Record one dialog as an observation. Exposed for the `observe-dialog` probe; the hook calls it
itself when `na_dialog_observe` is set.

`chosen` is the button index popp returned. A dialog the table does not know is still recorded,
marked `mapped:false`, so the real inventory can be built from a real game rather than guessed
in the table.
*/
void na_observe_dialog(const char* file, const char* label, int chosen);

/*
Record the base.workers + base.specialists allocation (na-yd4).

TWO REGISTRY IDS, ONE DECISION: mod_base_yield's greedy tile loop and the specialist count are
one answer — leftovers from the loop ARE the specialists. One record carries both.

NOT a world view. The contract's action_space is pick-one and an allocation over 21 tiles does
not fit that shape, so this is a compact outcome record and neither id enters OBSERVED.

Checks conf.na_yield_observe itself; the call site stays one line.
*/
void na_observe_base_yield(int base_id);

/*
Record a base.name naming event (na-yd4).

NOT a world view and NOT a decision record: no action space, no tier, no applied. The candidate
names live in files read inside mod_name_base, so enumerating them would mean re-reading those
files per base founding to build a list nobody applies.

The payload is `source` — which of the four pools the name came from. "sector_fallback" means
every named pool was exhausted, a content problem that otherwise surfaces only as bases called
"Sector 41".

Checks conf.na_name_observe itself, so the four call sites in mod_name_base stay one line each.
*/
void na_name_base_observed(int faction_id, const char* name, bool sea_base, const char* source);

/*
Record a base.defend_goal decision — how many defenders a base should hold (na-yd4).

OBSERVATION ONLY. move_upkeep already assigns the tier by percentile across the faction's whole
base list.

`score` and `cohort` are both required because this decision is RELATIVE: a base is tier 5
because it is in the top sixteenth of THIS faction's bases, so the same base with the same score
is a different tier in a bigger empire. The tier alone cannot be compared across turns.
*/
void na_observe_base_defend_goal(int base_id, int goal, int score, int cohort);

/*
Record a faction.tech_steal decision — which technology is taken (na-yd4).

OBSERVATION ONLY. mod_tech_pick already chooses; this writes it down with the real stealable
set: every tech the TARGET holds that we do not. That is deliberately not the research menu —
`tech_avail` answers a different question and would offer a plausible list of the wrong options.

`is_steal` distinguishes a probe team's deliberate operation from the acquisition that comes
free with a base capture. `tech_id` is expected post-normalisation, so 9999 means "nothing to
take" and is emitted as tech:none.
*/
void na_observe_faction_tech_steal(int faction_id, int faction_id_tgt, int tech_id, int is_steal);

/*
Record a base.project decision — which secret project a base starts (na-yd4).

OBSERVATION ONLY. find_project already chooses; this writes down what it chose along with the
engine's own `facility_score` for every buildable project under the SAME governor weights the
chooser used — which is why Wgov is passed in rather than reconstructed.

`chosen` has three shapes and they are kept distinct: a negated facility id (a project, or the
chooser's Skunkworks / Subspace Generator prerequisite answers), a positive missile unit id, or
the GOV_NONE sentinel — the last translated by the caller into `declined`, since that sentinel
is file-local to build.cpp.
*/
void na_observe_base_project(int base_id, int chosen, bool declined, WItem& Wgov);

/*
Record a base.satellite decision — which orbital this base builds (na-yd4).

OBSERVATION ONLY. find_satellite already chooses; this writes down what it chose and, unusually
for this bucket, the real alternatives: four satellite types with their own availability, built
count and faction goal.

`chosen` is find_satellite's return and `declined` is the caller's translation of its GOV_NONE
sentinel, which is file-local to build.cpp — passed in rather than re-spelled here, so there is
one definition of "the chooser picked nothing".

-FAC_AEROSPACE_COMPLEX means "build the prerequisite first" and is named separately rather than
folded into a decline: a base working toward orbit is not a base that turned orbit down.
*/
void na_observe_base_satellite(int base_id, int chosen, bool declined);

/*
Record an econ.corner_market decision — cornering the global energy market (na-yd4).

OBSERVATION ONLY, and the highest-stakes surface in that bucket: a move toward economic
victory, AI-only, firing at most a handful of times per game.

`cost` is corner_market()'s own return and `credits_before` is read before the deduction, so
the record shows the reserve the decision was actually made against rather than what was left
afterwards.
*/
void na_observe_corner_market(int faction_id, int cost, int credits_before, bool cornered);

/*
Record a council.call decision — convening the Planetary Council (na-yd4).

OBSERVATION ONLY. call_council decides internally and returns nothing useful, so `called` is a
STATE TRANSITION observed by the caller: STATE_COUNCIL_HAS_CONVENED off before, on after.
`eligible` is can_call_council's own answer, passed in rather than re-derived.
*/
void na_observe_council_call(int faction_id, bool eligible, bool called);

/*
Record a base.staple decision — nerve stapling, na-yd4's first surface.

OBSERVATION ONLY. `consider_staple` (build.cpp) already decides, and the native path being the
fallback is what makes this bucket safe to instrument from the first record: invariant 9 needs
nothing built first.

Called only when consider_staple's eligibility gate opened, so every record is a decision that
was genuinely available rather than a base where stapling was never on the table. `stapled` is
the engine's own answer, passed in rather than re-derived.
*/
void na_observe_base_staple(int base_id, bool stapled);

/*
Record a base.retool decision — the production switch that would cost banked minerals.

OBSERVATION ONLY, and deliberately so. Unlike the other NO_AI_PATH surfaces this one already
HAS its deterministic tier: select_build threads a retool category through the production
chooser and push_item penalises a category crossing (na-lnv). What was missing was the record,
without which coverage cannot see the surface and na-6db has no baseline to A/B against.

Called from the select_build wrapper (build.cpp), which is the one place holding both halves:
the item the base was producing and the item the chooser picked. native_choice is their
category comparison — the engine's own answer, and the baseline the record exists for.
*/
void na_observe_base_retool(int base_id, int prev_id, int chosen);

/*
Decide whether to hurry: post the world view, spend or hold, return what the
engine hook should return (1 hurried, 0 did not).

Unlike the other three this OWNS the fallback. mod_base_hurry decides and spends
in one pass, so there is no point after it at which a different answer can still
be given — this runs first and calls it only when standing down. That also keeps
the one-record-per-decision invariant: a caller handling the fallback itself
would either write a second record or build the world view twice to avoid it.

Purchases go through hurry_item, which does the credit debit and the mineral
credit together. Gated on the engine's own can_hurry_item, a positive
hurry_cost, and sufficient reserves, so an unaffordable order is refused rather
than partially applied.

`minerals_before` and `credits_before` are the PRE-decision numbers, captured by
the caller before anything is spent.
*/
int na_decide_base_hurry(int base_id, int item, int minerals_before, int credits_before);

void na_autoload_tick();

/*
True when the command line says nobody is at the keyboard: -na-headless, or
-na-autoload, which implies it. Answered from the raw command line, so it is
usable from DllMain before cmd_parse has populated anything.

The one thing that changes in an unattended run, and the only thing an
interactive run pays for it, is one comparison per would-be dialog.
*/
bool na_headless();

/*
Drop-in MessageBoxA that a headless run survives.

Interactive: forwards, unchanged. Headless: writes the text to the observation
log and stderr instead of showing it, then returns IDOK for a box that only
offered OK — so the caller's own control flow, which is where fatality is already
decided, still runs. A box that asks a real question is not answered here at all;
the run stops instead. See the definition for why suppression is deliberately not
allowed to know which sites are fatal.
*/
int na_message_box(HWND hwnd, const char* text, const char* caption, UINT type);

/*
Exit the process once -na-exit-turn turns are complete. No-op when the flag is
unset. Called as the first statement of mod_turn_upkeep, which is the only site
at which the finished turn's records and autosave are all on disk and the next
turn has not begun — the definition argues that at length, because the choice of
site is what makes the resulting artifacts trustworthy.
*/
void na_exit_turn_check();

/*
Tell the orchestrator what the coming turn is expected to ask about.

Called from the first statements of mod_turn_upkeep, where *CurrentTurn still names the turn that
just finished — so the forecast is built from the board as it stood at the end of it. That is
what makes it a FORECAST and not a worklist: a base can be captured or finish a project before
its decision would have been raised, and then it never is.
*/
void na_announce_turn();

/*
The faction economy totals are zeroed and re-accumulated per base inside
mod_production_phase, and mod_base_build fires inside that window. These bracket it so
na_write_metrics can OMIT a half-summed total rather than publish it as income (na-an6).
*/
/*
Emit one econ.energy_sliders observation: how the faction split its 10 energy points across
economy, labs and psych.

The first of the 27 "native path, safe fallback" surfaces to be instrumented (na-yd4), chosen by
decision-inputs.md's own rule -- low frequency, high stakes. It fires once per faction-turn and
sets the ratio every base's energy is divided by, so it is the widest-reaching number in the game
that no LLM has ever been asked about.

OBSERVATION ONLY. mod_allocate_energy decides; this records what it decided and what else was
legal. Applying here would need an apply path and validation, which is the second half of yd4.
*/
void na_observe_econ_energy_sliders(int faction_id);

void na_accumulate_begin(int faction_id);
void na_accumulate_end(int faction_id);

/*
True once the -na-autoload sequence has finished, however it finished, and when
no autoload was requested at all.
*/
bool na_autoload_settled();

/*
End our own turn when the turn number has not moved for -na-auto-turn seconds of
a live session, so an unattended run advances at all. No-op unless the flag is
set. Called from the window procedure beside na_autoload_tick — see neural.cpp for
why a stall timer, why the engine's own Console_end_my_turn, and what happens if
ending a turn raises a modal.
*/
void na_auto_turn_tick();

/*
Poll the command channel and act on it. Called from the window procedure.

Why a file and not input injection: terranx.exe reads the mouse through DirectInput
and runs inside a Wine virtual desktop, so synthesised clicks do not reach it —
measured, both via window messages and via XTEST with warped coordinates. And under
XWayland an external screenshot of the root window returns solid black, because each
X client is composited separately.

Both problems disappear in-process. The DLL already owns the window handle and can
BitBlt its own client area, which needs no compositor cooperation, no portal
permission, and no X server at all — so it works identically under Xvfb. A file is
the simplest channel an outside agent can drive with no IPC setup.

hwnd comes from the window procedure rather than a FindWindow call, so it is always
the right window and needs no title matching.
*/
void na_command_tick(void* hwnd);

/*
Start the input worker thread (idempotent). Input lives on its own thread because the
window procedure - and therefore na_command_tick - stops being called while a modal
dialog runs its own nested message pump, which is precisely when input is needed.

Reads the "na-input" file; handles click / dclick / key / text by PostMessage only.
PostMessage is thread-safe and just queues, so this never touches engine state. Anything
that does must stay on na_command_tick's path.
*/
void na_input_start(void* hwnd);
