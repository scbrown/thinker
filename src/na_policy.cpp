/*
The policy overlay's registry mirror and reply parser. See na_policy.h for the contract.

A real (small) JSON reader rather than na_json_string's key search, because the overlay is
nested and a key search would match "faction_id" inside any other object in the reply. It
reads the top-level object only, takes `na_policy` and `policy_hash`, and skips everything
else structurally. Nothing here allocates.
*/

#include "na_policy.h"

#include <string.h>

const NaKnobDef NA_KNOBS[] = {
    {"focus.fight",   NA_KNOB_INT, "Faction.AI_fight",  NA_PF_AI_FIGHT,  -1, 1, 0},
    {"focus.growth",  NA_KNOB_INT, "Faction.AI_growth", NA_PF_AI_GROWTH,  0, 1, 0},
    {"focus.tech",    NA_KNOB_INT, "Faction.AI_tech",   NA_PF_AI_TECH,    0, 1, 0},
    {"focus.wealth",  NA_KNOB_INT, "Faction.AI_wealth", NA_PF_AI_WEALTH,  0, 1, 0},
    {"focus.power",   NA_KNOB_INT, "Faction.AI_power",  NA_PF_AI_POWER,   0, 1, 0},
    {"strategy.attack_enemy_hq",   NA_KNOB_FLAG, "Faction.player_flags", -1, 0, 0, 0x200000u},
    {"strategy.land_power",        NA_KNOB_FLAG, "Faction.player_flags", -1, 0, 0, 0x4000000u},
    {"strategy.sea_power",         NA_KNOB_FLAG, "Faction.player_flags", -1, 0, 0, 0x8000000u},
    {"strategy.air_power",         NA_KNOB_FLAG, "Faction.player_flags", -1, 0, 0, 0x10000000u},
    {"strategy.search_objectives", NA_KNOB_FLAG, "Faction.player_flags", -1, 0, 0, 0x20000000u},
    {"strategy.defend_objectives", NA_KNOB_FLAG, "Faction.player_flags", -1, 0, 0, 0x40000000u},
    {"strategy.attack_objectives", NA_KNOB_FLAG, "Faction.player_flags", -1, 0, 0, 0x80000000u},
    {"target.base",  NA_KNOB_INT,   "Faction.base_id_attack_target", NA_PF_ATTACK_TARGET,
        -1, NA_POLICY_MAX_BASE_ID, 0},
    {"target.goals", NA_KNOB_GOALS, "Faction.goals", -1, 0, NA_POLICY_MAX_GOALS, 0},
};
const int NA_KNOB_COUNT = sizeof(NA_KNOBS) / sizeof(NA_KNOBS[0]);

const NaGoalType NA_GOAL_TYPES[] = {
    {"attack", 0},    // AI_GOAL_ATTACK
    {"defend", 2},    // AI_GOAL_DEFEND
    {"scout", 3},     // AI_GOAL_SCOUT
    {"colonize", 8},  // AI_GOAL_COLONIZE
};
const int NA_GOAL_TYPE_COUNT = sizeof(NA_GOAL_TYPES) / sizeof(NA_GOAL_TYPES[0]);

const NaKnobDef* na_policy_knob(const char* name) {
    for (int i = 0; i < NA_KNOB_COUNT; i++) {
        if (strcmp(NA_KNOBS[i].name, name) == 0) {
            return &NA_KNOBS[i];
        }
    }
    return nullptr;
}

