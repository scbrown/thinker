#!/usr/bin/env python3
"""aegis-8fye.9: every operator verb on the command channel is gated in neural.cpp.

The policy itself is tested under Wine (na_order_policy_test). What that cannot see is whether
the handlers in neural.cpp actually CALL it, and in what order. That is a property of the
source, and it fails silently: a handler that loses its gate in a merge still compiles, still
works, and lets a seat write another faction's production.

For each operator verb, inside its handler block:

  1. na_operator_gate("<verb>") is the first statement — before parsing, so a refused seat
     learns nothing about the board from a validation message;
  2. for the apply* verbs, na_apply_gate("<verb>", ...) runs before the first engine write,
     and for apply / apply-hurry its faction comes from Bases[...].faction_id (the entity's
     owner), never from a parsed argument.

It also checks that the verb list in na_order_policy.h and the handlers agree, so a new
operator verb cannot be added to one and forgotten in the other.

Source-only. No build, no game.
"""

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
NEURAL = ROOT / "src" / "neural.cpp"
POLICY = ROOT / "src" / "na_order_policy.h"

# The first engine write each apply* verb makes. The gate must precede it.
FIRST_WRITE = {
    "apply": "mod_base_change(",
    "apply-tech": "plr.tech_research_id = ",
    "apply-se": 'na_cmd_result("apply-se", "no change", true)',
    "apply-hurry": 'na_cmd_result("apply-hurry", "did not hurry", true)',
}
ENTITY_OWNER = {
    "apply": "const int fid = Bases[base_id].faction_id;",
    "apply-hurry": 'na_apply_gate("apply-hurry", base.faction_id)',
}


def policy_verbs():
    text = POLICY.read_text()
    block = text[text.index("na_policy_is_operator_verb"):]
    block = block[block.index("{", block.index("verbs[]")):block.index("};")]
    return re.findall(r'"([a-z-]+)"', block)


def handler(text, verb):
    m = re.search(r'if \(strncmp\(line, "%s[ "]' % re.escape(verb), text)
    if not m:
        return None
    start = m.end()
    # The handler runs until the next top-level command check.
    nxt = re.search(r'\n    if \(str(n)?cmp\(line, "', text[start:])
    return text[start:start + nxt.start()] if nxt else text[start:]


def main():
    text = NEURAL.read_text()
    failures = []
    verbs = policy_verbs()
    if len(verbs) < 9:
        failures.append(f"policy verb list looks truncated: {verbs}")
    for verb in verbs:
        body = handler(text, verb)
        if body is None:
            failures.append(f"{verb}: no handler found in neural.cpp")
            continue
        first = body.split("\n", 2)[1].strip() if "\n" in body else ""
        if first != f'if (!na_operator_gate("{verb}")) {{':
            failures.append(f"{verb}: na_operator_gate is not the first statement (got {first!r})")
        if verb in FIRST_WRITE:
            gate = body.find(f'na_apply_gate("{verb}"')
            write = body.find(FIRST_WRITE[verb])
            if gate < 0:
                failures.append(f"{verb}: no na_apply_gate")
            elif write < 0:
                failures.append(f"{verb}: first-write marker {FIRST_WRITE[verb]!r} not found")
            elif gate > write:
                failures.append(f"{verb}: na_apply_gate runs after the first engine write")
        if verb in ENTITY_OWNER and ENTITY_OWNER[verb] not in body:
            failures.append(f"{verb}: faction is not derived from the base's owner")
    # The gate must not have drifted from its three documented answers.
    if "return na_policy_order(na_policy_state(), faction_id, why, n);" not in text:
        failures.append("na_order_gate no longer delegates to na_policy_order")

    for f in failures:
        print("FAIL ", f)
    if failures:
        return 1
    print(f"ok    {len(verbs)} operator verbs gated; apply* faction derived from the entity")
    return 0


if __name__ == "__main__":
    sys.exit(main())
