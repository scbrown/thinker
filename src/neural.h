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