namespace {

const int MAX_DEPTH = 32;

struct Reader {
    const char* p;
    char* why;
    size_t why_cap;
    bool failed;
};

bool fail(Reader* r, const char* why) {
    if (!r->failed) {
        r->failed = true;
        if (r->why && r->why_cap) {
            strncpy(r->why, why, r->why_cap - 1);
            r->why[r->why_cap - 1] = '\0';
        }
    }
    return false;
}

void ws(Reader* r) {
    while (*r->p == ' ' || *r->p == '\t' || *r->p == '\n' || *r->p == '\r') {
        r->p++;
    }
}

bool eat(Reader* r, char c) {
    ws(r);
    if (*r->p != c) {
        return false;
    }
    r->p++;
    return true;
}

// A string into `out`. \u escapes outside ASCII are refused into `out` but skipped when
// `out` is null; nothing the overlay names needs them.
bool string(Reader* r, char* out, size_t cap) {
    ws(r);
    if (*r->p != '"') {
        return fail(r, "expected a string");
    }
    r->p++;
    size_t n = 0;
    while (*r->p && *r->p != '"') {
        char c = *r->p++;
        if ((unsigned char)c < 0x20) {
            return fail(r, "control character in string");
        }
        if (c == '\\') {
            char e = *r->p++;
            switch (e) {
                case '"': c = '"'; break;
                case '\\': c = '\\'; break;
                case '/': c = '/'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                case 'u': {
                    unsigned v = 0;
                    for (int i = 0; i < 4; i++) {
                        char h = *r->p++;
                        v <<= 4;
                        if (h >= '0' && h <= '9') v |= (unsigned)(h - '0');
                        else if (h >= 'a' && h <= 'f') v |= (unsigned)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') v |= (unsigned)(h - 'A' + 10);
                        else return fail(r, "bad \\u escape");
                    }
                    if (out && (v == 0 || v > 0x7f)) {
                        return fail(r, "non-ASCII escape in a policy name");
                    }
                    c = (char)v;
                    break;
                }
                default:
                    return fail(r, "bad escape");
            }
        }
        if (out) {
            if (n + 1 >= cap) {
                return fail(r, "string too long");
            }
            out[n++] = c;
        }
    }
    if (*r->p != '"') {
        return fail(r, "unterminated string");
    }
    r->p++;
    if (out) {
        out[n] = '\0';
    }
    return true;
}

// An integer. A fraction or exponent is refused: every knob is integral, and 1.0 accepted
// here would be a value the orchestrator never sends.
bool integer(Reader* r, long* out) {
    ws(r);
    const char* s = r->p;
    bool neg = false;
    if (*s == '-') {
        neg = true;
        s++;
    }
    if (*s < '0' || *s > '9') {
        return fail(r, "expected an integer");
    }
    long v = 0;
    int digits = 0;
    while (*s >= '0' && *s <= '9') {
        if (++digits > 9) {
            return fail(r, "integer out of range");
        }
        v = v * 10 + (*s - '0');
        s++;
    }
    if (*s == '.' || *s == 'e' || *s == 'E') {
        return fail(r, "expected an integer, got a fraction");
    }
    r->p = s;
    *out = neg ? -v : v;
    return true;
}

bool literal(Reader* r, const char* word) {
    size_t n = strlen(word);
    if (strncmp(r->p, word, n) != 0) {
        return fail(r, "bad literal");
    }
    r->p += n;
    return true;
}

bool boolean(Reader* r, bool* out) {
    ws(r);
    if (*r->p == 't') {
        *out = true;
        return literal(r, "true");
    }
    if (*r->p == 'f') {
        *out = false;
        return literal(r, "false");
    }
    return fail(r, "expected true or false");
}

bool skip(Reader* r, int depth) {
    if (depth > MAX_DEPTH) {
        return fail(r, "nested too deeply");
    }
    ws(r);
    char c = *r->p;
    if (c == '"') {
        return string(r, nullptr, 0);
    }
    if (c == '{' || c == '[') {
        const char close = c == '{' ? '}' : ']';
        r->p++;
        if (eat(r, close)) {
            return true;
        }
        do {
            if (c == '{') {
                if (!string(r, nullptr, 0) || !eat(r, ':')) {
                    return fail(r, "bad object");
                }
            }
            if (!skip(r, depth + 1)) {
                return false;
            }
        } while (eat(r, ','));
        return eat(r, close) || fail(r, "unclosed container");
    }
    if (c == 't') return literal(r, "true");
    if (c == 'f') return literal(r, "false");
    if (c == 'n') return literal(r, "null");
    if (c == '-' || (c >= '0' && c <= '9')) {
        r->p++;
        while ((*r->p >= '0' && *r->p <= '9') || *r->p == '.' || *r->p == 'e'
               || *r->p == 'E' || *r->p == '+' || *r->p == '-') {
            r->p++;
        }
        return true;
    }
    return fail(r, "expected a value");
}

bool bounded(Reader* r, long lo, long hi, int* out) {
    long v;
    if (!integer(r, &v)) {
        return false;
    }
    if (v < lo || v > hi) {
        return fail(r, "value outside the registry bounds");
    }
    *out = (int)v;
    return true;
}

bool goal(Reader* r, NaPolicyGoal* g) {
    if (!eat(r, '{')) {
        return fail(r, "goal is not an object");
    }
    bool have_type = false, have_x = false, have_y = false, have_pr = false;
    if (!eat(r, '}')) {
        do {
            char key[16];
            if (!string(r, key, sizeof(key)) || !eat(r, ':')) {
                return fail(r, "bad goal field");
            }
            bool* seen;
            if (strcmp(key, "type") == 0) {
                char name[16];
                if (!string(r, name, sizeof(name))) {
                    return false;
                }
                g->type = -1;
                for (int i = 0; i < NA_GOAL_TYPE_COUNT; i++) {
                    if (strcmp(NA_GOAL_TYPES[i].name, name) == 0) {
                        g->type = NA_GOAL_TYPES[i].type;
                    }
                }
                if (g->type < 0) {
                    return fail(r, "goal type not in the registry");
                }
                seen = &have_type;
            } else if (strcmp(key, "x") == 0) {
                if (!bounded(r, 0, NA_POLICY_MAX_COORD, &g->x)) return false;
                seen = &have_x;
            } else if (strcmp(key, "y") == 0) {
                if (!bounded(r, 0, NA_POLICY_MAX_COORD, &g->y)) return false;
                seen = &have_y;
            } else if (strcmp(key, "priority") == 0) {
                if (!bounded(r, NA_GOAL_PRIORITY_LO, NA_GOAL_PRIORITY_HI, &g->priority)) {
                    return false;
                }
                seen = &have_pr;
            } else {
                return fail(r, "unknown goal field");
            }
            if (*seen) {
                return fail(r, "repeated goal field");
            }
            *seen = true;
        } while (eat(r, ','));
        if (!eat(r, '}')) {
            return fail(r, "unclosed goal");
        }
    }
    if (!have_type || !have_x || !have_y || !have_pr) {
        return fail(r, "goal is missing a field");
    }
    return true;
}

bool knob_value(Reader* r, const NaKnobDef* k, NaPolicy* f) {
    switch (k->kind) {
        case NA_KNOB_INT: {
            const uint32_t bit = 1u << k->int_field;
            if (f->int_set & bit) {
                return fail(r, "repeated knob");
            }
            if (!bounded(r, k->lo, k->hi, &f->ints[k->int_field])) {
                return false;
            }
            f->int_set |= bit;
            return true;
        }
        case NA_KNOB_FLAG: {
            if (f->flag_mask & k->mask) {
                return fail(r, "repeated knob");
            }
            bool on;
            if (!boolean(r, &on)) {
                return false;
            }
            f->flag_mask |= k->mask;
            if (on) {
                f->flag_bits |= k->mask;
            }
            return true;
        }
        case NA_KNOB_GOALS: {
            if (f->goals_set) {
                return fail(r, "repeated knob");
            }
            if (!eat(r, '[')) {
                return fail(r, "target.goals is not a list");
            }
            f->goals_set = true;
            f->goal_count = 0;
            if (eat(r, ']')) {
                return true;
            }
            do {
                if (f->goal_count >= k->hi) {
                    return fail(r, "too many goals");
                }
                if (!goal(r, &f->goals[f->goal_count])) {
                    return false;
                }
                f->goal_count++;
            } while (eat(r, ','));
            return eat(r, ']') || fail(r, "unclosed goal list");
        }
    }
    return fail(r, "unknown knob kind");
}

bool knobs(Reader* r, NaPolicy* f) {
    if (!eat(r, '{')) {
        return fail(r, "knobs is not an object");
    }
    if (eat(r, '}')) {
        return true;
    }
    do {
        char name[48];
        if (!string(r, name, sizeof(name)) || !eat(r, ':')) {
            return fail(r, "bad knob name");
        }
        const NaKnobDef* k = na_policy_knob(name);
        if (!k) {
            return fail(r, "unknown knob");
        }
        if (!knob_value(r, k, f)) {
            return false;
        }
    } while (eat(r, ','));
    return eat(r, '}') || fail(r, "unclosed knobs");
}

bool entry(Reader* r, NaPolicySet* out) {
    if (!eat(r, '{')) {
        return fail(r, "na_policy entry is not an object");
    }
    int faction_id = -1;
    NaPolicy f;
    memset(&f, 0, sizeof(f));
    bool have_knobs = false;
    if (!eat(r, '}')) {
        do {
            char key[16];
            if (!string(r, key, sizeof(key)) || !eat(r, ':')) {
                return fail(r, "bad entry field");
            }
            if (strcmp(key, "faction_id") == 0) {
                if (faction_id >= 0) {
                    return fail(r, "repeated faction_id");
                }
                if (!bounded(r, 1, NA_POLICY_FACTIONS - 1, &faction_id)) {
                    return false;
                }
            } else if (strcmp(key, "knobs") == 0) {
                if (have_knobs) {
                    return fail(r, "repeated knobs");
                }
                if (!knobs(r, &f)) {
                    return false;
                }
                have_knobs = true;
            } else {
                return fail(r, "unknown entry field");
            }
        } while (eat(r, ','));
        if (!eat(r, '}')) {
            return fail(r, "unclosed entry");
        }
    }
    if (faction_id < 0 || !have_knobs) {
        return fail(r, "entry needs faction_id and knobs");
    }
    if (out->faction[faction_id].present) {
        return fail(r, "faction repeated in na_policy");
    }
    f.present = true;
    out->faction[faction_id] = f;
    return true;
}

bool is_hash(const char* s) {
    if (strlen(s) != 64) {
        return false;
    }
    for (const char* c = s; *c; c++) {
        if (!((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'f'))) {
            return false;
        }
    }
    return true;
}

}  // namespace

