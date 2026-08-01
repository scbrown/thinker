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

void na_autoload_tick();

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
