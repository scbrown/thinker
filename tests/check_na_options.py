#!/usr/bin/env python3
"""na-alr.3: prove every Neural Amplifier setting still reaches its field.

This surface fails SILENTLY. If a key stops being parsed, the option reverts to
its default, the build is clean and the game starts and plays — just not the way
it was configured to. llm_factions defaulting back to 0 means "no bridge in the
loop", which is indistinguishable from a working stock build.

That is not hypothetical: v5.5 rewrote config.cpp, and the reason the sync
checked this by hand (na-15o.4) is that a MATCH chain left behind by a refactor
still compiles and simply never runs.

Three things have to agree, and each disagreement is a different bug:

  na_config.h   the fields                      -> a field nobody parses is dead
  neural.cpp    the ini-key -> field table      -> a table entry to nowhere
  thinker.ini   the documented keys             -> a key that does nothing

Source-only. No build, no game.
"""

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
CONFIG_H = ROOT / "src" / "na_config.h"
NEURAL = ROOT / "src" / "neural.cpp"
INI = ROOT / "docs" / "thinker.ini"

# Parsed by hand rather than through NaConfig, because it is a std::string
# global (Config gets memcpy-ed around by engine code).
STRING_OPTIONS = {"llm_endpoint"}


def fields():
    text = CONFIG_H.read_text()
    body = text[text.index("struct NaConfig {"):]
    return {m.group(1) for m in re.finditer(r"^\s+int\s+(\w+)\s*=", body, re.M)}


def table():
    text = NEURAL.read_text()
    block = text[text.index("static const NaOption NaOptions[]"):]
    block = block[:block.index("};")]
    return {k: f for k, f in re.findall(r'\{"(\w+)",\s*&NaConfig::(\w+)\}', block)}


def documented():
    keys = set()
    for line in INI.read_text().splitlines():
        m = re.match(r"^(na_\w+|llm_\w+)\s*=", line)
        if m:
            keys.add(m.group(1))
    return keys


def main():
    f, t, d = fields(), table(), documented()
    failures = []

    for key, field in sorted(t.items()):
        if field not in f:
            failures.append(f"table entry '{key}' points at NaConfig::{field}, "
                            f"which is not a field in na_config.h")

    for field in sorted(f):
        hits = [k for k, v in t.items() if v == field]
        if not hits:
            failures.append(f"NaConfig::{field} is never parsed — no thinker.ini key "
                            f"reaches it, so it is stuck at its default forever")
        elif len(hits) > 1:
            failures.append(f"NaConfig::{field} is parsed from {len(hits)} keys "
                            f"({', '.join(sorted(hits))}); last one wins silently")

    for key in sorted(d):
        if key not in t and key not in STRING_OPTIONS:
            failures.append(f"thinker.ini documents '{key}' but nothing parses it — "
                            f"a setting that reads as supported and does nothing")

    for key in sorted(set(t) | STRING_OPTIONS):
        if key not in d:
            failures.append(f"'{key}' is parsed but undocumented in docs/thinker.ini")

    if failures:
        print(f"NA option check FAILED — {len(failures)} problem(s):\n", file=sys.stderr)
        for line in failures:
            print(f"  {line}", file=sys.stderr)
        return 1

    print(f"all NA options wired: {len(t)} int settings + {len(STRING_OPTIONS)} string, "
          f"fields/table/thinker.ini agree")
    return 0


if __name__ == "__main__":
    sys.exit(main())
