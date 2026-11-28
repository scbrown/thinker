#!/usr/bin/env python3
"""aegis-5af / aegis-km6g: an unattended -na-new-game must get past PLANETFALL, the first-base
monument and the human seat's research picker."""

import pathlib
import sys


ROOT = pathlib.Path(__file__).resolve().parent.parent
neural = (ROOT / "src" / "neural.cpp").read_text()
config = (ROOT / "src" / "config.cpp").read_text()
failures = []

tick = neural[neural.index("void na_autoload_tick() {"):]
tick = tick[: tick.index("\n}\n")]
if "na_autoload.empty() && !new_game" not in tick:
    failures.append("na_autoload_tick leaves early for -na-new-game again (PLANETFALL never confirmed)")
session = tick[tick.index("case NA_AS_WAIT_SESSION"):]
guard = session.find("na_autoload.empty()")
if guard < 0 or guard > session.find("mod_load_daemon("):
    failures.append("a -na-new-game session reaches the save-load call")
if '"new_game_session"' not in session:
    failures.append("the new-game session no longer logs new_game_session")

use = config[config.index("void __cdecl prefs_use() {"):]
use = use[: use.index("\n}\n")]
gate = use[use.find("if (na_headless())"):] if "if (na_headless())" in use else ""
for flag, live in (("PREF_AV_INTERLUDES_DISABLED", "*GamePreferences |="),
                   ("MPREF_AV_MONUMENTS_DISABLED", "*GameMorePreferences |=")):
    if f"{live} {flag}" not in gate:
        failures.append(f"headless prefs_use no longer sets {flag} on the live global")
if "AlphaIniPrefs->" in gate:
    failures.append("the headless override writes AlphaIniPrefs (it would persist into the ini)")

tech = (ROOT / "src" / "tech.cpp").read_text()
pick = tech[tech.index("int __cdecl mod_tech_pick("):]
pick = pick[: pick.index("\n}\n")]
# aegis-km6g: the engine's tech_pick opens SELECT RESEARCH GOAL for a human seat.
if "other_faction_id < 0 && is_human(faction_id) && na_headless()" not in pick:
    failures.append("headless research picks for the human seat reach the engine picker again")
elif pick.find("mod_tech_ai(faction_id)") < 0 or pick.find("mod_tech_ai(faction_id)") > pick.find(
    "tech_pick(faction_id, flag, other_faction_id, label)"
):
    failures.append("the headless research branch no longer takes mod_tech_ai's choice")
select = tech[tech.index("int __cdecl mod_tech_selection("):]
if select.find("mod_tech_pick(") > select.find("na_decide_faction_tech("):
    failures.append("a brain seat no longer gets the research decision after the pick")

if failures:
    print(f"new-game headless check FAILED — {len(failures)} problem(s):", file=sys.stderr)
    for failure in failures:
        print(f"  {failure}", file=sys.stderr)
    sys.exit(1)

print("headless -na-new-game confirms PLANETFALL, skips the load, suppresses monuments/interludes, and auto-picks research")
