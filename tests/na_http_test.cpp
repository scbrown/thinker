/*
Standalone tests for the orchestrator wire.

`na_http.cpp` links no engine headers, which is what makes this possible: the
DLL's one piece of network code can be built into a small exe and run under Wine
against a throwaway Python server, with no SMAC install and no game running.
That matters because every other part of the adapter can only be tested by
playing, and this is the part most likely to be wrong in a way that stalls a turn.

Built by `just na-test`, which also starts and stops the server.

    na_http_test <base-url>      e.g. na_http_test http://127.0.0.1:8099
*/

#include "../src/na_http.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>

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

static void check_str(const char* got, const char* want, const char* what) {
    bool ok = got && want && strcmp(got, want) == 0;
    if (!ok) {
        printf("      got %s want %s\n", got ? got : "(null)", want ? want : "(null)");
    }
    check(ok, what);
}

static void test_buf() {
    NaBuf b;
    na_buf_init(&b);
    na_buf_puts(&b, "a");
    na_buf_printf(&b, "%d", 42);
    na_buf_puts(&b, "z");
    check_str(b.data, "a42z", "buf appends in order");
    na_buf_free(&b);

    // Past the 4096 initial reservation, so the doubling path runs. A world
    // view with a full action space is well past this.
    na_buf_init(&b);
    for (int i = 0; i < 5000; i++) {
        na_buf_puts(&b, "x");
    }
    check(b.len == 5000 && b.data[4999] == 'x' && b.data[5000] == '\0',
          "buf grows past the initial capacity and stays terminated");
    na_buf_free(&b);

    na_buf_init(&b);
    na_buf_escaped(&b, "he said \"hi\"\n\ttab\\slash");
    check_str(b.data, "he said \\\"hi\\\"\\n\\ttab\\\\slash", "escaping covers the JSON set");
    na_buf_free(&b);

    // Base names are player-editable and faction nouns come from alphax.txt, so
    // a control character in the record is reachable from user input.
    na_buf_init(&b);
    na_buf_escaped(&b, "\x01");
    check_str(b.data, "\\u0001", "control characters escape as \\u");
    na_buf_free(&b);

    na_buf_init(&b);
    na_buf_printf(&b, "%s", "");
    check(b.len == 0 && !b.failed, "empty printf leaves a usable buffer");
    na_buf_free(&b);
}

static void test_json() {
    char out[64];

    const char* orders =
        "{\"schema_version\":\"0.1\",\"choices\":[{\"action_id\":\"unit:12\","
        "\"reason\":\"needs defence\"}],\"degraded\":false,\"cited\":[]}";
    check(na_json_string(orders, "action_id", out, sizeof(out)), "finds a nested key");
    check_str(out, "unit:12", "reads the nested value");
    check(na_json_string(orders, "reason", out, sizeof(out)) && strcmp(out, "needs defence") == 0,
          "reads a value with spaces");
    check(!na_json_true(orders, "degraded"), "degraded false reads false");

    check(na_json_true("{\"degraded\":true}", "degraded"), "degraded true reads true");
    check(!na_json_true(orders, "missing"), "absent boolean reads false");

    // The failure this guards: matching "action_id" as a *value* rather than a
    // key. A reason string mentioning the field name would otherwise win,
    // because it appears earlier in the document.
    const char* tricky =
        "{\"note\":\"action_id\",\"reason\":\"the \\\"action_id\\\" was odd\","
        "\"choices\":[{\"action_id\":\"facility:4\"}]}";
    check(na_json_string(tricky, "action_id", out, sizeof(out)), "finds key past a decoy value");
    check_str(out, "facility:4", "decoy value does not win");

    check(na_json_string("{\"a\":\"x\\\"y\"}", "a", out, sizeof(out)) && strcmp(out, "x\"y") == 0,
          "unescapes an embedded quote");
    check(na_json_string("{\"a\":\"p\\\\q\"}", "a", out, sizeof(out)) && strcmp(out, "p\\q") == 0,
          "unescapes a backslash");

    check(!na_json_string(orders, "nope", out, sizeof(out)), "absent key fails");
    check(!na_json_string("{\"a\":null}", "a", out, sizeof(out)), "null value fails");
    check(!na_json_string("{\"a\":7}", "a", out, sizeof(out)), "non-string value fails");
    check(!na_json_string("{\"a\":\"unterminated", "a", out, sizeof(out)),
          "unterminated string fails");

    // Truncation must fail rather than return a prefix: a truncated id would be
    // a *different* legal-looking id, and applying it is worse than falling back.
    char tiny[4];
    check(!na_json_string("{\"a\":\"abcdefgh\"}", "a", tiny, sizeof(tiny)),
          "value too long for the buffer fails rather than truncating");
}

static void test_endpoint_rejects() {
    NaBuf resp;
    check(!na_http_post("https://127.0.0.1:8099", "/decide", "{}", 500, &resp),
          "https endpoint is refused, not downgraded");
    check(!na_http_post("", "/decide", "{}", 500, &resp), "empty endpoint fails");
    check(!na_http_post("http://127.0.0.1:0", "/decide", "{}", 500, &resp), "port 0 fails");
    check(!na_http_post("http://127.0.0.1:99999", "/decide", "{}", 500, &resp),
          "out-of-range port fails");
    check(!na_http_post("http://127.0.0.1:8099", "/decide", "{}", 0, &resp),
          "zero timeout fails without connecting");
}

