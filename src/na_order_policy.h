#pragma once

#include <stdio.h>
#include <string.h>

/*
Who may give which order on the command channel (aegis-8fye.9).

Pure functions over plain ints, with NO engine headers, for the same reason na_http.cpp has
none: it builds into a standalone exe that runs under Wine with no SMAC install, so the policy
a seat is held to can regress in CI rather than only in a played game. neural.cpp reads the
engine globals and passes them in; nothing here touches game memory.

Two gates, and they answer different questions:

  na_policy_operator  is this verb a seat's at all? The apply* family and the fixture verbs are
                      the operator's, refused unless na_operator_verbs=1. Checked FIRST, before
                      parsing, so a seat learns nothing about the board from the error text of a
                      verb it may not use.

  na_policy_order     may THIS faction be ordered right now? halted / not routed / not its turn,
                      worded so an agent can tell a retry from a configuration error from a
                      fairness refusal (see na_order_gate in neural.cpp).

The faction passed to na_policy_order is the ENTITY'S: the base's owner, the vehicle's owner.
For apply-tech and apply-se the entity is the faction itself, so the argument is used — and is
safe only because the gate then requires it to be a routed faction whose turn it is. A seat
naming another faction is refused as "not its turn", never applied.
*/

struct NaPolicyState {
    int halted;            // *GameHalted
    int routed_mask;       // conf.na.llm_factions
    int current_faction;   // *CurrentPlayerFaction
    int operator_verbs;    // conf.na.operator_verbs
};

// The verbs a seat may not use unless the operator enabled them. A prefix match against the
// command line, longest-first is unnecessary because every entry is matched as a whole word.
static inline bool na_policy_is_operator_verb(const char* line) {
    static const char* const verbs[] = {
        "apply", "apply-tech", "apply-se", "apply-hurry",
        "withdraw", "unit-turn", "settle-mode",
        "probe-withdrawal-demand", "probe-foreign-unit",
    };
    if (!line) {
        return false;
    }
    size_t word = strcspn(line, " \t");
    for (const char* v : verbs) {
        if (strlen(v) == word && strncmp(line, v, word) == 0) {
            return true;
        }
    }
    return false;
}

static inline bool na_policy_routed(const NaPolicyState& s, int faction_id) {
    return faction_id > 0 && faction_id < 8 && (s.routed_mask & (1 << faction_id)) != 0;
}

static inline bool na_policy_operator(const NaPolicyState& s, const char* verb,
                                      char* why, size_t n) {
    if (s.operator_verbs) {
        return true;
    }
    snprintf(why, n, "%s is an operator verb; refused unless na_operator_verbs=1 in thinker.ini",
             verb);
    return false;
}

static inline bool na_policy_order(const NaPolicyState& s, int faction_id, char* why, size_t n) {
    if (s.halted) {
        snprintf(why, n, "game is halted; retry");
        return false;
    }
    if (!na_policy_routed(s, faction_id)) {
        snprintf(why, n, "faction %d is not routed to the orchestrator (llm_factions)", faction_id);
        return false;
    }
    if (faction_id != s.current_faction) {
        snprintf(why, n, "not faction %d's turn (current is %d)", faction_id, s.current_faction);
        return false;
    }
    return true;
}
