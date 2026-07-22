#!/usr/bin/env python3
"""Migrate the Bezel corpus to the v0.217 keyword-led lambda syntax
(spec §27.3 / Appendix A.7 `LambdaExpr` / B.1, proposal 0064, impl #2101).

Through v0.216 a lambda was `(params) => body` (with `async (params) =>` for
the asynchronous form). v0.217 retires that spelling: a lambda is keyword-led,
`lambda(params): T? => body` / `async lambda(params) => body`. The rewrite is
one inserted token per lambda — `lambda` immediately before the parameter
list's `(` — and nothing else changes. This script performs that insertion
across:

  (i)  embedded Bezel programs inside C++ string literals in tests/**/*.cpp
       (ordinary, escaped, and raw `R"…( … )…"` literals; adjacent literals
       spliced by whitespace/`+` are treated as one program, following the
       migrate_case_of.py precedent) — including the parse-shape expectation
       strings rendered by tests/support/parse_support.hpp, whose new-form
       output (`(lambda(x) => x)`) is exactly the old expectation with the
       same insertion applied;
  (ii) Bezel source in tests/lit/*.bzl and tests/conformance/**/*.bzl
       (per #1380: existing case files are edited in place — hand annotations
       preserved, no blind regeneration).

Why this needs more than a regex (the 0064 §5 finding): `=>` is dominated by
NON-lambda uses — case/when arm separators and expression-bodied members — and
`(…) =>` itself also matches a case-arm destructuring pattern head
(`Point(x, y) => …`), a when-arm call condition (`IsReady(a) => …`), and a
parenthesized when-guard (`when (t > 0) => …`). Classification is therefore
by PARSE CONTEXT, approximated conservatively:

  A `(`-…-`)`-`=>` site is a lambda head iff the token immediately BEFORE the
  `(` puts it in EXPRESSION position:
    - an identifier / literal / `)` / `]` before the `(` means a pattern head
      or call — NOT a lambda (this also covers an already-migrated head,
      whose previous word is `lambda` — the idempotency rule);
    - the keyword `async` means the retired async form — a lambda;
    - `when` means a parenthesized guard — not a lambda;
    - expression-operator keywords (`and`/`or`/`not`/`in`/`is`/`as`/`mod`)
      mean a grouped operand — not a lambda;
    - clear expression positions (`:=`, `(`, `[`, `,`, `:`, `=>`, `??`,
      `return`, `then`, `else`, `do`, or the start of a program) are lambda
      CANDIDATES, confirmed only if the paren interior has LambdaParams shape
      (comma-separated `mods? ident (: Type)?` / `_` / `Qual(…)` destructure,
      or empty). A candidate whose interior is NOT param-shaped (e.g. a
      parenthesized when-arm condition `(a and b) => r` after a `,`) is left
      untouched and reported as RESIDUE for manual triage;
    - anything else is reported as RESIDUE.

Files containing the marker `migrate_lambda_keyword: skip-file` are never
touched (the lit reject tests for the retired spelling hold old-form code on
purpose).

Idempotent and re-runnable: a migrated head's `(` is preceded by the word
`lambda`, which the identifier rule skips; re-running on a migrated (or
partially hand-fixed) tree is a no-op. Exit code 0; residue is advisory.

Usage: scripts/migrate_lambda_keyword.py [root-or-file ...]
       (defaults to `tests`)
"""

import os
import re
import sys

SKIP_MARKER = "migrate_lambda_keyword: skip-file"

# ── Phase A: extract a "logical" Bezel text with a physical-offset map ───────


def logical_from_bzl(text):
    """Whole .bzl file is Bezel source: identity map."""
    return list(text), [i for i in range(len(text))]


_CPP_SPLICE_GAP = re.compile(
    r"[\s]*(\+[\s]*[A-Za-z_][\w.]*(\([^()]*\))?[\s]*)*\+?[\s]*"
)

