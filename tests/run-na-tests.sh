#!/usr/bin/env bash
# Build and run the Neural Amplifier checks that need no game.
#
# Two kinds, and the first kind is the cheap one:
#
#   SOURCE PROPERTIES — read the tree, assert something structural about it.
#     No build, no Wine, no network. These catch the failures that are
#     invisible to a compiler: a seam dropped by a merge, the native answer
#     leaking into a request body.
#
#   WIRE TESTS — src/na_http.cpp links no engine headers, so the DLL's one
#     piece of network code builds standalone and runs under Wine against a
#     stub server. Everything else in the adapter needs a real SMAC install to
#     exercise; this does not, which is why it is the part that can regress
#     in CI.
#
#   tests/run-na-tests.sh              stub server only
#   tests/run-na-tests.sh --with-orchestrator
#       also starts `neural-amplifier serve` from a sibling NeuralAmplifier
#       checkout and drives a real POST /decide through the client. That is the
#       half a stub cannot cover — whether uvicorn answers this client the way
#       the client assumes.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(dirname "$here")"
na="${NA_DIR:-$root/../NeuralAmplifier}"
stub_port="${NA_STUB_PORT:-8099}"
orch_port="${NA_ORCH_PORT:-8077}"

# Source properties first: they need nothing, they run in under a second, and
# a tree that fails them is not worth spending a build on.
python3 "$here/check_seams.py"
python3 "$here/test_check_seams.py" >/dev/null && echo "ok    the seam guard can still fail (11 mutations refused)"
python3 "$here/check_native_choice_withheld.py" >/dev/null \
    && echo "ok    every decide surface withholds the native answer"
echo

# Presets resolve against the working directory, not the -S path, so this has to
# run from the Thinker root — the caller's cwd is usually a NeuralAmplifier checkout.
(cd "$root" && cmake --preset release >/dev/null)
cmake --build "$root/build/release" --target na_http_test -j"$(nproc)" >/dev/null
echo "built na_http_test"

pids=()
cleanup() {
    for pid in ${pids+"${pids[@]}"}; do
        kill "$pid" 2>/dev/null || true
    done
}
trap cleanup EXIT

python3 "$here/na_http_server.py" "$stub_port" >/dev/null 2>&1 &
pids+=($!)

orch_url=""
if [ "${1:-}" = "--with-orchestrator" ]; then
    if [ ! -d "$na/orchestrator" ]; then
        echo "no orchestrator at $na — set NA_DIR to a NeuralAmplifier checkout" >&2
        exit 1
    fi
    (cd "$na/orchestrator" && uv run neural-amplifier serve --port "$orch_port") >/dev/null 2>&1 &
    pids+=($!)
    orch_url="http://127.0.0.1:$orch_port"
    # uvicorn's first start pays import cost; the client's own deadlines are what
    # the tests measure, so waiting here rather than inside them keeps the
    # timeout assertions honest.
    for _ in $(seq 1 60); do
        if curl -fsS "$orch_url/health" >/dev/null 2>&1; then break; fi
        sleep 1
    done
    curl -fsS "$orch_url/health" >/dev/null || { echo "orchestrator did not come up" >&2; exit 1; }
fi

# The stub server needs a moment to bind; the tests fail loudly if it has not.
sleep 1
WINEDEBUG="${WINEDEBUG:--all}" wine "$root/build/release/na_http_test.exe" \
    "http://127.0.0.1:$stub_port" $orch_url
