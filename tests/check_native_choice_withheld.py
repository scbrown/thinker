"""na-glk: prove the native answer cannot be in the request body, per decide surface.

The property: inside each na_decide_* function, the na_build_* call must pass reveal_native
= false, and the only na_write_native_* call must appear AFTER the na_http_post. Checked on
source order because the buffer IS the request body up to the post and the record after it.
"""

import pathlib
import re
import sys

SRC = str(pathlib.Path(__file__).resolve().parent.parent / "src" / "neural.cpp")
text = open(SRC).read()
lines = text.splitlines()

SURFACES = {
    "na_decide_base_production": ("na_build_base_production", "na_write_native_choice"),
    "na_decide_faction_tech": ("na_build_faction_tech", "na_write_native_tech"),
    "na_decide_faction_se": ("na_build_faction_se", "na_write_native_se"),
}
# The one surface that must NOT change: its decide path never knows the native answer.
CONTROL = "na_decide_base_hurry"

failures = []


_STR = re.compile(r'"(?:\\.|[^"\\])*"')


def _code(ln: str) -> str:
    """Braces inside string literals are not scope. na_buf_puts(&w, "}") ended the walk early."""
    return _STR.sub('""', ln)


def body_of(fn: str) -> list[tuple[int, str]]:
    start = next(i for i, ln in enumerate(lines) if re.match(rf"\w[\w \*]*\b{fn}\(", ln))
    depth, out, seen = 0, [], False
    for i in range(start, len(lines)):
        out.append((i + 1, lines[i]))
        c = _code(lines[i])
        depth += c.count("{") - c.count("}")
        if "{" in c:
            seen = True
        if seen and depth <= 0:
            break
    return out


for fn, (build, emit) in SURFACES.items():
    body = body_of(fn)
    post = [n for n, ln in body if "na_http_post" in ln]
    builds = [(n, ln) for n, ln in body if build + "(" in ln]
    emits = [n for n, ln in body if emit + "(" in ln]

    if not post:
        failures.append(f"{fn}: no na_http_post found — cannot locate the send")
        continue
    if not builds:
        failures.append(f"{fn}: no {build} call found")
        continue
    # Only the buffer that is POSTED must withhold. na_decide_base_production also builds a
    # record-only buffer (&d, the divergence report) that is logged and never sent — it may
    # and should reveal. Key on the buffer name the post actually reads.
    posted_buf = re.search(r"na_http_post\([^;]*?,\s*(\w+)\.data", "\n".join(l for _, l in body))
    posted = posted_buf.group(1) if posted_buf else "w"
    for n, ln in builds:
        target = re.search(rf"{build}\(&?(\w+)", ln)
        if target and target.group(1) != posted:
            print(f"    :{n} builds &{target.group(1)} (record-only, never posted) — reveal ok")
            continue
        if "false" not in ln:
            failures.append(f"{fn}:{n}: {build} into the POSTED buffer does not pass "
                            f"reveal_native=false -> {ln.strip()}")
    if not emits:
        failures.append(f"{fn}: record never regains the native answer ({emit} absent)")
    for n in emits:
        if n < post[0]:
            failures.append(f"{fn}:{n}: {emit} runs BEFORE the post at :{post[0]} — it would be sent")

    sent = [n for n, ln in builds if (re.search(rf"{build}\(&?(\w+)", ln) or [None])
            and re.search(rf"{build}\(&?(\w+)", ln).group(1) == posted]
    withheld = all("false" in ln for n, ln in builds if n in sent)
    ok_emit = bool(emits) and all(n > post[0] for n in emits)
    print(
        f"{fn:28} posted-buffer &{posted} built@{sent} withheld={withheld} "
        f"post@{post[0]} native-emit@{emits} {'OK' if ok_emit and withheld else 'FAIL'}"
    )

# Control: base.hurry already withholds via its tri-state sentinel and must stay untouched.
hurry = body_of(CONTROL)
sentinel = [n for n, ln in hurry if "na_build_base_hurry" in ln and "-1" in ln]
print(f"{CONTROL:28} sentinel -1 passed at {sentinel} (unchanged control)")
if not sentinel:
    failures.append(f"{CONTROL}: lost its -1 sentinel")

print()
if failures:
    print("FAILURES:")
    for f in failures:
        print("  " + f)
    sys.exit(1)
print("all decide surfaces withhold the native answer from the request and restore it after")
