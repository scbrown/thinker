#include "na_http.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

// ---------------------------------------------------------------- NaBuf

void na_buf_init(NaBuf* b) {
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    b->failed = false;
}

void na_buf_free(NaBuf* b) {
    free(b->data);
    na_buf_init(b);
}

// Grow to hold `extra` more bytes plus a terminator. Doubling rather than
// fitting exactly: a world view is built from a few hundred small appends
// (one per action in the action space) and exact-fit growth makes that
// quadratic.
static bool na_buf_reserve(NaBuf* b, size_t extra) {
    if (b->failed) {
        return false;
    }
    size_t need = b->len + extra + 1;
    if (need <= b->cap) {
        return true;
    }
    size_t cap = b->cap ? b->cap : 4096;
    while (cap < need) {
        cap *= 2;
    }
    char* next = (char*)realloc(b->data, cap);
    if (!next) {
        b->failed = true;
        return false;
    }
    b->data = next;
    b->cap = cap;
    return true;
}

void na_buf_puts(NaBuf* b, const char* s) {
    if (!s) {
        return;
    }
    size_t n = strlen(s);
    if (!na_buf_reserve(b, n)) {
        return;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

void na_buf_printf(NaBuf* b, const char* fmt, ...) {
    if (b->failed) {
        return;
    }
    // Measure first, then format. The alternative — format into a fixed stack
    // buffer and hope — is exactly how a base name or a facility effect string
    // from alphax.txt ends up truncated mid-record, and a truncated record is
    // an unparseable one.
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0 || !na_buf_reserve(b, (size_t)n)) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    b->len += (size_t)n;
}

void na_buf_escaped(NaBuf* b, const char* s) {
    if (!s) {
        return;
    }
    for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
        switch (*p) {
            case '"':  na_buf_puts(b, "\\\""); break;
            case '\\': na_buf_puts(b, "\\\\"); break;
            case '\n': na_buf_puts(b, "\\n");  break;
            case '\r': na_buf_puts(b, "\\r");  break;
            case '\t': na_buf_puts(b, "\\t");  break;
            default:
                if (*p < 0x20) {
                    na_buf_printf(b, "\\u%04x", *p);
                } else {
                    char one[2] = {(char)*p, '\0'};
                    na_buf_puts(b, one);
                }
        }
    }
}

// ---------------------------------------------------------------- deadline

/*
Milliseconds left before `deadline`, never negative.

GetTickCount wraps every 49.7 days. Subtracting as unsigned and casting the
result to signed is the standard idiom that survives the wrap; comparing the
two tick values directly is the version that does not, and a game left running
across the wrap would get an instant timeout on every decision from then on.
*/
static int na_ms_left(DWORD deadline) {
    int left = (int)(deadline - GetTickCount());
    return left > 0 ? left : 0;
}

// Wait until the socket is readable (or writable, for connect completion) or
// the deadline passes. Returns false on timeout or error.
static bool na_wait(SOCKET s, bool for_write, DWORD deadline) {
    int left = na_ms_left(deadline);
    if (left <= 0) {
        return false;
    }
    fd_set set;
    FD_ZERO(&set);
    FD_SET(s, &set);
    timeval tv;
    tv.tv_sec = left / 1000;
    tv.tv_usec = (left % 1000) * 1000;
    // Winsock reports a failed non-blocking connect through the exception set
    // only, so connect waits must watch both or an unreachable orchestrator
    // costs the full timeout instead of failing immediately.
    fd_set err;
    FD_ZERO(&err);
    FD_SET(s, &err);
    int rc = select(0, for_write ? NULL : &set, for_write ? &set : NULL,
                    for_write ? &err : NULL, &tv);
    if (rc <= 0) {
        return false;
    }
    return FD_ISSET(s, &set) != 0;
}

// ---------------------------------------------------------------- endpoint

