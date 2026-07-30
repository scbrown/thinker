#pragma once

#include "main.h"

/*
Neural Amplifier bridge.

Everything the orchestrator needs to see a decision point, and nothing more. The
adapter stays thin on purpose: it serializes engine state and (later) applies a
returned choice. It does not decide, and it does not build the decision record —
that belongs to the orchestrator, which owns the record of truth.

A0 milestone: observe only. No HTTP, no blocking, no behaviour change. A faction
that is not LLM-routed never reaches this code at all.
*/

/*
Emit one base.production observation. native_choice is Thinker's own pick, which
remains authoritative until A1 wires the orchestrator's answer back in.

has_gov is recorded because mod_base_build fires MORE THAN ONCE per base per turn:
mod_base_reset is hooked at eleven engine call sites (patch.cpp:859-869 —
bases_reset, base_production, and four BaseWin entry points), and measured play
shows exactly two calls per base per turn returning *different* choices. Without a
discriminator those two lines are indistinguishable, and the orchestrator's
"exactly one decision record per decision" invariant has no way to tell a fresh
decision from a re-evaluation of the same one.
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
