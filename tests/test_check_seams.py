#!/usr/bin/env python3
"""na-alr.1: hold the seam guard to the regressions it claims to catch.

check_seams.py exists because losing a seam is silent everywhere else — the
build stays clean, the wire tests stay green. A guard for a silent failure is
worth exactly as much as its ability to go red, and a guard nobody has watched
fail is indistinguishable from one that always passes.

So each case below mutates a COPY of the tree the way a bad merge actually
would, and asserts the checker refuses it. The control at the end asserts the
unmutated tree still passes, so a checker that simply fails at everything
cannot pass this file either.
"""

import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
CHECKER = ROOT / "tests" / "check_seams.py"


def run(root):
    env = {"NA_SEAM_ROOT": str(root), "PATH": "/usr/bin:/bin"}
    p = subprocess.run([sys.executable, str(CHECKER)],
                       capture_output=True, text=True, env=env)
    return p.returncode, p.stdout + p.stderr


def sandbox(tmp):
    """A throwaway copy of everything the checker reads."""
    root = pathlib.Path(tmp) / "tree"
    (root / "tests").mkdir(parents=True)
    shutil.copytree(ROOT / "src", root / "src")
    for f in ("na_seams.tsv", "na_seams.py", "check_seams.py"):
        shutil.copy(ROOT / "tests" / f, root / "tests" / f)
    return root


def edit(root, relpath, fn):
    path = root / relpath
    path.write_text(fn(path.read_text()))


CASES = []


def case(name, expect):
    def deco(fn):
        CASES.append((name, fn, expect))
        return fn
    return deco


@case("a dropped seam is caught", "MISSING")
def drop_seam(root):
    edit(root, "src/gameturn.cpp",
         lambda t: t.replace("    na_observe_econ_energy_sliders(faction_id);\n", ""))


@case("a seam commented out is caught", "MISSING")
def comment_seam(root):
    edit(root, "src/gameturn.cpp",
         lambda t: t.replace("    na_accumulate_end(faction_id);",
                             "    // na_accumulate_end(faction_id);"))


@case("a seam that moved to another function is caught, and says where", "now in")
def move_between_functions(root):
    # Delete from production_phase, re-add inside allocate_energy.
    edit(root, "src/gameturn.cpp",
         lambda t: t.replace("    na_accumulate_begin(faction_id);\n", ""))
    edit(root, "src/gameturn.cpp",
         lambda t: t.replace("void __cdecl allocate_energy(int faction_id) {",
                             "void __cdecl allocate_energy(int faction_id) {\n"
                             "    na_accumulate_begin(faction_id);"))


@case("a 'last' seam that drifted upward is caught", "must be the LAST statement")
def break_last(root):
    # Still present, still in the right function, but now above the final
    # clamp loop it is required to follow. This is the case a presence-only
    # check cannot see.
    edit(root, "src/gameturn.cpp", lambda t: t.replace(
        "    na_observe_econ_energy_sliders(faction_id);\n", ""))
    edit(root, "src/gameturn.cpp", lambda t: t.replace(
        "    int effic_val = clamp(plr->SE_effic_2, 0, 99)",
        "    na_observe_econ_energy_sliders(faction_id);\n"
        "    int effic_val = clamp(plr->SE_effic_2, 0, 99)"))


@case("a 'first' seam that drifted downward is caught", "must be the FIRST statement")
def break_first(root):
    edit(root, "src/gameturn.cpp", lambda t: t.replace(
        "    na_exit_turn_check();\n", "", 1))
    edit(root, "src/gameturn.cpp", lambda t: t.replace(
        "    snprintf(ThinkerVars->build_date, 12, MOD_DATE);",
        "    na_exit_turn_check();\n"
        "    snprintf(ThinkerVars->build_date, 12, MOD_DATE);"))


@case("a 'before:' seam that slipped past its anchor is caught", "must sit BEFORE")
def break_before(root):
    # Sample the corner-market reserve AFTER the deduction — the exact error
    # the invariant exists to prevent, and one that still compiles and runs.
    edit(root, "src/gameturn.cpp", lambda t: t.replace(
        "            if (conf.na.endgame_observe) {\n"
        "                na_observe_corner_market(faction_id, cost, na_credits_before, na_cornered);\n"
        "            }\n", ""))
    edit(root, "src/gameturn.cpp", lambda t: t.replace(
        "                f->energy_credits -= cost;",
        "                f->energy_credits -= cost;\n"
        "                na_observe_corner_market(faction_id, cost, na_credits_before, na_cornered);"))


@case("an anchor that vanished fails rather than silently passing", "cannot be checked")
def anchor_gone(root):
    # If upstream renames the line an invariant is pinned to, the honest
    # outcome is a failure asking for a human, not a green tick.
    edit(root, "src/gameturn.cpp", lambda t: t.replace(
        "    f->best_mineral_output = 0;", "    f->best_mineral_output_v2 = 0;"))


@case("a duplicated seam is caught by count", "COUNT")
def duplicate_seam(root):
    edit(root, "src/gameturn.cpp", lambda t: t.replace(
        "    na_accumulate_end(faction_id);",
        "    na_accumulate_end(faction_id);\n    na_accumulate_end(faction_id);"))


@case("a new untracked seam is caught", "UNTRACKED")
def untracked_seam(root):
    edit(root, "src/tech.cpp", lambda t: t.replace(
        "int __cdecl mod_tech_ai(int faction_id) {",
        "int __cdecl mod_tech_ai(int faction_id) {\n    na_brand_new_seam(faction_id);"))


@case("a seam lost with its whole host function is caught", "MISSING")
def drop_host_function(root):
    # What v5.5 actually did: the host function disappeared from the file.
    text = (root / "src/gameturn.cpp").read_text()
    start = text.index("void __cdecl allocate_energy(int faction_id) {")
    edit(root, "src/gameturn.cpp", lambda t: t[:start])


def main():
    failures = []
    for name, mutate, expect in CASES:
        with tempfile.TemporaryDirectory() as tmp:
            root = sandbox(tmp)
            mutate(root)
            code, out = run(root)
            if code == 0:
                failures.append(f"{name}: checker PASSED a tree it should have refused")
            elif expect not in out:
                failures.append(
                    f"{name}: refused, but not for the stated reason "
                    f"(no {expect!r} in output)")
            else:
                print(f"ok    {name}")

    # Control: the real tree must still pass, or every case above is vacuous.
    with tempfile.TemporaryDirectory() as tmp:
        root = sandbox(tmp)
        code, out = run(root)
        if code != 0:
            failures.append(f"CONTROL: unmutated tree was refused:\n{out}")
        else:
            print("ok    the unmutated tree still passes (control)")

    print()
    if failures:
        for f in failures:
            print(f"FAIL  {f}", file=sys.stderr)
        print(f"\n{len(CASES) + 1} checks, {len(failures)} failures", file=sys.stderr)
        return 1
    print(f"{len(CASES) + 1} checks, 0 failures")
    return 0


if __name__ == "__main__":
    sys.exit(main())
