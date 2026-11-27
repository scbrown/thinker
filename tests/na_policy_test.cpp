/*
Standalone tests for the policy overlay parser (src/na_policy.cpp).

Like na_http_test, this links no engine headers and runs under Wine with no SMAC install.
The engine-side apply (neural.cpp, na_policy_receive) needs a game to exercise; the parser
is the half that decides what the engine is told, so it is the half pinned here.

    na_policy_test [<base-url>]   with a url, also reads a reply over the wire from the stub
*/

#include "../src/na_http.h"
#include "../src/na_policy.h"

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

// The orchestrator's reply for one seated faction, as FastAPI serialises it.
static const char* REPLY =
    "{\"turn\":12,\"expected\":3,\"grounding\":{\"status\":\"primed\",\"rows\":2},"
    "\"na_policy\":[{\"faction_id\":3,\"knobs\":{\"focus.fight\":-1,\"focus.tech\":1,"
    "\"strategy.sea_power\":true,\"strategy.attack_enemy_hq\":false,\"target.base\":41,"
    "\"target.goals\":[{\"priority\":5,\"type\":\"attack\",\"x\":10,\"y\":12},"
    "{\"priority\":20,\"type\":\"colonize\",\"x\":0,\"y\":511}]}},"
    "{\"faction_id\":5,\"knobs\":{\"target.base\":-1}}],"
    "\"policy_hash\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"}";

static void check_reply(const NaPolicySet& s, NaPolicyParse got, const char* where) {
    char what[160];
    snprintf(what, sizeof(what), "%s: the full overlay parses", where);
    check(got == NA_POLICY_OK && s.present, what);
    const NaPolicy& f3 = s.faction[3];
    snprintf(what, sizeof(what), "%s: faction 3's ints carry exactly what was set", where);
    check(f3.present && f3.int_set == ((1u << NA_PF_AI_FIGHT) | (1u << NA_PF_AI_TECH)
                                       | (1u << NA_PF_ATTACK_TARGET))
              && f3.ints[NA_PF_AI_FIGHT] == -1 && f3.ints[NA_PF_AI_TECH] == 1
              && f3.ints[NA_PF_ATTACK_TARGET] == 41,
          what);
    snprintf(what, sizeof(what), "%s: flags decide both the set and the cleared bit", where);
    check(f3.flag_mask == (0x8000000u | 0x200000u) && f3.flag_bits == 0x8000000u, what);
    snprintf(what, sizeof(what), "%s: goals map to AIGoal values in order", where);
    check(f3.goals_set && f3.goal_count == 2 && f3.goals[0].type == 0 && f3.goals[0].x == 10
              && f3.goals[0].y == 12 && f3.goals[0].priority == 5 && f3.goals[1].type == 8
              && f3.goals[1].y == 511 && f3.goals[1].priority == 20,
          what);
    snprintf(what, sizeof(what), "%s: a faction absent from na_policy has no overlay", where);
    check(!s.faction[1].present && !s.faction[2].present && !s.faction[4].present
              && s.faction[5].present && s.faction[5].int_set == (1u << NA_PF_ATTACK_TARGET)
              && s.faction[5].ints[NA_PF_ATTACK_TARGET] == -1,
          what);
    snprintf(what, sizeof(what), "%s: policy_hash is carried verbatim", where);
    check(strcmp(s.hash, "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef") == 0,
          what);
}

static NaPolicyParse parse(const char* json, NaPolicySet* s) {
    char why[96];
    return na_policy_parse(json, s, why, sizeof(why));
}

static bool empty(const NaPolicySet& s) {
    NaPolicySet zero;
    memset(&zero, 0, sizeof(zero));
    return memcmp(&s, &zero, sizeof(s)) == 0;
}

static const char* HASH = "\"policy_hash\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"";

// A one-knob reply for faction 2, to try one value at a time.
static NaPolicyParse one(const char* knobs, NaPolicySet* s) {
    static char buf[1024];
    snprintf(buf, sizeof(buf), "{\"turn\":1,\"na_policy\":[{\"faction_id\":2,\"knobs\":{%s}}],%s}",
             knobs, HASH);
    return parse(buf, s);
}

