"""Shared seam extraction for the Neural Amplifier adapter.

A "seam" is any Neural Amplifier symbol that lives inside a file Thinker owns —
either one of our helpers defined there, or a call from upstream's code into
ours. They are the fork's entire merge-fragility surface: everything in
neural.cpp / na_http.cpp is ours alone and cannot conflict, while these sit in
the middle of code upstream rewrites.

Kept separate from check_seams.py so the checker and the manifest generator
cannot drift apart by extracting differently.
"""

import re

# Files that are entirely ours. Nothing in them can be lost to a merge, so
# nothing in them is a seam.
OURS = {"neural.cpp", "na_http.cpp"}

# A function definition at file scope: starts in column 0, has a parameter list,
# and opens a brace on the same line. Indented code (if/while/else, member
# functions) is deliberately excluded — every seam host in this codebase is a
# file-scope function, and a looser pattern starts matching control flow.
FN_DEF = re.compile(r"^[A-Za-z_][A-Za-z0-9_:\*\s&<>,]*\b(\w+)\s*\([^;]*\)\s*(?:const)?\s*\{")

NA_SYMBOL = re.compile(r"\bna_[a-z0-9_]+\s*\(")


def strip_code(lines):
    """Yield (lineno, code) with comments blanked out.

    Blanked rather than dropped so line numbers stay usable, and so a seam
    that has been *commented out* reads as absent — which is the point.
    """
    in_block = False
    for i, raw in enumerate(lines, 1):
        line = raw
        if in_block:
            end = line.find("*/")
            if end < 0:
                yield i, ""
                continue
            line = " " * (end + 2) + line[end + 2:]
            in_block = False
        # Strip block comments that open and close on this line.
        while True:
            start = line.find("/*")
            if start < 0:
                break
            end = line.find("*/", start + 2)
            if end < 0:
                line = line[:start]
                in_block = True
                break
            line = line[:start] + " " * (end + 2 - start) + line[end + 2:]
        start = line.find("//")
        if start >= 0:
            line = line[:start]
        yield i, line


def extract(path):
    """Return a list of seam dicts for one source file."""
    lines = path.read_text().splitlines()
    host = "<file-scope>"
    seams = []
    for lineno, code in strip_code(lines):
        m = FN_DEF.match(code)
        if m:
            host = m.group(1)
        for hit in NA_SYMBOL.finditer(code):
            sym = hit.group(0)[:-1].strip()
            # On a definition line the host was just set to this same symbol.
            kind = "def" if (m and sym == host) else "call"
            seams.append({"file": path.name, "host": host, "symbol": sym,
                          "kind": kind, "line": lineno})
    return seams


def extract_all(src_dir):
    seams = []
    for path in sorted(src_dir.glob("*.cpp")):
        if path.name in OURS:
            continue
        seams.extend(extract(path))
    return seams


def function_body(lines, host):
    """Return (start, end) 1-based line numbers of `host`'s body, braces excluded.

    Brace counting runs over comment-stripped text so a brace inside a comment
    cannot close the function early.
    """
    stripped = list(strip_code(lines))
    start = None
    for lineno, code in stripped:
        m = FN_DEF.match(code)
        if m and m.group(1) == host:
            start = lineno
            break
    if start is None:
        return None
    depth = 0
    for lineno, code in stripped[start - 1:]:
        # Blank out string and char literals so their braces do not count.
        code = re.sub(r'"(\\.|[^"\\])*"', '""', code)
        code = re.sub(r"'(\\.|[^'\\])*'", "''", code)
        depth += code.count("{") - code.count("}")
        if depth == 0 and lineno > start:
            return (start + 1, lineno - 1)
        if depth == 0 and lineno == start and code.count("}"):
            return (start, start)
    return None


def body_statements(lines, span):
    """Non-blank, non-comment lines inside a body span, as (lineno, code)."""
    out = []
    for lineno, code in strip_code(lines):
        if span[0] <= lineno <= span[1] and code.strip():
            out.append((lineno, code))
    return out
