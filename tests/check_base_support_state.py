#!/usr/bin/env python3
"""na-b8g: base state must expose the engine's support mechanism, not a proxy."""

import pathlib
import sys


ROOT = pathlib.Path(__file__).resolve().parent.parent
text = (ROOT / "src" / "neural.cpp").read_text()
start = text.index("static void na_write_base_state(")
end = text.index("\n}\n", start)
body = text[start:end]

failures = []
for field in ("support_cost", "support_free", "se_support"):
    if f'\\\"{field}\\\"' not in body:
        failures.append(f"base_state no longer emits {field}")

if "VSTATE_REQUIRES_SUPPORT" not in body or "support_cost += support_unit_cost" not in body:
    failures.append("support_cost is not derived from engine-marked charged units")
if "unit_support_free(f.SE_support, b.pop_size)" not in body:
    failures.append("support_free is not derived from the engine allowance function")
if "unit_support_cost(f.SE_support)" not in body:
    failures.append("support_cost does not use the engine cost table")
if '"support_cost":%d", b.mineral_consumption' in body:
    failures.append("mineral_consumption was relabelled as support despite convoy costs")
if "if (support_cost" in body:
    failures.append("zero support cost is omitted instead of serving as the positive control")

if failures:
    print(f"base support state check FAILED — {len(failures)} problem(s):", file=sys.stderr)
    for failure in failures:
        print(f"  {failure}", file=sys.stderr)
    sys.exit(1)

print("base state exports engine-marked support cost, free allowance, and SE rating")
