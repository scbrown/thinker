#!/usr/bin/env python3
"""aegis-us0fb: keep modal visibility and the headless escape ahead of call-through."""

import pathlib
import sys


ROOT = pathlib.Path(__file__).resolve().parent.parent
text = (ROOT / "src" / "neural.cpp").read_text()
start = text.index("int __thiscall na_dialog_exec(")
end = text.index("\nint __cdecl na_popp(", start)
body = text[start:end]

engine = body.index("na_engine_dialog_exec(This, a2, a3)")
escape = body.index("matched && entry && conf.na.dialog_auto && na_headless()")
observe = body.index("na_observe_dialog(na_pending_dialog_file, na_pending_dialog_label, -1)")

failures = []
if escape > engine:
    failures.append("the unmapped headless escape moved after the call that can block forever")
if observe > engine:
    failures.append("dialog observation moved back onto the blocking call's return path")
if 'strcmp(entry->label, "COMM") == 0' not in body[escape:engine]:
    failures.append("the headless escape widened beyond the explicitly mapped COMM dialog")
if "entry->button_decline" not in body[escape:engine]:
    failures.append("the COMM escape no longer uses the table's explicit decline mapping")
if "na_dialog_auto_answered++" not in body[escape:engine]:
    failures.append("the COMM escape no longer increments auto_answered")

table_start = text.index("static const NaDialogEntry NaDialogTable[]")
table_end = text.index("};", table_start)
table = text[table_start:table_end]
if '{nullptr,   "COMM",' not in table or "NA_DIALOG_DECISION, 0, 1" not in table:
    failures.append("COMM is not mapped as a decision with Script.txt's yes=0/no=1 buttons")

if failures:
    print(f"dialog headless escape check FAILED — {len(failures)} problem(s):", file=sys.stderr)
    for failure in failures:
        print(f"  {failure}", file=sys.stderr)
    sys.exit(1)

print("dialog headless escape precedes call-through and records visibility + answer")