static void test_live(const char* base) {
    NaBuf resp;
    char url[256];
    char out[64];

    snprintf(url, sizeof(url), "%s", base);
    bool ok = na_http_post(url, "/echo", "{\"action_id\":\"unit:7\"}", 5000, &resp);
    check(ok, "POST to a live server succeeds");
    if (ok) {
        check(na_json_string(resp.data, "action_id", out, sizeof(out))
              && strcmp(out, "unit:7") == 0, "the body round-trips");
        na_buf_free(&resp);
    }

    // A body larger than one send window — the partial-write path. A real
    // world view with a full action space is this size.
    NaBuf big;
    na_buf_init(&big);
    na_buf_puts(&big, "{\"pad\":\"");
    for (int i = 0; i < 200000; i++) {
        na_buf_puts(&big, "x");
    }
    na_buf_puts(&big, "\",\"action_id\":\"unit:9\"}");
    ok = na_http_post(url, "/echo", big.data, 15000, &resp);
    check(ok, "a body past one send window is delivered whole");
    if (ok) {
        check(na_json_string(resp.data, "action_id", out, sizeof(out))
              && strcmp(out, "unit:9") == 0, "the large body round-trips intact");
        na_buf_free(&resp);
    }
    na_buf_free(&big);

    check(!na_http_post(url, "/boom", "{}", 5000, &resp), "a 500 is a failure");
    check(!na_http_post(url, "/notfound", "{}", 5000, &resp), "a 404 is a failure");

    // The invariant-9 case: the orchestrator is alive but slow. The call must
    // give up on OUR deadline, not the server's.
    DWORD started = GetTickCount();
    ok = na_http_post(url, "/slow", "{}", 1000, &resp);
    DWORD spent = GetTickCount() - started;
    check(!ok, "a server slower than the deadline fails");
    check(spent < 3000, "and gives up near the deadline rather than waiting it out");
    printf("      slow call returned after %lums (deadline 1000ms)\n", (unsigned long)spent);

    // Nothing listening: the everyday case of "the orchestrator is not running".
    started = GetTickCount();
    check(!na_http_post("http://127.0.0.1:9", "/decide", "{}", 3000, &resp),
          "a closed port fails");
    spent = GetTickCount() - started;
    check(spent < 3000, "and fails fast rather than burning the whole deadline");
}

/*
The real exchange, against a real orchestrator.

The stub server above proves the socket code handles the four HTTP outcomes. This
proves the one thing a stub cannot: that uvicorn answers *this* client the way the
client assumes. The HTTP/1.0 request line is a bet that the server will not reply
with chunked transfer-encoding — true by spec, and worth confirming against the
actual server rather than the spec, because the DLL has no de-chunker and a
chunked reply would fail as "no action_id" with no hint as to why.

Run against `neural-amplifier serve`, which needs no game and no API key: the
default brain is the scripted one.
*/
static void test_orchestrator(const char* base) {
    NaBuf resp;
    char out[64];

    // A minimal but genuine world view: the contract's five required fields plus
    // an action space, shaped exactly as src/neural.cpp emits it.
    const char* world_view =
        "{\"schema_version\":\"0.1\",\"engine\":\"thinker\",\"scope\":\"base\","
        "\"surface_id\":\"base.production\",\"turn\":42,\"faction_id\":1,"
        "\"faction\":\"Gaians\","
        "\"trace\":{\"traceparent\":\"00-0000002a000000010000000107a1c0de-000000010000002b-01\"},"
        "\"base_id\":0,\"base\":\"Gaia's Landing\",\"x\":13,\"y\":8,"
        "\"native_choice\":-4,\"native_choice_name\":\"Recycling Tanks\","
        "\"metrics\":{\"energy_reserves\":82,\"energy_income\":14,\"base_count\":2},"
        "\"action_space\":["
        "{\"id\":\"unit:0\",\"action\":\"Colony Pod\",\"cost\":30,\"category\":\"unit\"},"
        "{\"id\":\"facility:4\",\"action\":\"Recycling Tanks\",\"cost\":40,"
        "\"category\":\"facility\"}]}";

    bool ok = na_http_post(base, "/decide", world_view, 20000, &resp);
    check(ok, "the orchestrator answers a POST /decide from this client");
    if (!ok) {
        return;
    }
    // Not chunked: if uvicorn had framed the body, this is where it would show up
    // as leading hex length markers rather than a JSON object.
    check(resp.data[0] == '{', "the reply body is bare JSON, not chunk-framed");
    check(na_json_string(resp.data, "action_id", out, sizeof(out)),
          "action_id is extractable from a real Orders reply");
    // The scripted brain falls back to the first legal action, so the id it
    // returns must be one we offered — which is what the DLL then re-checks
    // against the engine before applying.
    bool known = strcmp(out, "unit:0") == 0 || strcmp(out, "facility:4") == 0;
    if (!known) {
        printf("      action_id was %s\n", out);
    }
    check(known, "the id comes from the action space we sent");
    printf("      orchestrator chose %s\n", out);
    na_buf_free(&resp);

    // A world view the contract rejects (no faction). The DLL must read a 422 as
    // a failure and fall back, not as an empty-but-successful answer.
    ok = na_http_post(base, "/decide",
                      "{\"schema_version\":\"0.1\",\"engine\":\"thinker\","
                      "\"scope\":\"base\",\"turn\":1}", 20000, &resp);
    check(!ok, "a contract-invalid world view is a failure, not an empty answer");
}

int main(int argc, char** argv) {
    test_buf();
    test_json();
    test_endpoint_rejects();
    if (argc > 1) {
        test_live(argv[1]);
    } else {
        printf("      (no base url given — live server tests skipped)\n");
    }
    if (argc > 2) {
        test_orchestrator(argv[2]);
    } else {
        printf("      (no orchestrator url given — /decide tests skipped)\n");
    }
    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
