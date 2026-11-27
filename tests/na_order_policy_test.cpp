/*
Standalone tests for the seat order policy (aegis-8fye.9).

na_order_policy.h links no engine headers, so the rules a seat is held to on the command channel
build into a small exe and run under Wine with no SMAC install. What a played game would add is
the reading of engine globals, and that half is a source property checked by
tests/check_order_gates.py: every apply* verb derives its faction from the entity and passes
na_apply_gate before it mutates anything.

Each case below composes the two gates the way neural.cpp's handlers do: operator gate first,
then the order gate with the ENTITY'S faction (the base's owner for apply / apply-hurry, the
named faction for apply-tech / apply-se).

    na_order_policy_test        exit 0 all passed, 1 any failed
*/

#include "../src/na_order_policy.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;
static int checks = 0;

static void check(bool ok, const char* what) {
    checks++;
    if (!ok) {
        failures++;
        printf("FAIL  %s\n", what);
    } else {
        printf("ok    %s\n", what);
    }
}

static bool contains(const char* s, const char* sub) {
    return s && sub && strstr(s, sub) != nullptr;
}

// A tiny board: base 0 belongs to faction 1, base 1 to faction 2.
static const int base_owner[] = {1, 2};

// What the apply handler does: operator gate, then the base OWNER through the order gate.
static bool apply_base(const NaPolicyState& s, const char* verb, int base_id,
                       char* why, size_t n) {
    why[0] = '\0';
    if (!na_policy_operator(s, verb, why, n)) {
        return false;
    }
    return na_policy_order(s, base_owner[base_id], why, n);
}

// What apply-tech / apply-se do: operator gate, then the named faction through the order gate.
static bool apply_faction(const NaPolicyState& s, const char* verb, int faction_id,
                          char* why, size_t n) {
    why[0] = '\0';
    if (!na_policy_operator(s, verb, why, n)) {
        return false;
    }
    return na_policy_order(s, faction_id, why, n);
}

int main() {
    char why[160];

    // Factions 1 and 2 both routed; it is faction 1's turn.
    NaPolicyState off = {0, (1 << 1) | (1 << 2), 1, 0};
    NaPolicyState on = off;
    on.operator_verbs = 1;

    // ---- operator flag OFF: every apply* and fixture verb refused, with the reason ----
    const char* operator_verbs[] = {
        "apply", "apply-tech", "apply-se", "apply-hurry",
        "withdraw", "unit-turn", "settle-mode",
        "probe-withdrawal-demand", "probe-foreign-unit",
    };
    for (const char* v : operator_verbs) {
        char what[96];
        why[0] = '\0';
        bool ok = na_policy_operator(off, v, why, sizeof(why));
        snprintf(what, sizeof(what), "%s refused with na_operator_verbs=0", v);
        check(!ok && contains(why, "na_operator_verbs=1") && contains(why, v), what);
        snprintf(what, sizeof(what), "%s allowed past the operator gate with na_operator_verbs=1", v);
        check(na_policy_operator(on, v, why, sizeof(why)), what);
    }
    check(!apply_base(off, "apply", 0, why, sizeof(why)),
          "apply to OWN base refused with flag off (operator gate binds before ownership)");
    check(!apply_faction(off, "apply-tech", 1, why, sizeof(why)),
          "apply-tech for own faction refused with flag off");

    // ---- operator flag ON: own entity on own turn works ----
    check(apply_base(on, "apply", 0, why, sizeof(why)), "apply to own base on own turn works");
    check(apply_base(on, "apply-hurry", 0, why, sizeof(why)),
          "apply-hurry at own base on own turn works");
    check(apply_faction(on, "apply-tech", 1, why, sizeof(why)),
          "apply-tech for own faction on own turn works");
    check(apply_faction(on, "apply-se", 1, why, sizeof(why)),
          "apply-se for own faction on own turn works");

    // ---- a seat cannot act on another faction's entity, even with the flag on ----
    check(!apply_base(on, "apply", 1, why, sizeof(why)) && contains(why, "not faction 2's turn"),
          "apply to ANOTHER faction's base refused (owner derived from the base)");
    check(!apply_base(on, "apply-hurry", 1, why, sizeof(why)) && contains(why, "not faction 2"),
          "apply-hurry at another faction's base refused");
    check(!apply_faction(on, "apply-tech", 2, why, sizeof(why)) && contains(why, "not faction 2"),
          "apply-tech naming another faction refused (spoofed faction_id)");
    check(!apply_faction(on, "apply-se", 2, why, sizeof(why)) && contains(why, "not faction 2"),
          "apply-se naming another faction refused (spoofed faction_id)");

    // An unrouted faction is a configuration error, not a turn error, even on its own turn.
    NaPolicyState unrouted = on;
    unrouted.current_faction = 3;
    check(!apply_faction(unrouted, "apply-tech", 3, why, sizeof(why))
          && contains(why, "not routed"),
          "apply-tech for an unrouted faction refused as not routed");
    check(!apply_faction(on, "apply-se", 0, why, sizeof(why)) && contains(why, "not routed"),
          "faction 0 (native life) is never routed");
    check(!apply_faction(on, "apply-se", 8, why, sizeof(why)) && contains(why, "not routed"),
          "faction 8 is out of range and never routed");

    NaPolicyState halted = on;
    halted.halted = 1;
    check(!apply_base(halted, "apply", 0, why, sizeof(why)) && contains(why, "halted"),
          "apply refused while the game is halted");

    // ---- the existing gated verbs (move/skip/build) are unchanged ----
    // Same three answers, same wording, and the operator flag does not enter into them.
    for (int flag = 0; flag <= 1; flag++) {
        NaPolicyState s = flag ? on : off;
        char what[96];
        snprintf(what, sizeof(what), "order gate: own faction on own turn passes (flag=%d)", flag);
        check(na_policy_order(s, 1, why, sizeof(why)), what);
        snprintf(what, sizeof(what), "order gate: other faction refused, same text (flag=%d)", flag);
        check(!na_policy_order(s, 2, why, sizeof(why))
              && strcmp(why, "not faction 2's turn (current is 1)") == 0, what);
        snprintf(what, sizeof(what), "order gate: unrouted refused, same text (flag=%d)", flag);
        check(!na_policy_order(s, 4, why, sizeof(why))
              && strcmp(why, "faction 4 is not routed to the orchestrator (llm_factions)") == 0,
              what);
        NaPolicyState h = s;
        h.halted = 1;
        snprintf(what, sizeof(what), "order gate: halted refused, same text (flag=%d)", flag);
        check(!na_policy_order(h, 1, why, sizeof(why))
              && strcmp(why, "game is halted; retry") == 0, what);
    }

    // ---- verb classification: the list neural.cpp is checked against ----
    check(na_policy_is_operator_verb("apply 3 unit:1"), "'apply 3 unit:1' is an operator verb");
    check(na_policy_is_operator_verb("apply-se 1 se:none"), "'apply-se' is an operator verb");
    check(na_policy_is_operator_verb("unit-turn 1"), "'unit-turn' is an operator verb");
    check(!na_policy_is_operator_verb("withdraw-stats"), "'withdraw-stats' is read-only, not operator");
    check(!na_policy_is_operator_verb("move 1 2 3"), "'move' is a seat verb");
    check(!na_policy_is_operator_verb("build 0 -5"), "'build' is a seat verb");
    check(!na_policy_is_operator_verb("observe-tech 1"), "'observe-tech' is not operator");
    check(!na_policy_is_operator_verb("applyx"), "a verb is matched as a whole word");

    printf("\n%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