/*
Split "http://host:port" into host and port.

Only what thinker.ini can actually contain. A https endpoint is rejected rather
than downgraded to http: the orchestrator holds an Anthropic API key, and a
config typo that quietly sends a world view in clear text to a port that was
meant to be TLS is the kind of thing that is discovered much later or never.
*/
static bool na_split_endpoint(const char* endpoint, char* host, size_t host_cap, int* port) {
    if (!endpoint) {
        return false;
    }
    const char* p = endpoint;
    if (strncmp(p, "http://", 7) == 0) {
        p += 7;
    } else if (strncmp(p, "https://", 8) == 0) {
        return false;
    }
    const char* end = p;
    while (*end && *end != ':' && *end != '/') {
        end++;
    }
    size_t n = (size_t)(end - p);
    if (n == 0 || n >= host_cap) {
        return false;
    }
    memcpy(host, p, n);
    host[n] = '\0';
    *port = 80;
    if (*end == ':') {
        *port = atoi(end + 1);
        if (*port <= 0 || *port > 65535) {
            return false;
        }
    }
    return true;
}

/*
Winsock startup, once per process.

The DLL cannot own WSACleanup: terranx.exe may have its own Winsock users (the
original game has multiplayer), and calling cleanup on unload would decrement a
refcount we did not take. Starting up and never cleaning up is the correct
asymmetry for a mod DLL — the process exit does the rest.
*/
static bool na_winsock_ready() {
    static int state = 0;  // 0 untried, 1 ready, -1 failed
    if (state == 0) {
        WSADATA wsa;
        state = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0) ? 1 : -1;
    }
    return state == 1;
}

// ---------------------------------------------------------------- post

bool na_http_post(const char* endpoint, const char* path, const char* body,
                  int timeout_ms, NaBuf* resp) {
    na_buf_init(resp);
    char host[256];
    int port = 0;
    if (!na_split_endpoint(endpoint, host, sizeof(host), &port) || !na_winsock_ready()) {
        return false;
    }
    if (timeout_ms <= 0) {
        return false;
    }
    const DWORD deadline = GetTickCount() + (DWORD)timeout_ms;

    // getaddrinfo can block past the deadline on a name that needs a DNS
    // lookup. In practice the endpoint is a loopback literal and this returns
    // immediately; the deadline is re-checked straight after so a slow resolve
    // costs the call rather than the turn.
    addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char portstr[8];
    snprintf(portstr, sizeof(portstr), "%d", port);
    addrinfo* ai = NULL;
    if (getaddrinfo(host, portstr, &hints, &ai) != 0 || !ai) {
        return false;
    }
    if (na_ms_left(deadline) <= 0) {
        freeaddrinfo(ai);
        return false;
    }

    SOCKET s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (s == INVALID_SOCKET) {
        freeaddrinfo(ai);
        return false;
    }
    // Non-blocking for the whole exchange. One deadline covers connect, send
    // and read together — see the header on why per-stage timeouts do not add
    // up to the number in the config file.
    u_long nonblocking = 1;
    ioctlsocket(s, FIONBIO, &nonblocking);

    bool ok = false;
    if (connect(s, ai->ai_addr, (int)ai->ai_addrlen) == 0
        || WSAGetLastError() == WSAEWOULDBLOCK) {
        ok = na_wait(s, true, deadline);
    }
    freeaddrinfo(ai);
    if (ok) {
        int err = 0;
        int len = sizeof(err);
        if (getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&err, &len) != 0 || err != 0) {
            ok = false;
        }
    }

    NaBuf req;
    na_buf_init(&req);
    if (ok) {
        /*
        HTTP/1.0 on purpose.

        The orchestrator runs under uvicorn, which is free to answer an HTTP/1.1
        request with chunked transfer-encoding — and de-chunking is a parser this
        DLL has no business carrying. An HTTP/1.0 request may not be answered
        with chunked encoding, so the server is obliged to send a plain body and
        close. That turns "read the response" into "read until EOF", which is
        the one framing that cannot be got subtly wrong.
        */
        na_buf_printf(&req, "POST %s HTTP/1.0\r\n", path);
        na_buf_printf(&req, "Host: %s:%d\r\n", host, port);
        na_buf_puts(&req, "Content-Type: application/json\r\n");
        na_buf_printf(&req, "Content-Length: %lu\r\n", (unsigned long)strlen(body));
        na_buf_puts(&req, "Connection: close\r\n\r\n");
        na_buf_puts(&req, body);
        ok = !req.failed;
    }

    size_t sent = 0;
    while (ok && sent < req.len) {
        int n = send(s, req.data + sent, (int)(req.len - sent), 0);
        if (n > 0) {
            sent += (size_t)n;
        } else if (n < 0 && WSAGetLastError() == WSAEWOULDBLOCK) {
            // A world view runs to tens of kilobytes and will not fit one send
            // window, so a partial write here is the normal case, not an edge.
            ok = na_wait(s, true, deadline);
        } else {
            ok = false;
        }
    }
    na_buf_free(&req);

    NaBuf raw;
    na_buf_init(&raw);
    while (ok) {
        char chunk[4096];
        int n = recv(s, chunk, sizeof(chunk) - 1, 0);
        if (n > 0) {
            chunk[n] = '\0';
            na_buf_puts(&raw, chunk);
            ok = !raw.failed;
        } else if (n == 0) {
            break;  // clean EOF — the whole response is in `raw`
        } else if (WSAGetLastError() == WSAEWOULDBLOCK) {
            ok = na_wait(s, false, deadline);
        } else {
            ok = false;
        }
    }
    closesocket(s);

    if (ok && raw.data) {
        // "HTTP/1.x NNN ..." — anything that is not 2xx is a failure the caller
        // handles the same way as a dead socket, so the code is not extracted.
        ok = raw.len > 12 && strncmp(raw.data, "HTTP/1.", 7) == 0 && raw.data[9] == '2';
    } else {
        ok = false;
    }
    if (ok) {
        const char* sep = strstr(raw.data, "\r\n\r\n");
        if (sep) {
            na_buf_puts(resp, sep + 4);
            ok = !resp->failed && resp->len > 0;
        } else {
            ok = false;
        }
    }
    na_buf_free(&raw);
    if (!ok) {
        na_buf_free(resp);
    }
    return ok;
}