_CPP_ESCAPES = {
    "n": "\n", "t": "\t", "r": "\r", '"': '"', "\\": "\\", "'": "'", "0": "\0",
}


def logical_from_cpp(text):
    """Concatenate the decoded contents of C++ string literals into a logical
    Bezel text; logical[i] maps to phys[i], the physical offset of the source
    character (insertion happens AT that offset). Adjacent literals spliced by
    whitespace/`+` continue one group; other gaps insert a hard separator so
    unrelated programs never merge. Handles ordinary, escaped, and raw
    (`R"delim( … )delim"`) literals; skips C++ comments and char literals."""
    logical, phys = [], []
    i, n = 0, len(text)
    gap_start = None  # physical offset where the gap after the last literal began

    def separator():
        # NUL is a hard group boundary: prev_token treats it as start-of-text,
        # so classification never leaks across unrelated literal groups.
        if logical and logical[-1] != "\x00":
            logical.append("\x00")
            phys.append(-1)  # never an insertion point

    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            i = n if j < 0 else j
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
            continue
        if c == "'":
            i += 1
            while i < n and text[i] != "'":
                i += 2 if text[i] == "\\" else 1
            i += 1
            continue
        # Raw string literal R"delim( … )delim"
        m = None
        if c == "R" and i + 1 < n and text[i + 1] == '"':
            m = re.match(r'R"([^\s()\\]{0,16})\(', text[i:])
        if m:
            if gap_start is not None and not _CPP_SPLICE_GAP.fullmatch(
                    text[gap_start:i]):
                separator()
            body_start = i + m.end()
            closer = ")" + m.group(1) + '"'
            j = text.find(closer, body_start)
            j = n if j < 0 else j
            for k in range(body_start, j):
                logical.append(text[k])
                phys.append(k)
            i = j + len(closer)
            gap_start = i
            continue
        if c == '"':
            if gap_start is not None and not _CPP_SPLICE_GAP.fullmatch(
                    text[gap_start:i]):
                separator()
            i += 1
            while i < n:
                d = text[i]
                if d == "\\" and i + 1 < n:
                    logical.append(_CPP_ESCAPES.get(text[i + 1], text[i + 1]))
                    phys.append(i)
                    i += 2
                    continue
                if d == '"':
                    i += 1
                    break
                logical.append(d)
                phys.append(i)
                i += 1
            gap_start = i
            continue
        i += 1
    return logical, phys


# ── Phase B: blank Bezel comments and string interiors in place ──────────────


def blank_bezel_noncode(logical):
    """Replace the characters of Bezel `//`/`/* */` comments and string/char
    literal contents (including the quotes) with spaces, preserving indices,
    so lambda-head matching never fires inside them."""
    out = logical[:]
    i, n = 0, len(out)
    while i < n:
        c = out[i]
        if c == "/" and i + 1 < n and out[i + 1] == "/":
            while i < n and out[i] != "\n":
                out[i] = " "
                i += 1
            continue
        if c == "/" and i + 1 < n and out[i + 1] == "*":
            out[i] = out[i + 1] = " "
            i += 2
            while i < n:
                if out[i] == "*" and i + 1 < n and out[i + 1] == "/":
                    out[i] = out[i + 1] = " "
                    i += 2
                    break
                out[i] = " "
                i += 1
            continue
        if c in ('"', "'"):
            quote = c
            out[i] = " "
            i += 1
            while i < n:
                d = out[i]
                if d == "\\" and i + 1 < n:
                    out[i] = out[i + 1] = " "
                    i += 2
                    continue
                out[i] = " "
                i += 1
                if d == quote:
                    break
            continue
        i += 1
    return "".join(out)


# ── Classification ───────────────────────────────────────────────────────────

