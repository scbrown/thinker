#!/usr/bin/env python3
"""na-alr.1: prove every Neural Amplifier seam is still where it has to be.

WHY THIS EXISTS. The adapter's seams live inside functions Thinker owns, and
losing one is silent in every gate we had. Measured on the v5.5 merge: delete
`na_observe_econ_energy_sliders` from gameturn.cpp and you get a build with exit
0 and ZERO warnings, 43/43 wire tests passing, and check_native_choice_withheld
passing. The observation just stops happening.

That is not hypothetical for this fork. Upstream v5.5 moved the whole turn loop
into gameturn.cpp and dropped the mod_ prefix; the merge presented 547 lines of
ours against an empty upstream side and eight seams had to be re-placed by hand.

So this checks two different things, and the second is the interesting one:

  PRESENCE  - the seam is still in its host function, the right number of times.
  ORDER     - the seam is still in the right PLACE in that function.

A seam that survives a merge but MOVES is as wrong as one that vanished, and
only the ordering half can catch it. Those invariants are not stylistic: reading
the energy sliders before the engine's last clamp records a value the game goes
on to overwrite, and sampling the corner-market reserve after the deduction
records what survived the decision rather than what it was made against.

Reads source only — no game, no build, no network. Runs next to the wire tests.

  python3 tests/check_seams.py              verify against tests/na_seams.tsv
  python3 tests/check_seams.py --regenerate rewrite the manifest from source
"""

import os
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import na_seams  # noqa: E402

# NA_SEAM_ROOT lets test_check_seams.py point the checker at a mutated copy of
# the tree. A guard that has never been watched failing is not a guard.
ROOT = pathlib.Path(os.environ.get("NA_SEAM_ROOT")
                    or pathlib.Path(__file__).resolve().parent.parent)
SRC = ROOT / "src"
MANIFEST = ROOT / "tests" / "na_seams.tsv"

HEADER = ["file", "host", "symbol", "kind", "count", "invariant"]

# Ordering invariants, keyed (file, host, symbol). Everything not listed is
# checked for presence only. Add an entry here when placement carries an
# argument that a reader could otherwise undo by accident.
#
#   first          must be the first statement of the host function
#   last           must be the last statement of the host function
#   before:REGEX   every occurrence sits above the first line matching REGEX
#   after:REGEX    every occurrence sits below the last line matching REGEX
INVARIANTS = {
    # -na-exit-turn must read the turn that just FINISHED, so it has to run
    # before *CurrentTurn is incremented forty-odd lines down.
    ("gameturn.cpp", "turn_upkeep", "na_exit_turn_check"): "first",
    # A run that is stopping must not first publish a forecast for a turn it
    # will never play, so the announce sits below the exit check.
    ("gameturn.cpp", "turn_upkeep", "na_announce_turn"): r"after:na_exit_turn_check\(\)",
    # The faction's economy totals are zeroed and re-accumulated one base at a
    # time; mod_base_build fires inside that window. The mark has to open
    # before the zeroing or the window it describes is wrong.
    ("gameturn.cpp", "production_phase", "na_accumulate_begin"):
        r"before:f->best_mineral_output = 0",
    ("gameturn.cpp", "production_phase", "na_accumulate_end"): "last",
    # The reserve has to be read before the deduction: the record shows what
    # the decision was made against, not what survived it.
    ("gameturn.cpp", "faction_upkeep", "na_observe_corner_market"):
        r"before:f->energy_credits -= cost",
    # allocate_energy narrows SE_alloc_labs four separate times. Observing
    # anywhere earlier records a value the engine goes on to overwrite.
    ("gameturn.cpp", "allocate_energy", "na_observe_econ_energy_sliders"): "last",
    # The dialog hook replaces a pointer that the write_call table below may
    # already read, so it must be installed before the first patch is written.
    ("patch.cpp", "patch_setup", "na_install_dialog_hook"):
        r"before:write_(call|jump)\(",
}


def collect():
    """Aggregate seams into {(file, host, symbol, kind): [linenos]}."""
    agg = {}
    for s in na_seams.extract_all(SRC):
        agg.setdefault((s["file"], s["host"], s["symbol"], s["kind"]), []).append(s["line"])
    return agg


def write_manifest(agg):
    rows = ["\t".join(HEADER)]
    for key in sorted(agg):
        f, host, sym, kind = key
        inv = INVARIANTS.get((f, host, sym), "-")
        rows.append("\t".join([f, host, sym, kind, str(len(agg[key])), inv]))
    MANIFEST.write_text("\n".join(rows) + "\n")


