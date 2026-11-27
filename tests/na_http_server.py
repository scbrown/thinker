#!/usr/bin/env python3
"""A throwaway HTTP server for the na_http tests.

Not the orchestrator and not a stand-in for it — it exists to give the DLL's
socket code the four answers it has to survive: a normal 200, a 500, a 404, and
a server that is alive but slower than the caller's deadline. The real
orchestrator can only supply the first of those on demand.

    tests/na_http_server.py 8099
"""

from __future__ import annotations

import json
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# The shape NeuralAmplifier's announce_turn returns with an overlay set (faction_policy.py).
TURN_POLICY_REPLY = {
    "turn": 12,
    "expected": 3,
    "grounding": {"status": "primed", "rows": 2},
    "na_policy": [
        {
            "faction_id": 3,
            "knobs": {
                "focus.fight": -1,
                "focus.tech": 1,
                "strategy.sea_power": True,
                "strategy.attack_enemy_hq": False,
                "target.base": 41,
                "target.goals": [
                    {"priority": 5, "type": "attack", "x": 10, "y": 12},
                    {"priority": 20, "type": "colonize", "x": 0, "y": 511},
                ],
            },
        },
        {"faction_id": 5, "knobs": {"target.base": -1}},
    ],
    "policy_hash": "0123456789abcdef" * 4,
}


class Handler(BaseHTTPRequestHandler):
    # Keep the test output readable; the exe reports its own results.
    def log_message(self, fmt: str, *args: object) -> None:  # noqa: A003
        pass

    def do_POST(self) -> None:  # noqa: N802
        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(length)

        if self.path == "/echo":
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif self.path == "/turn-policy":
            # The orchestrator's /turn reply with an overlay, byte for byte the inline REPLY
            # in na_policy_test.cpp, so the wire read is checked against the same values.
            reply = json.dumps(TURN_POLICY_REPLY, separators=(",", ":")).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(reply)))
            self.end_headers()
            self.wfile.write(reply)
        elif self.path == "/boom":
            self.send_error(500, "deliberate")
        elif self.path == "/slow":
            # Longer than any deadline the test uses. The client must not wait
            # this out — that is invariant 9 measured rather than asserted.
            time.sleep(10)
            self.send_response(200)
            self.send_header("Content-Length", "2")
            self.end_headers()
            self.wfile.write(b"{}")
        else:
            self.send_error(404, "no such path")


def main() -> int:
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8099
    server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    # The /slow test leaves a request in flight when the client hangs up, so the
    # server must outlive it without blocking shutdown.
    server.daemon_threads = True
    print(f"na_http test server on 127.0.0.1:{port}", flush=True)
    server.serve_forever()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
