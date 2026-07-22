#!/usr/bin/env python3
"""Migrate the Bezel corpus to the v0.208 paren-delimited expression forms
(spec §20 / §19.1 / §19.7 / §9.5, proposal 0057, impl #2079).

Four rewrites, per the v0.208 changelog's migration note ("mechanical in
shape but not a token substitution"):

  R3  `expr with Name := v, … end`      →  `expr with(Name := v, …)`
  R1  `( X if C else Y )`               →  `(if C then X else Y)`   (paren-for-
      paren; chains fold to `elif`)
  R2  `X if C else Y` (bare postfix)    →  `(if C then X else Y)`   (the
      wrapping pair is gained; X found by scanning left to the nearest
      top-level expression boundary)
  R4  expression-position `case … of … end [case]` / `when … end [when]`
                                        →  `(case … of …)` / `(when …)`
      (statement-position forms are untouched; classification is by the
      token before the keyword, exactly the #2101 migration's approach)

Passes run in the order R3, R1, R2, R4, re-extracting the logical text after
each pass (a with-value may contain an old conditional, an arm body may
contain an old with, …). Rewritten spans are taken from the PHYSICAL text
(so C++ string-literal escapes survive); a span that crosses a C++ literal
splice boundary is reported as residue instead of rewritten.

Anything unclassifiable or structurally surprising is left untouched and
reported as RESIDUE for hand triage; the validation runs (gtest subset, lit,
conformance) are the backstop for the sites this conservative matcher
declines. Files containing `migrate_paren_exprs: skip-file` are never
touched. Re-runnable: migrated forms no longer match any pass's pattern.

Shares the corpus text model with migrate_lambda_keyword.py (imported).

Usage: scripts/migrate_paren_exprs.py [root-or-file ...]   (default: tests)
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from migrate_lambda_keyword import (  # noqa: E402
    blank_bezel_noncode,
    logical_from_bzl,
    logical_from_cpp,
    matching_paren,
    prev_token,
)

SKIP_MARKER = "migrate_paren_exprs: skip-file"

WORD = re.compile(r"[A-Za-z_]\w*")

# Expression-position classification for R4 (mirrors migrate_lambda_keyword):
# the token immediately before `case`/`when` that makes it an EXPRESSION.
# `(` is deliberately ABSENT: a `(`-preceded `case`/`when` is the already-
# migrated atom (`(case …`) — that is R4's idempotency guard — so the rare
# old form directly inside a call paren (`f(case … end)`) is left to the
# compiler's targeted retired-spelling diagnostic and hand fixing.
_EXPR_PREV_PUNCT = {":=", "[", ",", "=>", "??", ":", "+=", "-="}
_EXPR_PREV_WORDS = {"return", "then", "else"}
# Openers that close with `end` when scanning for a construct's closer.
_END_OPENERS = {"do", "case", "if", "while", "for", "try"}


def phys_end(text, phys, b):
    """Physical offset just PAST logical char b-1, accounting for a C++
    escape (two physical bytes for one logical char)."""
    last = phys[b - 1]
    if last < 0:
        return -1
    if text[last] == "\\":
        if b < len(phys) and phys[b] == last + 2:
            return last + 2  # interior escape, next char contiguous
        if b >= len(phys) or phys[b] != last + 1:
            return last + 2  # trailing escape at a span/literal boundary
    return last + 1


def phys_span(text, logical, phys, a, b):
    """Physical text of logical span [a, b), or None when the span crosses a
    C++ literal splice boundary (rewriting across literals is hand work)."""
    if a >= b:
        return ""
    for i in range(a, b - 1):
        pa, pb = phys[i], phys[i + 1]
        if pa < 0 or pb < 0:
            return None
        gap = pb - pa
        if gap == 1:
            continue
        if gap == 2 and text[pa] == "\\":
            continue  # a decoded escape
        return None
    if phys[b - 1] < 0:
        return None
    return text[phys[a]:phys_end(text, phys, b)]


def word_at(s, i):
    m = WORD.match(s, i)
    return m.group(0) if m else ""


def is_kw_at(s, i, kw):
    """Word `kw` begins at index i (with word boundaries)."""
    if not s.startswith(kw, i):
        return False
    if i > 0 and (s[i - 1].isalnum() or s[i - 1] == "_"):
        return False
    j = i + len(kw)
    return j >= len(s) or not (s[j].isalnum() or s[j] == "_")


def scan_matching_end(s, i):
    """From index i (just past a construct's opener), find the offset of the
    `end` word that closes it, tracking nested end-bearing openers. Balanced
    paren groups are skipped whole (a parenthesized atom never contains a
    bare `end`; old expression forms inside parens still nest their own
    `end`s, which the recursive skip inside the group would need — those
    groups are re-extracted on the NEXT pass, so skipping is safe here only
    because passes iterate to fixpoint). `when` is counted as an opener only
    in non-guard position. Returns -1 on failure."""
    depth = 0
    n = len(s)
    while i < n:
        c = s[i]
        if c == "(":
            j = matching_paren(s, i)
            if j < 0:
                return -1
            i = j + 1
            continue
        if c == ";" and depth == 0:
            # An EXPRESSION construct never contains a top-level `;` (the
            # statement-arm bodies belong to the statement forms, which are
            # never scanned here) — a `;` means the classification or the
            # structure is off; bail out rather than capture a distant `end`.
            return -1
        if c.isalpha() or c == "_":
            w = word_at(s, i)
            if w == "end":
                if depth == 0:
                    return i
                depth -= 1
            elif w in _END_OPENERS:
                depth += 1
            elif w == "when":
                pk, pt = prev_token(s, i)
                if (pk == "punct" and pt in {";", ":=", "=>", ",", "(", "["}) \
                        or (pk == "word" and pt in {"do", "then", "else",
                                                    "return"}) \
                        or pk == "":
                    depth += 1
                # guard `when` (after a pattern): not an opener
            elif w == "with":
                # Old-form `with Name := … end` opens an `end` scope.
                rest = s[i + len(w):]
                if re.match(r"\s+[A-Za-z_]\w*\s*:=", rest):
                    depth += 1
            i += len(w)
            continue
        i += 1
    return -1


def top_level_find(s, start, stop, preds):
    """Scan s[start:stop) at top level (skipping balanced (), []), calling
    each (name, matcher) in preds at every position; return (name, i) for
    the first hit, else (None, -1). Matchers get (s, i)."""
    i = start
    while i < stop:
        c = s[i]
        if c == "(":
            j = matching_paren(s, i)
            if j < 0 or j >= stop:
                return (None, -1)
            i = j + 1
            continue
        if c == "[":
            depth = 1
            j = i + 1
            while j < stop and depth:
                if s[j] == "[":
                    depth += 1
                elif s[j] == "]":
                    depth -= 1
                j += 1
            i = j
            continue
        for name, m in preds:
            if m(s, i):
                return (name, i)
        i += 1
    return (None, -1)


# ── R3: with … end → with(…) ────────────────────────────────────────────────


def pass_with(text, logical, phys, residue):
    s = blank_bezel_noncode(logical)
    edits = []  # (phys_start, phys_end, replacement)
    for m in re.finditer(r"\bwith\b", s):
        i = m.start()
        rest = s[m.end():]
        rm = re.match(r"(\s+)[A-Za-z_]\w*\s*:=", rest)
        if not rm:
            continue  # with(…) new form, cancellation clause, or unrelated
        # Find the closing `end` of the init-assign list.
        end_off = scan_matching_end(s, m.end())
        if end_off < 0:
            residue.append((i, "with-update: no matching `end`"))
            continue
        # `with` + following whitespace → `with(`
        p_with_end = phys[m.end() - 1] + 1 if phys[m.end() - 1] >= 0 else -1
        p_gap_end = phys[m.end() + len(rm.group(1)) - 1] + 1
        if p_with_end < 0 or p_gap_end < 0:
            residue.append((i, "with-update: unmappable"))
            continue
        edits.append((p_with_end, p_gap_end, "("))
        # Same-line spaces + `end` → `)`. The walk-back tests the LOGICAL
        # characters (the blanked text turns string-literal interiors into
        # spaces, which must never be consumed as whitespace) and never
        # crosses a newline: a line-leading `end` keeps its line so the `)`
        # can never land inside a preceding `//` comment.
        k = end_off
        while k > 0 and str(logical[k - 1]) in " \t":
            k -= 1
        if k > 0 and str(logical[k - 1]) in "\r\n":
            k = end_off
        p_ws = phys[k]
        p_end_end = phys[end_off + 2] + 1  # past 'end'
        if p_ws < 0 or p_end_end < 0:
            residue.append((i, "with-update: unmappable end"))
            continue
        edits.append((p_ws, p_end_end, ")"))
    return edits


# ── R1/R2: postfix conditional → (if C then X else Y) ───────────────────────


def split_conditional(s, x_start, if_off, stop):
    """Decompose `X if C1 else V2 if C2 else … else Y` (the right-associative
    postfix chain) into ([(c_span, v_span)…], else_span) where each segment
    pairs a condition with the VALUE it selects — X for the first, the span
    between one `else` and the next `if` for the rest — i.e. the `elif`
    fold's segments. Returns None on any surprise (notably a top-level
    `then` before `else`, the statement-if shape)."""
    segs = []
    pending_value = (x_start, if_off)  # the value selected by the NEXT cond
    cur_if = if_off
    while True:
        c_start = cur_if + 2
        name, else_off = top_level_find(
            s, c_start, stop,
            [("then", lambda t, i: is_kw_at(t, i, "then")),
             ("else", lambda t, i: is_kw_at(t, i, "else"))])
        if name != "else":
            return None  # a statement-if (or malformed): leave alone
        segs.append(((c_start, else_off), pending_value))
        v_start = else_off + 4
        name2, next_if = top_level_find(
            s, v_start, stop, [("if", lambda t, i: is_kw_at(t, i, "if"))])
        if name2 == "if":
            pending_value = (v_start, next_if)
            cur_if = next_if
            continue
        return (segs, (v_start, stop))


def build_atom(text, logical, phys, segs, els):
    """Render `(if C1 then V1 elif C2 then V2 … else Y)` from the spans; None
    when any span crosses a literal boundary."""
    def sp(a, b):
        # trim logical whitespace at the edges
        while a < b and str(logical[a]).isspace():
            a += 1
        while b > a and str(logical[b - 1]).isspace():
            b -= 1
        return phys_span(text, logical, phys, a, b)

    es = sp(*els)
    if es is None or not es:
        return None
    parts = []
    for idx, (c, v) in enumerate(segs):
        cs = sp(*c)
        vs = sp(*v)
        if cs is None or not cs or vs is None or not vs:
            return None
        parts.append(("if " if idx == 0 else "elif ") + cs + " then " + vs)
    return "(" + " ".join(parts) + " else " + es + ")"


_BOUNDARY_PUNCT = {":=", "=>", "+=", "-=", "(", "[", ",", ";", ":"}
_BOUNDARY_WORDS = {"return", "then", "else", "do", "in", "of"}


def pass_postfix_if(text, logical, phys, residue, paren_only):
    s = blank_bezel_noncode(logical)
    edits = []
    used = []  # claimed [start, stop) logical ranges, to avoid overlap
    for m in re.finditer(r"\bif\b", s):
        if_off = m.start()
        if any(a <= if_off < b for (a, b) in used):
            continue
        # Enclosing context: scan LEFT to the nearest top-level boundary.
        # (X-emptiness — the statement-if / already-migrated-`(if` guard — is
        # tested AFTER the scan on the LOGICAL text: a prev-token test on the
        # blanked text would misread an X that is exactly a string literal
        # (`"yes" if b else "no"`, blanked to spaces) as an empty X.)
        x_start, encl_open = None, None
        depth = 0
        j = if_off - 1
        while j >= 0:
            c = s[j]
            if c in ")]":
                depth += 1
            elif c in "([":
                if depth == 0:
                    x_start = j + 1
                    encl_open = j if c == "(" else None
                    break
                depth -= 1
            elif depth == 0:
                if c == "\x00":
                    x_start = j + 1
                    break
                two = s[j - 1:j + 1]
                if two in {":=", "=>", "+=", "-="}:
                    x_start = j + 1
                    break
                if c in ",;:":
                    x_start = j + 1
                    break
                if c.isalpha():
                    # word boundaries: return/then/else/do/in/of
                    for w in _BOUNDARY_WORDS:
                        if s[j - len(w) + 1:j + 1] == w and \
                                is_kw_at(s, j - len(w) + 1, w):
                            x_start = j + 1
                            break
                    if x_start is not None:
                        break
            j -= 1
        if x_start is None:
            x_start = 0
        # X must be nonempty on the LOGICAL text: an `if` with nothing but
        # whitespace before it (back to its boundary) is a statement-if or
        # the already-migrated `(if` atom.
        if all(str(logical[t]).isspace() for t in range(x_start, if_off)):
            continue
        # Terminator of the whole conditional: top-level `;` `,` `)` `]` /
        # `end` / group end.
        stop = None
        k = if_off
        dep = 0
        n = len(s)
        while k < n:
            c = s[k]
            if c == "(":
                jj = matching_paren(s, k)
                if jj < 0:
                    break
                k = jj + 1
                continue
            if c == "[":
                dep += 1
            elif c == "]":
                if dep == 0:
                    stop = k
                    break
                dep -= 1
            elif dep == 0:
                if c in ";,)" or c == "\x00":
                    stop = k
                    break
                if c.isalpha() and is_kw_at(s, k, "end"):
                    stop = k
                    break
            k += 1
        if stop is None:
            stop = n
        reuse_parens = False
        if paren_only:
            # R1: only rewrite when the conditional exactly fills a paren
            # group: `( X if C else Y )`.
            if encl_open is None:
                continue
            close = matching_paren(s, encl_open)
            if close != stop:
                continue
            # Paren-for-paren applies only when the pair is a GROUPING paren.
            # A call's or contract clause's argument paren (preceded by an
            # identifier — `F(…)`, `ensures(…)` — or a postfix `)`/`]`) is an
            # arg delimiter, not the conditional's grouping: the atom must
            # supply its OWN pair inside it (v0.208 §20's argument-position
            # rule), so only the interior is replaced.
            opk, opt = prev_token(s, encl_open)
            reuse_parens = not (
                (opk == "word" and opt not in _BOUNDARY_WORDS) or
                (opk == "punct" and opt in (")", "]")))
        elif encl_open is not None and matching_paren(s, encl_open) == stop:
            continue  # R1's case; handled by the paren_only pass
        dec = split_conditional(s, x_start, if_off, stop)
        if dec is None:
            continue  # statement-if shape (then before else): leave
        segs, els = dec
        atom = build_atom(text, logical, phys, segs, els)
        if atom is None:
            residue.append((if_off, "postfix-if: span not rewritable"))
            continue
        if paren_only and reuse_parens:
            a, b = encl_open, stop + 1  # replace including the outer parens
        else:
            a, b = x_start, stop
            while a < b and str(logical[a]).isspace():
                a += 1
            while b > a and str(logical[b - 1]).isspace():
                b -= 1
        # The REPLACED range must be physically contiguous too: a C++
        # `"…" + var + "…"` splice inside the conditional would otherwise be
        # silently deleted by the replacement even when every extracted
        # sub-span (X/C/Y) individually sat inside one literal.
        if phys_span(text, logical, phys, a, b) is None:
            residue.append((if_off, "postfix-if: span not rewritable"))
            continue
        pa = phys[a]
        pb = phys_end(text, phys, b)
        if pa < 0 or pb <= pa:
            residue.append((if_off, "postfix-if: unmappable"))
            continue
        edits.append((pa, pb, atom))
        used.append((x_start, stop))
    return edits


# ── R4: expression-position case/when … end → (…) ───────────────────────────


def pass_case_when(text, logical, phys, residue):
    s = blank_bezel_noncode(logical)
    edits = []
    for m in re.finditer(r"\b(case|when)\b", s):
        kw = m.group(1)
        i = m.start()
        pk, pt = prev_token(s, i)
        is_expr = (pk == "punct" and pt in _EXPR_PREV_PUNCT) or \
                  (pk == "word" and pt in _EXPR_PREV_WORDS)
        if not is_expr:
            continue
        if kw == "case":
            # Skip the parse-support debug format (`case x of { … } end`,
            # `case <bare> of {`) — a rendering, not Bezel source.
            om = re.search(r"\bof\b", s[m.end():m.end() + 400])
            if om:
                after_of = s[m.end() + om.end():].lstrip()
                if after_of.startswith("{"):
                    continue
        end_off = scan_matching_end(s, m.end())
        if end_off < 0:
            residue.append((i, f"expr-{kw}: no matching `end`"))
            continue
        # Optional `end case` / `end when` tail word, then the arm/statement
        # terminator.
        tail_end = end_off + 3
        tm = re.match(r"(\s+)(case|when)\b(?=\s*[;,)\]])", s[tail_end:])
        if tm and tm.group(2) == kw:
            tail_end += tm.end(2)
        # Insert `(` before the keyword…
        pi = phys[i]
        if pi < 0:
            residue.append((i, f"expr-{kw}: unmappable"))
            continue
        edits.append((pi, pi, "("))
        # …and replace same-line-spaces+`end`[+tail] with `)`. The walk-back
        # tests the LOGICAL characters (blanked string interiors are spaces
        # and must never be consumed as whitespace) and never crosses a
        # newline: a line-leading `end` keeps its line so the `)` can never
        # land inside a preceding `//` comment.
        k = end_off
        while k > 0 and str(logical[k - 1]) in " \t":
            k -= 1
        if k > 0 and str(logical[k - 1]) in "\r\n":
            k = end_off
        p_ws = phys[k]
        p_end = phys[tail_end - 1] + 1
        if p_ws < 0 or p_end < 0:
            residue.append((i, f"expr-{kw}: unmappable end"))
            continue
        edits.append((p_ws, p_end, ")"))
    return edits


# ── driver ───────────────────────────────────────────────────────────────────


def apply_edits(text, edits):
    for (a, b, rep) in sorted(set(edits), key=lambda e: e[0], reverse=True):
        text = text[:a] + rep + text[b:]
    return text


def line_of(text, off):
    return text.count("\n", 0, off) + 1


def migrate_file(path):
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()
    if SKIP_MARKER in text:
        return 0, []
    total = 0
    residues = []
    passes = [
        ("with", lambda t, lg, ph, rs: pass_with(t, lg, ph, rs)),
        ("paren-if", lambda t, lg, ph, rs: pass_postfix_if(t, lg, ph, rs,
                                                           True)),
        ("bare-if", lambda t, lg, ph, rs: pass_postfix_if(t, lg, ph, rs,
                                                          False)),
        ("case-when", lambda t, lg, ph, rs: pass_case_when(t, lg, ph, rs)),
    ]
    for name, fn in passes:
        # Iterate each pass to a fixpoint (nested constructs surface after
        # the outer one is rewritten).
        for _ in range(10):
            if path.endswith(".bzl"):
                logical, phys = logical_from_bzl(text)
            else:
                logical, phys = logical_from_cpp(text)
            rs = []
            edits = fn(text, logical, phys, rs)
            for (off, why) in rs:
                p = phys[off] if 0 <= off < len(phys) and phys[off] >= 0 else 0
                residues.append((line_of(text, p), name + ": " + why))
            if not edits:
                break
            text = apply_edits(text, edits)
            total += len(set(edits)) // (2 if name in ("with", "case-when")
                                         else 1)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    # de-duplicate residue lines (fixpoint iterations re-report)
    residues = sorted(set(residues))
    return total, residues


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
    all_res = []
    for path in sorted(files):
        try:
            k, res = migrate_file(path)
        except Exception as e:  # noqa
            print(f"ERROR {path}: {e}", file=sys.stderr)
            continue
        for (ln, why) in res:
            all_res.append(f"RESIDUE {path}:{ln}: {why}")
        if k:
            total_files += 1
            total_sites += k
            print(f"{k:3d}  {path}")
    print(f"\n~{total_sites} rewrites across {total_files} files")
    if all_res:
        print(f"\n{len(all_res)} site(s) left untouched — triage by hand:")
        for r in all_res:
            print("  " + r)


if __name__ == "__main__":
    main(sys.argv)