# Keywords that, immediately before `(…) =>`, mean "not a lambda head".
_GUARD_KEYWORDS = {"when"}
_OPERATOR_KEYWORDS = {"and", "or", "not", "in", "is", "as", "mod"}
# Tokens/keywords that put the `(` in expression position (lambda candidate).
# `+=`/`-=` are the §17.2 event subscribe/unsubscribe positions, whose RHS is
# routinely a handler lambda.
_EXPR_POSITION_PUNCT = {":=", "(", "[", ",", ":", "=>", "??", "+=", "-="}
_EXPR_POSITION_KEYWORDS = {"return", "then", "else", "do", "of"}

_WORD_RE = re.compile(r"[A-Za-z_]\w*\Z")

_PARAM_ITEM = re.compile(
    r"^(?:(?:ref|out\s+owned|out|owned|scoped)\s+)?"
    r"(?:_|[A-Za-z_]\w*)(?:\s*:\s*\S.*)?$",
    re.S,
)
_DESTRUCTURE_ITEM = re.compile(
    r"^[A-Za-z_]\w*(?:\.[A-Za-z_]\w*)*\s*\(.*\)\s*$", re.S)


def prev_token(s, i):
    """The token (word or punctuator) immediately before index i in the
    blanked logical text; ('', '') at start of text/group. Newlines are
    crossed (multi-line heads); the NUL group separator is a hard boundary."""
    j = i
    while j > 0 and s[j - 1] in " \t\r\n":
        j -= 1
    if j == 0 or s[j - 1] == "\x00":
        return ("", "")
    m = _WORD_RE.search(s[:j])
    if m and m.end() == j:
        return ("word", m.group(0))
    # Multi-char punctuators of interest, longest first.
    for p in (":=", "=>", "->", "??", "..", "+=", "-="):
        if s[j - len(p):j] == p:
            return ("punct", p)
    return ("punct", s[j - 1])


def token_before_offset(s, i):
    """Physical start offset of the token prev_token(s, i) found — i.e. the
    index just past the whitespace-preceding position — for second-level
    context checks."""
    j = i
    while j > 0 and s[j - 1] in " \t\r\n":
        j -= 1
    return j


def matching_paren(s, i):
    """Index of the `)` matching the `(` at s[i], or -1."""
    depth = 0
    j = i
    n = len(s)
    while j < n:
        if s[j] == "(":
            depth += 1
        elif s[j] == ")":
            depth -= 1
            if depth == 0:
                return j
        j += 1
    return -1


def split_top_level(interior):
    """Split on top-level commas, tracking (), [], <> depth; `->` is a unit
    (its `>` is not an angle closer)."""
    items, buf = [], []
    depth_par = depth_brk = depth_ang = 0
    i, n = 0, len(interior)
    while i < n:
        c = interior[i]
        if c == "-" and i + 1 < n and interior[i + 1] == ">":
            buf.append("->")
            i += 2
            continue
        if c == "(":
            depth_par += 1
        elif c == ")":
            depth_par -= 1
        elif c == "[":
            depth_brk += 1
        elif c == "]":
            depth_brk -= 1
        elif c == "<":
            depth_ang += 1
        elif c == ">":
            depth_ang = max(0, depth_ang - 1)
        elif (c == "," and depth_par == 0 and depth_brk == 0 and
              depth_ang == 0):
            items.append("".join(buf))
            buf = []
            i += 1
            continue
        buf.append(c)
        i += 1
    items.append("".join(buf))
    return items


def is_param_list(interior):
    """Does the paren interior have LambdaParams shape (A.7)?"""
    if interior.strip() == "":
        return True  # zero params: the empty pair
    items = split_top_level(interior)
    # v0.214 trailing comma: a final empty item is fine.
    if len(items) > 1 and items[-1].strip() == "":
        items = items[:-1]
    for item in items:
        t = item.strip()
        if t == "":
            return False
        if _PARAM_ITEM.match(t) or _DESTRUCTURE_ITEM.match(t):
            continue
        return False
    return True


