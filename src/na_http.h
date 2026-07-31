#pragma once

#include <stddef.h>

/*
Neural Amplifier: the wire to the orchestrator, plus the buffer that feeds it.

Deliberately free of every engine header. Nothing here touches game state, so it
links into a standalone test binary and can be exercised under Wine against a
throwaway HTTP server with no SMAC install present — which is the only way any of
this gets tested before a game exists to test it in.
*/

/*
A growable byte buffer.

The observation writers used to print straight into the JSONL FILE*, which was
fine while the record's only destination was disk. A1 needs the same bytes twice
— once as the request body, once as the log line — and the record cannot be
written to the log first because two of its fields (`tier`, `applied`) are only
known after the call returns. So the writers build here and the caller decides
where it goes.

Allocation failure is absorbed, not reported: `failed` latches and every
subsequent append is a no-op, so a caller can emit a whole record without
checking each step and test `failed` once at the end. An out-of-memory DLL that
silently stops decorating a log line is a far better outcome than one that
crashes the game mid-turn.
*/
struct NaBuf {
    char* data;
    size_t len;
    size_t cap;
    bool failed;
};

void na_buf_init(NaBuf* b);
void na_buf_free(NaBuf* b);
void na_buf_puts(NaBuf* b, const char* s);
void na_buf_printf(NaBuf* b, const char* fmt, ...);

/*
Append `s` as the body of a JSON string, escaping what RFC 8259 requires. Does
NOT write the surrounding quotes — callers usually have a key to emit alongside.

Base names are editable in-game and faction nouns come from a text file, so every
name that reaches a world view is player-supplied and untrusted as far as our
output format is concerned. An unescaped quote produces a line the orchestrator
silently drops — the sort of bug that surfaces three weeks later as "coverage is
mysteriously low" rather than as an error.
*/
void na_buf_escaped(NaBuf* b, const char* s);

/*
POST `body` as application/json and return the response body in `resp`.

`endpoint` is the orchestrator base URL as it appears in thinker.ini —
"http://127.0.0.1:8000". Only http and an explicit host[:port] are understood; a
https endpoint is rejected rather than silently downgraded.

`timeout_ms` bounds the WHOLE exchange — DNS, connect, send and the read loop —
against one deadline, not one timeout per stage. That distinction is invariant 9
("the game never stalls waiting on the brain"): three stages each honouring a
2 s timeout is a 6 s freeze, and the number in the config file needs to mean what
a player would think it means.

Returns true only on a 2xx with a body. Every other outcome — unreachable,
timed out, 5xx, malformed — is false, and the caller's contract is to fall back
to the engine's own answer. There is no error detail out-parameter on purpose:
the only decision available to the caller is "use the fallback", and a code that
cannot change the outcome is a code nobody checks.
*/
bool na_http_post(const char* endpoint, const char* path, const char* body,
                  int timeout_ms, NaBuf* resp);

/*
Extract the first string value for `key` anywhere in `json`.

A deliberate non-parser. The DLL reads exactly one field out of the orchestrator's
reply — the chosen action id — and shipping a JSON parser into a 32-bit game DLL
to do that would be more code to get wrong than the thing it enables. The reply is
validated on the other side by Pydantic, and anything this misreads produces an
id that fails the action-space check and falls back.

Handles escapes well enough to round-trip an id (\\", \\\\, \\n, \\t, \\/); \\u
escapes are passed through unchanged, which is correct for action ids because
they are ASCII by construction ("unit:12", "facility:34").
*/
bool na_json_string(const char* json, const char* key, char* out, size_t cap);

// True when the JSON has `"key":true`. Used for `degraded`, which is worth
// recording even though it does not change what gets applied.
bool na_json_true(const char* json, const char* key);
