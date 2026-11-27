#pragma once
/*
Per-faction policy overlay: the fork's mirror of the orchestrator's frozen knob registry, and
the parser for the overlay the `POST /turn` reply carries (docs/seats-and-policy.md, section 4).

Like na_http.cpp, this links no engine headers. That keeps the parser testable under Wine
with no game (tests/na_policy_test.cpp), and keeps the registry table readable by a source
check that compares it with the orchestrator's (tests/check_na_policy_registry.py).

THE WIRE SHAPE, exactly as the orchestrator sends it:

    {"turn": 12, "expected": 3,
     "na_policy": [{"faction_id": 3, "knobs": {"focus.fight": 1, "target.base": 41}}],
     "policy_hash": "<64 hex chars>"}

`na_policy` is the COMPLETE overlay. A faction absent from it has no overlay, and the fork
restores its own value. When nothing is set both keys are absent and the reply is the one
this fork has always ignored.

The parser refuses the whole overlay on anything it cannot check: an unknown knob, a value
outside the registry's bounds, a faction outside 1..7 or repeated, a malformed document.
A refused overlay is applied as the empty one, which is Thinker's own behaviour. A posture
half-understood is worse than none, and the orchestrator validated it already, so a refusal
here means version skew, not a seat's mistake.
*/

#include <stddef.h>
#include <stdint.h>

static const int NA_POLICY_FACTIONS = 8;   // MaxPlayerNum
static const int NA_POLICY_MAX_GOALS = 8;  // faction_policy.MAX_GOALS
static const int NA_POLICY_MAX_BASE_ID = 511;
static const int NA_POLICY_MAX_COORD = 511;

// The integer fields a knob can write. Indices into NaPolicy::ints.
enum NaPolicyIntField {
    NA_PF_AI_FIGHT,
    NA_PF_AI_GROWTH,
    NA_PF_AI_TECH,
    NA_PF_AI_WEALTH,
    NA_PF_AI_POWER,
    NA_PF_ATTACK_TARGET,
    NA_PF_INT_COUNT,
};

enum NaKnobKind {
    NA_KNOB_INT,
    NA_KNOB_FLAG,
    NA_KNOB_GOALS,
};

struct NaKnobDef {
    const char* name;   // the registry name, e.g. "focus.fight"
    NaKnobKind kind;
    const char* field;  // the engine field, as the orchestrator's registry names it
    int int_field;      // NaPolicyIntField for NA_KNOB_INT, else -1
    int lo;
    int hi;
    uint32_t mask;      // the player_flags bit for NA_KNOB_FLAG, else 0
};

// The frozen registry. Adding a knob is a reviewed change here AND in the orchestrator.
extern const NaKnobDef NA_KNOBS[];
extern const int NA_KNOB_COUNT;

// The AIGoal values a policy may request, by wire name.
struct NaGoalType {
    const char* name;
    int type;
};
extern const NaGoalType NA_GOAL_TYPES[];
extern const int NA_GOAL_TYPE_COUNT;
static const int NA_GOAL_PRIORITY_LO = 1;
static const int NA_GOAL_PRIORITY_HI = 20;

struct NaPolicyGoal {
    int type;      // AIGoal value
    int x;
    int y;
    int priority;
};

// One faction's overlay as received. Only what a seat SET; never anything Thinker computed.
struct NaPolicy {
    bool present;            // the faction is in na_policy
    uint32_t int_set;        // bit (1 << NaPolicyIntField) per int knob carried
    int32_t ints[NA_PF_INT_COUNT];
    uint32_t flag_mask;      // player_flags bits the overlay decides
    uint32_t flag_bits;      // their values, within flag_mask
    bool goals_set;          // target.goals carried (possibly an empty list)
    int goal_count;
    NaPolicyGoal goals[NA_POLICY_MAX_GOALS];
};

struct NaPolicySet {
    bool present;            // the reply carried na_policy
    char hash[65];           // policy_hash, lowercase hex; "" when absent
    NaPolicy faction[NA_POLICY_FACTIONS];
};

enum NaPolicyParse {
    NA_POLICY_ABSENT,        // no na_policy key: the empty overlay
    NA_POLICY_OK,            // parsed and every knob checked
    NA_POLICY_REFUSED,       // present but unacceptable; `out` is the empty overlay
};

/*
Parse the overlay out of a `/turn` reply body. `out` is always fully written: on ABSENT and
REFUSED it is the empty overlay. `why`, when non-null, receives a short reason on REFUSED.
*/
NaPolicyParse na_policy_parse(const char* json, NaPolicySet* out, char* why, size_t why_cap);

// The registry entry for a knob name, or null.
const NaKnobDef* na_policy_knob(const char* name);
