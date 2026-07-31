#!/usr/bin/env bash
# Build and run the Neural Amplifier wire tests.
#
# Needs a 32-bit MinGW cross-compiler and Wine, and NO game: src/na_http.cpp
# links no engine headers, so the DLL's one piece of network code runs as a
# standalone exe. Everything else in the adapter needs a real SMAC install to
# exercise; this does not, which is why it is the part that can regress in CI.
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