NaPolicyParse na_policy_parse(const char* json, NaPolicySet* out, char* why, size_t why_cap) {
    memset(out, 0, sizeof(*out));
    if (why && why_cap) {
        why[0] = '\0';
    }
    if (!json) {
        return NA_POLICY_ABSENT;
    }
    Reader r = {json, why, why_cap, false};
    NaPolicySet got;
    memset(&got, 0, sizeof(got));
    bool have_policy = false, have_hash = false;

    if (!eat(&r, '{')) {
        fail(&r, "reply is not an object");
    } else if (!eat(&r, '}')) {
        do {
            char key[32];
            // A top-level key longer than ours cannot be ours; skip it as a value would be.
            const char* at = r.p;
            if (!string(&r, key, sizeof(key))) {
                r.failed = false;
                r.p = at;
                if (!string(&r, nullptr, 0)) break;
                key[0] = '\0';
            }
            if (!eat(&r, ':')) {
                fail(&r, "bad reply object");
                break;
            }
            if (strcmp(key, "na_policy") == 0) {
                if (have_policy) {
                    fail(&r, "repeated na_policy");
                    break;
                }
                have_policy = true;
                if (!eat(&r, '[')) {
                    fail(&r, "na_policy is not a list");
                    break;
                }
                if (!eat(&r, ']')) {
                    do {
                        if (!entry(&r, &got)) break;
                    } while (eat(&r, ','));
                    if (r.failed || !eat(&r, ']')) {
                        fail(&r, "unclosed na_policy");
                        break;
                    }
                }
            } else if (strcmp(key, "policy_hash") == 0) {
                if (have_hash) {
                    fail(&r, "repeated policy_hash");
                    break;
                }
                have_hash = true;
                if (!string(&r, got.hash, sizeof(got.hash)) || !is_hash(got.hash)) {
                    fail(&r, "policy_hash is not a sha256 hex digest");
                    break;
                }
            } else if (!skip(&r, 1)) {
                break;
            }
        } while (eat(&r, ','));
        if (!r.failed && !eat(&r, '}')) {
            fail(&r, "unclosed reply");
        }
    }

    if (r.failed) {
        return have_policy ? NA_POLICY_REFUSED : NA_POLICY_ABSENT;
    }
    if (!have_policy && !have_hash) {
        return NA_POLICY_ABSENT;
    }
    if (have_policy != have_hash) {
        fail(&r, "na_policy and policy_hash travel together");
        return NA_POLICY_REFUSED;
    }
    got.present = true;
    *out = got;
    return NA_POLICY_OK;
}
