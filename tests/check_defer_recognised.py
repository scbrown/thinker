"""na-7bk: prove every decide surface recognises `defer`, and recognises it FIRST.

The property has two halves and the second is the one that bites.

1. Each `na_decide_*` function that parses an `action_id` from the reply must have a
   `na_action_is_defer` branch.
2. That branch must come BEFORE the branch that reports an unparseable id.

Both halves matter because the OUTCOME is identical either way — the engine's own pick is
applied — so nothing about the game's behaviour reveals a missing branch. What differs is the
record: fall through to the unparseable branch and the decision is written as
`tier=deterministic` with `fallback_reason="unparseable action_id defer"`, which says the
orchestrator malfunctioned. Nothing malfunctioned. An agent asked for more time.

That distinction is the entire mechanism. `degrade_rate` is derived from it, and it exists to
catch a run where the brain was silently absent — so a deferral recorded as a degradation makes
a working agent look like a missing one, in the one number built to tell those apart.

Static, on source order, for the same reason `check_native_choice_withheld.py` is: the ladder is
an if/else chain, so its order IS its behaviour and can be read without a game.

A surface added later without a defer branch fails here, which is the point — this file exists
because the failure it guards is invisible at runtime.
"""

import pathlib
import re
import sys

SRC = str(pathlib.Path(__file__).resolve().parent.parent / "src" / "neural.cpp")
lines = open(SRC).read().splitlines()

_STR = re.compile(r'"(?:\\.|[^"\\])*"')


def _code(ln: str) -> str:
    """Braces inside string literals are not scope."""
    return _STR.sub('""', ln)


def body_of(fn: str) -> list[tuple[int, str]]:
    start = next(i for i, ln in enumerate(lines) if re.match(rf"\w[\w \*]*\b{fn}\(", ln))
    depth, out, seen = 0, [], False
    for i in range(start, len(lines)):
        out.append((i + 1, lines[i]))
        c = _code(lines[i])
        depth += c.count("{") - c.count("}")
        if "{" in c:
            seen = True
        if seen and depth <= 0:
            break
    return out


def decide_surfaces() -> list[str]:
    """Every na_decide_* that reads an action_id out of a /decide reply.

    Discovered rather than listed. A hand-maintained list would pass on the day someone adds a
    fifth surface, which is precisely the day this check needs to fail.
    """
    found = []
    for i, ln in enumerate(lines):
        m = re.match(r"\w[\w \*]*\b(na_decide_\w+)\(", ln)
        if not m:
            continue
        name = m.group(1)
        body = "\n".join(l for _, l in body_of(name))
        if 'na_json_string(body.data, "action_id"' in body:
            found.append(name)
    return found


failures = []
surfaces = decide_surfaces()

if not surfaces:
    # A zero here would otherwise be a silent pass: no surfaces found, nothing checked, green.
    print("FAIL: found no na_decide_* surface parsing an action_id — the check never ran")
    sys.exit(1)

for fn in surfaces:
    body = body_of(fn)
    defer_at = [n for n, ln in body if "na_action_is_defer(" in ln]
    unparse_at = [n for n, ln in body if "unparseable action_id" in ln]

    if not defer_at:
        failures.append(
            f"{fn}: no na_action_is_defer branch — a deferral here would be recorded as an "
            f"orchestrator malfunction (tier=deterministic + fallback_reason)"
        )
        status = "FAIL"
    elif unparse_at and min(defer_at) > min(unparse_at):
        failures.append(
            f"{fn}:{min(defer_at)}: the defer branch is AFTER the unparseable branch at "
            f":{min(unparse_at)} — it can never be reached"
        )
        status = "FAIL"
    else:
        status = "OK"
    print(f"{fn:28} defer@{defer_at} unparseable@{unparse_at} {status}")

# The tier string has to actually be written, or the branch is decoration.
if "\"deferred\"" not in "\n".join(lines):
    failures.append('no tier="deferred" is ever written — the branch records nothing new')

if failures:
    print(f"\ndefer check FAILED — {len(failures)} problem(s):\n")
    for f in failures:
        print(f"  {f}")
    sys.exit(1)

print(f"\n{len(surfaces)} decide surface(s) recognise defer before reporting it unparseable")