static void test_parse() {
    NaPolicySet s;
    check_reply(s, parse(REPLY, &s), "inline");

    // Today's reply, with nothing set: the overlay is empty and nothing is refused.
    check(parse("{\"turn\":12,\"expected\":3}", &s) == NA_POLICY_ABSENT && empty(s),
          "a reply without na_policy is the empty overlay");
    check(parse("{\"turn\":12,\"expected\":3,\"grounding\":{\"status\":\"degraded\","
                "\"reason\":\"na_policy faction_id knobs\"}}", &s) == NA_POLICY_ABSENT,
          "policy words inside another field are not a policy");
    check(parse(nullptr, &s) == NA_POLICY_ABSENT && parse("", &s) == NA_POLICY_ABSENT
              && parse("not json", &s) == NA_POLICY_ABSENT,
          "no body or a garbled one without na_policy is the empty overlay");

    // The registry's bounds, refused rather than clamped.
    check(one("\"focus.fight\":-1", &s) == NA_POLICY_OK, "focus.fight -1 is in bounds");
    check(one("\"focus.fight\":2", &s) == NA_POLICY_REFUSED && empty(s),
          "focus.fight 2 is refused and leaves the empty overlay");
    check(one("\"focus.growth\":-1", &s) == NA_POLICY_REFUSED, "focus.growth -1 is refused");
    check(one("\"focus.tech\":1.0", &s) == NA_POLICY_REFUSED, "a fractional knob is refused");
    check(one("\"focus.wealth\":true", &s) == NA_POLICY_REFUSED, "a boolean int knob is refused");
    check(one("\"target.base\":511", &s) == NA_POLICY_OK, "target.base 511 is in bounds");
    check(one("\"target.base\":512", &s) == NA_POLICY_REFUSED, "target.base 512 is refused");
    check(one("\"target.base\":-2", &s) == NA_POLICY_REFUSED, "target.base -2 is refused");
    check(one("\"strategy.land_power\":1", &s) == NA_POLICY_REFUSED,
          "a flag knob takes true or false only");
    check(one("\"strategy.coop_with_human\":true", &s) == NA_POLICY_REFUSED,
          "a diplomatic bit is not a knob");
    check(one("\"plans.main_region\":3", &s) == NA_POLICY_REFUSED, "an unknown knob is refused");
    check(one("\"focus.fight\":1,\"focus.fight\":0", &s) == NA_POLICY_REFUSED,
          "a repeated knob is refused");
    check(one("\"target.goals\":[]", &s) == NA_POLICY_OK && s.faction[2].goals_set
              && s.faction[2].goal_count == 0,
          "an empty goal list is a set knob with no goals");
    check(one("\"target.goals\":[{\"type\":\"terraform\",\"x\":1,\"y\":1,\"priority\":1}]", &s)
              == NA_POLICY_REFUSED,
          "a goal type outside the registry is refused");
    check(one("\"target.goals\":[{\"type\":\"attack\",\"x\":1,\"y\":1,\"priority\":21}]", &s)
              == NA_POLICY_REFUSED,
          "a goal priority above 20 is refused");
    check(one("\"target.goals\":[{\"type\":\"attack\",\"x\":1,\"y\":1}]", &s)
              == NA_POLICY_REFUSED,
          "a goal missing a field is refused");
    {
        char nine[1024] = "\"target.goals\":[";
        for (int i = 0; i < 9; i++) {
            strcat(nine, i ? "," : "");
            strcat(nine, "{\"type\":\"scout\",\"x\":1,\"y\":1,\"priority\":1}");
        }
        strcat(nine, "]");
        check(one(nine, &s) == NA_POLICY_REFUSED, "nine goals are refused; the cap is 8");
    }

    // Faction ids are the engine's: 1..7, once each.
    check(parse("{\"na_policy\":[{\"faction_id\":0,\"knobs\":{}}],"
                "\"policy_hash\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"}",
                &s) == NA_POLICY_REFUSED,
          "native life cannot carry a policy");
    check(parse("{\"na_policy\":[{\"faction_id\":8,\"knobs\":{}}],"
                "\"policy_hash\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"}",
                &s) == NA_POLICY_REFUSED,
          "faction 8 is past MaxPlayerNum");
    check(parse("{\"na_policy\":[{\"faction_id\":1,\"knobs\":{}},{\"faction_id\":1,\"knobs\":{}}],"
                "\"policy_hash\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"}",
                &s) == NA_POLICY_REFUSED,
          "a repeated faction is refused");
    check(parse("{\"na_policy\":[{\"faction_id\":1,\"knobs\":{\"focus.fight\":1}}]}", &s)
              == NA_POLICY_REFUSED && empty(s),
          "na_policy without policy_hash is refused");
    check(parse("{\"na_policy\":[],\"policy_hash\":\"abc\"}", &s) == NA_POLICY_REFUSED,
          "a policy_hash that is not sha256 hex is refused");
    check(parse("{\"na_policy\":[{\"faction_id\":1,\"knobs\":{\"focus.fight\":1}}", &s)
              == NA_POLICY_REFUSED && empty(s),
          "a truncated reply is refused, not half-applied");

    char why[96];
    na_policy_parse("{\"na_policy\":[{\"faction_id\":2,\"knobs\":{\"nope\":1}}],"
                    "\"policy_hash\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"}",
                    &s, why, sizeof(why));
    check(strcmp(why, "unknown knob") == 0, "a refusal says why");
}

static void test_registry() {
    check(NA_KNOB_COUNT == 14, "the registry has the 14 first-cut knobs");
    const NaKnobDef* k = na_policy_knob("strategy.attack_objectives");
    check(k && k->kind == NA_KNOB_FLAG && k->mask == 0x80000000u,
          "a flag knob names its player_flags bit");
    check(na_policy_knob("strategy.team_up_vs_human") == nullptr,
          "no diplomatic or atrocity bit is in the registry");
}

static void test_live(const char* base) {
    NaBuf resp;
    bool ok = na_http_post(base, "/turn-policy", "{\"turn\":12,\"expected\":[]}", 2000, &resp);
    check(ok, "the stub answers a /turn with an overlay");
    if (ok) {
        NaPolicySet s;
        check_reply(s, parse(resp.data, &s), "over the wire");
        na_buf_free(&resp);
    }
}

int main(int argc, char** argv) {
    test_registry();
    test_parse();
    if (argc > 1) {
        test_live(argv[1]);
    } else {
        printf("      (no base url given; the wire read is skipped)\n");
    }
    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
