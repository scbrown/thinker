#!/usr/bin/env python3
"""The fork's knob registry mirrors the orchestrator's, and the overlay stays write-only.

src/na_policy.cpp holds the fork's copy of NeuralAmplifier's frozen registry
(orchestrator/src/neural_amplifier/faction_policy.py). Two tables that must agree
drift silently, so this reads both:

  * always: the C++ table against the registry frozen below, which is the
    orchestrator's as of the change that introduced the overlay;
  * when a NeuralAmplifier checkout is beside this one (or NA_DIR names one):
    the C++ table against the live faction_policy.registry().

It also asserts the overlay section of neural.cpp never sends anything: the overlay
is write-only, and a POST from it would be the only way plan state could reach a seat.

    tests/check_na_policy_registry.py
"""

from __future__ import annotations

import os
import re
import runpy
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

#: name -> (type, field, lo, hi, mask) as faction_policy.registry() reports it.
FROZEN = {
    "focus.fight": ("int", "Faction.AI_fight", -1, 1, 0),
    "focus.growth": ("int", "Faction.AI_growth", 0, 1, 0),
    "focus.tech": ("int", "Faction.AI_tech", 0, 1, 0),
    "focus.wealth": ("int", "Faction.AI_wealth", 0, 1, 0),
    "focus.power": ("int", "Faction.AI_power", 0, 1, 0),
    "strategy.attack_enemy_hq": ("flag", "Faction.player_flags", 0, 0, 0x200000),
    "strategy.land_power": ("flag", "Faction.player_flags", 0, 0, 0x4000000),
    "strategy.sea_power": ("flag", "Faction.player_flags", 0, 0, 0x8000000),
    "strategy.air_power": ("flag", "Faction.player_flags", 0, 0, 0x10000000),
    "strategy.search_objectives": ("flag", "Faction.player_flags", 0, 0, 0x20000000),
    "strategy.defend_objectives": ("flag", "Faction.player_flags", 0, 0, 0x40000000),
    "strategy.attack_objectives": ("flag", "Faction.player_flags", 0, 0, 0x80000000),
    "target.base": ("int", "Faction.base_id_attack_target", -1, 511, 0),
    "target.goals": ("goals", "Faction.goals", 0, 8, 0),
}
GOAL_TYPES = {"attack": 0, "defend": 2, "scout": 3, "colonize": 8}
KIND = {"NA_KNOB_INT": "int", "NA_KNOB_FLAG": "flag", "NA_KNOB_GOALS": "goals"}
CONST = {"NA_POLICY_MAX_BASE_ID": 511, "NA_POLICY_MAX_GOALS": 8}


def _num(tok: str) -> int:
    tok = tok.strip().rstrip("u")
    return CONST[tok] if tok in CONST else int(tok, 0)


def cpp_registry(src: str) -> dict[str, tuple[str, str, int, int, int]]:
    body = re.search(r"const NaKnobDef NA_KNOBS\[\] = \{(.*?)\n\};", src, re.S)
    assert body, "NA_KNOBS table not found in src/na_policy.cpp"
    rows = {}
    for m in re.finditer(r"\{\s*\"([^\"]+)\",\s*(\w+),\s*\"([^\"]+)\",([^}]*)\}", body.group(1)):
        name, kind, field, rest = m.groups()
        _int_field, lo, hi, mask = (t.strip() for t in rest.strip().strip(",").split(","))
        rows[name] = (KIND[kind], field, _num(lo), _num(hi), _num(mask))
    return rows


def cpp_goal_types(src: str) -> dict[str, int]:
    body = re.search(r"const NaGoalType NA_GOAL_TYPES\[\] = \{(.*?)\n\};", src, re.S)
    assert body, "NA_GOAL_TYPES table not found"
    return {n: int(v) for n, v in re.findall(r"\{\"(\w+)\",\s*(\d+)\}", body.group(1))}


def live_registry(na: Path) -> dict[str, tuple[str, str, int, int, int]] | None:
    path = na / "orchestrator/src/neural_amplifier/faction_policy.py"
    if not path.is_file():
        return None
    mod = runpy.run_path(str(path))
    rows = {}
    for row in mod["registry"]():
        kind = row["type"]
        lo, hi = (row["lo"], row["hi"]) if kind == "int" else (0, row.get("max", 0))
        rows[row["name"]] = (kind, row["field"], lo, hi, row.get("mask", 0))
    assert dict(mod["GOAL_TYPES"]) == GOAL_TYPES, "orchestrator goal types changed"
    return rows


def main() -> int:
    src = (ROOT / "src/na_policy.cpp").read_text()
    fork = cpp_registry(src)
    failures = []
    if fork != FROZEN:
        failures.append(f"fork registry differs from the frozen one:\n  fork   {fork}\n  frozen {FROZEN}")
    if cpp_goal_types(src) != GOAL_TYPES:
        failures.append("fork goal types differ from the frozen ones")

    na = Path(os.environ.get("NA_DIR", ROOT.parent / "NeuralAmplifier"))
    live = live_registry(na)
    if live is None:
        print(f"      (no orchestrator at {na}; compared against the frozen registry only)")
    elif live != fork:
        failures.append(f"fork registry differs from {na}:\n  fork {fork}\n  live {live}")

    neural = (ROOT / "src/neural.cpp").read_text()
    start = neural.index("Per-faction policy overlay (docs/seats-and-policy.md")
    end = neural.index("Announce the coming turn's expected decisions.")
    section = neural[start:end]
    for banned in ("na_http_post", "na_post_outcome", "plans["):
        if banned in section:
            failures.append(f"the overlay section uses {banned}: the overlay is write-only")
    if "if (got == NA_POLICY_ABSENT && !held_before && !na_policy_hash[0])" not in section:
        failures.append("the empty-overlay early return is gone: an empty policy must touch nothing")
    if "na_policy_receive(resp.data);" not in neural:
        failures.append("the /turn reply no longer reaches na_policy_receive")

    for f in failures:
        print(f"FAIL  {f}")
    if not failures:
        print(f"ok    the fork's {len(fork)} policy knobs mirror the orchestrator's registry")
        print("ok    the overlay section sends nothing and touches nothing when empty")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