def read_manifest():
    if not MANIFEST.exists():
        return None
    out = {}
    lines = MANIFEST.read_text().splitlines()
    for line in lines[1:]:
        if not line.strip():
            continue
        f, host, sym, kind, count, inv = line.split("\t")
        out[(f, host, sym, kind)] = (int(count), inv)
    return out


def check_order(failures, f, host, sym, inv, linenos):
    path = SRC / f
    lines = path.read_text().splitlines()
    span = na_seams.function_body(lines, host)
    if span is None:
        failures.append(f"{f}: cannot locate body of {host}() to check '{inv}' for {sym}")
        return
    stmts = na_seams.body_statements(lines, span)
    if not stmts:
        failures.append(f"{f}: {host}() has an empty body; cannot check '{inv}' for {sym}")
        return

    if inv == "first":
        lineno, code = stmts[0]
        if sym not in code:
            failures.append(
                f"{f}:{lineno} {host}(): {sym} must be the FIRST statement, "
                f"but the first statement is: {code.strip()[:60]}")
    elif inv == "last":
        lineno, code = stmts[-1]
        if sym not in code:
            failures.append(
                f"{f}:{lineno} {host}(): {sym} must be the LAST statement, "
                f"but the last statement is: {code.strip()[:60]}")
    elif inv.startswith("before:") or inv.startswith("after:"):
        mode, _, pattern = inv.partition(":")
        rx = re.compile(pattern)
        anchors = [ln for ln, code in stmts if rx.search(code) and sym not in code]
        if not anchors:
            failures.append(
                f"{f}: {host}(): anchor /{pattern}/ for {sym} no longer appears — "
                f"the invariant cannot be checked, so it is treated as broken")
            return
        for ln in linenos:
            if mode == "before" and ln > min(anchors):
                failures.append(
                    f"{f}:{ln} {host}(): {sym} must sit BEFORE /{pattern}/ "
                    f"(first match at line {min(anchors)})")
            if mode == "after" and ln < max(anchors):
                failures.append(
                    f"{f}:{ln} {host}(): {sym} must sit AFTER /{pattern}/ "
                    f"(last match at line {max(anchors)})")
    else:
        failures.append(f"unknown invariant '{inv}' for {sym} in {f}:{host}")


def main():
    agg = collect()

    if "--regenerate" in sys.argv:
        write_manifest(agg)
        calls = sum(len(v) for k, v in agg.items() if k[3] == "call")
        defs = sum(len(v) for k, v in agg.items() if k[3] == "def")
        print(f"wrote {MANIFEST.relative_to(ROOT)}: {calls} call sites, "
              f"{defs} definitions, across "
              f"{len({k[0] for k in agg})} upstream-owned files")
        return 0

    expected = read_manifest()
    if expected is None:
        print(f"no manifest at {MANIFEST.relative_to(ROOT)} — "
              f"run: python3 tests/check_seams.py --regenerate", file=sys.stderr)
        return 1

    failures = []

    for key, (count, inv) in sorted(expected.items()):
        f, host, sym, kind = key
        found = agg.get(key)
        if not found:
            # Say where it went, if it went anywhere — a seam that moved to
            # another function is a different bug from one that was dropped.
            elsewhere = [k[1] for k in agg if k[0] == f and k[2] == sym]
            hint = f" (now in {', '.join(sorted(set(elsewhere)))})" if elsewhere else ""
            failures.append(f"MISSING  {f}:{host}() {sym}{hint}")
            continue
        if len(found) != count:
            failures.append(
                f"COUNT    {f}:{host}() {sym}: manifest says {count}, found {len(found)} "
                f"(lines {', '.join(map(str, found))})")
        if inv != "-":
            check_order(failures, f, host, sym, inv, found)

    for key in sorted(agg):
        if key not in expected:
            f, host, sym, kind = key
            failures.append(
                f"UNTRACKED {f}:{host}() {sym} ({kind}) is not in the manifest — "
                f"run --regenerate if this is intended")

    calls = sum(len(v) for k, v in agg.items() if k[3] == "call")
    defs = sum(len(v) for k, v in agg.items() if k[3] == "def")
    checked_order = sum(1 for _, (_, i) in expected.items() if i != "-")

    if failures:
        print(f"seam check FAILED — {len(failures)} problem(s):\n", file=sys.stderr)
        for f in failures:
            print(f"  {f}", file=sys.stderr)
        print(f"\n{calls} call sites + {defs} definitions expected across "
              f"{len({k[0] for k in expected})} upstream-owned files", file=sys.stderr)
        return 1

    print(f"all seams present: {calls} call sites, {defs} definitions, "
          f"{len({k[0] for k in expected})} upstream-owned files, "
          f"{checked_order} ordering invariants held")
    return 0


if __name__ == "__main__":
    sys.exit(main())
