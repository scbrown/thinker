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