def find_insertions(blanked, phys):
    """Return (insertion physical offsets, residue records)."""
    inserts, residue = [], []
    for m in re.finditer(r"\(", blanked):
        i = m.start()
        close = matching_paren(blanked, i)
        if close < 0:
            continue
        after = blanked[close + 1:close + 16].lstrip()
        if not after.startswith("=>"):
            continue
        kind, tok = prev_token(blanked, i)
        if kind == "word":
            if tok == "async":
                inserts.append(phys[i])
                continue
            if tok in _GUARD_KEYWORDS or tok in _OPERATOR_KEYWORDS:
                continue
            if tok in _EXPR_POSITION_KEYWORDS:
                pass  # fall through to the interior check
            else:
                # identifier (incl. `lambda` — idempotency), pattern head,
                # or an unknown keyword: not a lambda head.
                continue
        elif kind == "punct":
            if tok == ":":
                # §27.3 v0.217: `lambda(params): () => body` — the paren pair
                # here is the RETURN-TYPE ASCRIPTION's unit type, not a lambda
                # head. Discriminate from a dict-literal value position
                # (`"key": (x) => v`, a real lambda) by what precedes the `:`:
                # a `)` (the params list closer) means ascription — skip.
                colon_end = token_before_offset(blanked, i)
                pk, pt = prev_token(blanked, colon_end - 1)
                if pk == "punct" and pt == ")":
                    continue
            if tok in _EXPR_POSITION_PUNCT:
                pass  # fall through to the interior check
            elif tok in (")", "]"):
                continue
            else:
                residue.append(i)
                continue
        # ('' , '') start-of-text: expression position; interior check.
        interior = blanked[i + 1:close]
        if is_param_list(interior):
            inserts.append(phys[i])
        else:
            residue.append(i)
    return inserts, residue


def line_of(text, off):
    return text.count("\n", 0, off) + 1


def migrate_file(path):
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()
    if SKIP_MARKER in text:
        return 0, []
    if path.endswith(".bzl"):
        logical, phys = logical_from_bzl(text)
    else:
        logical, phys = logical_from_cpp(text)
    if not logical:
        return 0, []
    blanked = blank_bezel_noncode(logical)
    inserts, residue_idx = find_insertions(blanked, phys)
    residue = []
    for i in residue_idx:
        p = phys[i] if 0 <= i < len(phys) else -1
        ctx = blanked[max(0, i - 30):i + 30].replace("\n", " ")
        residue.append((line_of(text, p) if p >= 0 else 0, ctx.strip()))
    if inserts:
        out = text
        for p in sorted(set(inserts), reverse=True):
            if p < 0:
                continue
            out = out[:p] + "lambda" + out[p:]
        with open(path, "w", encoding="utf-8") as f:
            f.write(out)
    return len(set(p for p in inserts if p >= 0)), residue


def main(argv):
    roots = argv[1:] or ["tests"]
    files = []
    for root in roots:
        if os.path.isfile(root):
            files.append(root)
            continue
        for dirpath, _dirs, names in os.walk(root):
            for name in names:
                if name.endswith(".cpp") or name.endswith(".bzl"):
                    files.append(os.path.join(dirpath, name))
    total_sites = total_files = 0
    all_residue = []
    for path in sorted(files):
        try:
            k, residue = migrate_file(path)
        except Exception as e:  # noqa
            print(f"ERROR {path}: {e}", file=sys.stderr)
            continue
        for (ln, ctx) in residue:
            all_residue.append(f"RESIDUE {path}:{ln}: …{ctx}…")
        if k:
            total_files += 1
            total_sites += k
            print(f"{k:3d}  {path}")
    print(f"\n{total_sites} insertions across {total_files} files")
    if all_residue:
        print(f"\n{len(all_residue)} unclassified `(…) =>` site(s) left "
              "untouched — triage by hand:")
        for r in all_residue:
            print("  " + r)


if __name__ == "__main__":
    main(sys.argv)