// ---------------------------------------------------------------- json

// Find `"key"` used as an object key — followed, after any whitespace, by a
// colon. Matching the bare quoted token would also match a *value* that happens
// to equal the key name, which for "action_id" is not hypothetical.
static const char* na_find_key(const char* json, const char* key) {
    char needle[64];
    int n = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (n < 0 || (size_t)n >= sizeof(needle)) {
        return NULL;
    }
    for (const char* p = strstr(json, needle); p; p = strstr(p + 1, needle)) {
        const char* q = p + n;
        while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') {
            q++;
        }
        if (*q == ':') {
            return q + 1;
        }
    }
    return NULL;
}

bool na_json_string(const char* json, const char* key, char* out, size_t cap) {
    if (!json || !out || cap == 0) {
        return false;
    }
    out[0] = '\0';
    const char* p = na_find_key(json, key);
    if (!p) {
        return false;
    }
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
        p++;
    }
    if (*p != '"') {
        return false;  // present but null or non-string — treat as absent
    }
    p++;
    size_t i = 0;
    while (*p && *p != '"') {
        char c = *p;
        if (c == '\\' && p[1]) {
            p++;
            switch (*p) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case '"': c = '"';  break;
                case '\\': c = '\\'; break;
                case '/': c = '/';  break;
                default:  c = *p;   break;
            }
        }
        if (i + 1 >= cap) {
            return false;  // truncated ids must not be applied — see the header
        }
        out[i++] = c;
        p++;
    }
    if (*p != '"') {
        return false;  // unterminated — a partial read, not a value
    }
    out[i] = '\0';
    return true;
}

bool na_json_true(const char* json, const char* key) {
    if (!json) {
        return false;
    }
    const char* p = na_find_key(json, key);
    if (!p) {
        return false;
    }
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
        p++;
    }
    return strncmp(p, "true", 4) == 0;
}
