#!/usr/bin/env python3
"""extract_diag_to_lit.py — gtest → lit/FileCheck extractor for Bezel
ownership/sema DIAGNOSTIC tests.

Reads a `tests/{ownership,sema}/*_test.cpp` file, finds every `BEZEL_TEST`
block that drives the compiler through `parseAndCheckOwnership(...)` (or a
same-shaped harness call / a local boolean pass-through wrapper around it)
and asserts on `.diagnostics.hasErrors()`, resolves the concatenated C++
source-string expression into a standalone Bezel compilation unit, actually
COMPILES it with the real `bezelc` binary, and emits one `.bz` lit test per
case:

  - a REJECT case (`BEZEL_EXPECT_TRUE(...hasErrors...)`) emits
    `// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s` plus `// CHECK:` /
    `// CHECK-SAME:` lines built from bezelc's OWN diagnostic text (not
    invented) — the C++ test usually only asserts the boolean, so the actual
    message text has to come from running the compiler.
  - an ACCEPT case (`BEZEL_EXPECT_FALSE(...hasErrors...)`) emits
    `// RUN: %bezelc -c -o %t.o %s` (exit-zero is the pass signal) plus,
    depending on whether bezelc printed anything on stderr:
      * clean compile (no diagnostics) → a second RUN piping through
        FileCheck with `--check-prefix=NODIAG --allow-empty` and
        `NODIAG-NOT: error` / `NODIAG-NOT: §` (mirrors
        tests/lit/ref_out_local_accept_1682.bz);
      * compiles clean but a WARNING fires (e.g. an auto-drop must-use
        warning) → a second RUN piping through FileCheck with a positive
        `WARN:` check on the actual warning text (never NODIAG, since the
        output isn't empty and a blanket "no §" would be false).

WHY run the real compiler at all (design note, not just implementation
detail): almost none of the BEZEL_TEST bodies in tests/ownership assert a
SPECIFIC diagnostic substring — the overwhelming majority just assert the
boolean `hasErrors()`. There is no message text sitting in the C++ source to
copy. The only source of truth for "what does bezelc actually say here" is
bezelc itself, so this extractor treats the compiler as an oracle: resolve
the source, compile it, and let its own stderr become the CHECK text. Per
CLAUDE.md's "decline-with-reasons" discipline, this is recorded here rather
than silently assumed.

USAGE
    scripts/extract_diag_to_lit.py <test.cpp> [<test.cpp> ...] \\
        --bezelc build/bezelc --out-dir tests/lit/ownership [--dry-run]

Every test in the input file must be resolvable end-to-end (source built,
compiled, verdict matches, CHECK line derived) or the script reports it as
UNRESOLVED and does NOT emit a `.bz` for it — by design, so a caller can
decide whether "whole-file" deletion of the original gtest file is safe
(only when unresolved == 0 for that file).
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field

# =============================================================================
# Balanced C++-aware scanners (paren/brace, string/char/comment aware).
# Adapted from /var/tmp/bezel-scratch/impl-run-2026-07-12/aot-plan/survey_extract.py
# (the `_balanced` / `decode_arg` pair), generalized to also balance `{}`.
# =============================================================================


def find_matching(text: str, open_idx: int, open_ch: str, close_ch: str):
    """Given the index of an opening `open_ch`, return (inner_text,
    index_of_matching_close) skipping over string/char literals, raw string
    literals, and comments so an unbalanced delimiter inside a literal never
    confuses the scan."""
    depth = 0
    i, n = open_idx, len(text)
    start = open_idx + 1
    while i < n:
        c = text[i]
        if c == 'R' and i + 1 < n and text[i + 1] == '"':
            k = i + 2
            delim = ''
            while k < n and text[k] != '(':
                delim += text[k]
                k += 1
            close_marker = ')' + delim + '"'
            end = text.find(close_marker, k)
            if end == -1:
                return None, None
            i = end + len(close_marker)
            continue
        if c == '"':
            i += 1
            while i < n:
                if text[i] == '\\':
                    i += 2
                    continue
                if text[i] == '"':
                    break
                i += 1
            i += 1
            continue
        if c == "'":
            i += 1
            while i < n:
                if text[i] == '\\':
                    i += 2
                    continue
                if text[i] == "'":
                    break
                i += 1
            i += 1
            continue
        if c == '/' and i + 1 < n and text[i + 1] == '/':
            nl = text.find('\n', i)
            i = n if nl == -1 else nl
            continue
        if c == '/' and i + 1 < n and text[i + 1] == '*':
            end = text.find('*/', i)
            i = n if end == -1 else end + 2
            continue
        if c == open_ch:
            depth += 1
        elif c == close_ch:
            depth -= 1
            if depth == 0:
                return text[start:i], i
        i += 1
    return None, None


def find_calls(text: str, name: str, start: int = 0):
    """Yield (arg_text, call_start, call_end_exclusive) for every top-level
    call `name(...)` (word-boundary before `name`)."""
    pat = re.compile(r'(?<![A-Za-z0-9_])' + re.escape(name) + r'\(')
    out = []
    for m in pat.finditer(text, start):
        open_paren = m.end() - 1
        arg, close = find_matching(text, open_paren, '(', ')')
        if close is None:
            continue
        out.append((arg, m.start(), close + 1))
    return out


def decode_literal_run(arg: str):
    """If `arg` is purely one-or-more adjacent string literals (regular or
    raw, no operators), return the decoded concatenation; else None."""
    i, n = 0, len(arg)
    parts = []
    while i < n:
        c = arg[i]
        if c in ' \t\r\n':
            i += 1
            continue
        if c == '/' and i + 1 < n and arg[i + 1] == '/':
            nl = arg.find('\n', i)
            i = n if nl == -1 else nl
            continue
        if c == '/' and i + 1 < n and arg[i + 1] == '*':
            end = arg.find('*/', i)
            i = n if end == -1 else end + 2
            continue
        if c == 'R' and i + 1 < n and arg[i + 1] == '"':
            k = i + 2
            delim = ''
            while k < n and arg[k] != '(':
                delim += arg[k]
                k += 1
            close_marker = ')' + delim + '"'
            end = arg.find(close_marker, k)
            if end == -1:
                return None
            parts.append(arg[k + 1:end])
            i = end + len(close_marker)
            continue
        if c == '"':
            i += 1
            buf = []
            while i < n and arg[i] != '"':
                if arg[i] == '\\':
                    nxt = arg[i + 1] if i + 1 < n else ''
                    buf.append({'n': '\n', 't': '\t', 'r': '\r', '"': '"',
                                '\\': '\\', "'": "'", '0': '\0'}.get(nxt, nxt))
                    i += 2
                    continue
                buf.append(arg[i])
                i += 1
            parts.append(''.join(buf))
            i += 1
            continue
        # any other token -> not a pure literal run
        return None
    return ''.join(parts) if parts else None


def split_top_level(expr: str, sep: str = '+'):
    """Split `expr` on a top-level binary operator `sep`, honoring
    paren/string nesting (comment-aware)."""
    parts = []
    depth = 0
    i, n = 0, len(expr)
    start = 0
    while i < n:
        c = expr[i]
        if c == '"':
            i += 1
            while i < n:
                if expr[i] == '\\':
                    i += 2
                    continue
                if expr[i] == '"':
                    break
                i += 1
            i += 1
            continue
        if c == "'":
            i += 1
            while i < n:
                if expr[i] == '\\':
                    i += 2
                    continue
                if expr[i] == "'":
                    break
                i += 1
            i += 1
            continue
        if c == '/' and i + 1 < n and expr[i + 1] == '/':
            nl = expr.find('\n', i)
            i = n if nl == -1 else nl
            continue
        if c == '/' and i + 1 < n and expr[i + 1] == '*':
            end = expr.find('*/', i)
            i = n if end == -1 else end + 2
            continue
        if c in '([{':
            depth += 1
        elif c in ')]}':
            depth -= 1
        elif c == sep and depth == 0:
            parts.append(expr[start:i])
            start = i + 1
        i += 1
    parts.append(expr[start:])
    return parts


def find_top_level_semicolon(text: str, start: int):
    """Return the index of the next `;` at paren/brace depth 0 (string-aware),
    starting the scan at `start`."""
    depth = 0
    i, n = start, len(text)
    while i < n:
        c = text[i]
        if c == '"':
            i += 1
            while i < n:
                if text[i] == '\\':
                    i += 2
                    continue
                if text[i] == '"':
                    break
                i += 1
            i += 1
            continue
        if c == "'":
            i += 1
            while i < n:
                if text[i] == '\\':
                    i += 2
                    continue
                if text[i] == "'":
                    break
                i += 1
            i += 1
            continue
        if c == '/' and i + 1 < n and text[i + 1] == '/':
            nl = text.find('\n', i)
            i = n if nl == -1 else nl
            continue
        if c == '/' and i + 1 < n and text[i + 1] == '*':
            end = text.find('*/', i)
            i = n if end == -1 else end + 2
            continue
        if c in '([{':
            depth += 1
        elif c in ')]}':
            depth -= 1
        elif c == ';' and depth == 0:
            return i
        i += 1
    return -1


# =============================================================================
# Top-level declaration parsing: string constants + single-string-arg helper
# functions (the `mod(...)`/`wrap(...)`/`withTypes(...)` idiom used throughout
# tests/ownership to build a full `module M; ...` program around a fragment).
# =============================================================================


def mask_spans(text: str, spans):
    """Return a copy of `text` with the given [start,end) spans blanked out
    (kept the same length, so all other offsets remain valid) — used to hide
    BEZEL_TEST bodies while scanning for file-scope decls."""
    out = list(text)
    for s, e in spans:
        for i in range(s, e):
            if out[i] != '\n':
                out[i] = ' '
    return ''.join(out)


def find_bezel_tests(text: str):
    """Yield dicts {suite, case, body, body_start, body_end, decl_start} for
    each `BEZEL_TEST(suite, case) { body }`."""
    out = []
    for m in re.finditer(r'BEZEL_TEST\(\s*(\w+)\s*,\s*(\w+)\s*\)', text):
        j = text.find('{', m.end())
        if j == -1:
            continue
        body, close = find_matching(text, j, '{', '}')
        if body is None:
            continue
        out.append({
            'suite': m.group(1),
            'case': m.group(2),
            'body': body,
            'body_start': j + 1,
            'body_end': close,
            'decl_start': m.start(),
        })
    return out


def parse_consts(text_masked: str):
    """Return {name: decoded_str} for
    `[static ][constexpr ]const char* NAME = <lit-run>;` and
    `[static ]std::string NAME = <lit-run>;`."""
    consts = {}
    pat = re.compile(
        r'(?:static\s+)?(?:constexpr\s+)?(?:const\s+char\s*\*|std::string)\s+'
        r'(\w+)\s*=\s*'
    )
    for m in pat.finditer(text_masked):
        rhs_start = m.end()
        semi = find_top_level_semicolon(text_masked, rhs_start)
        if semi == -1:
            continue
        rhs = text_masked[rhs_start:semi].strip()
        dec = decode_literal_run(rhs)
        if dec is not None:
            consts[m.group(1)] = dec
    return consts


def extract_return_expr(body_text: str):
    """Find the first top-level `return <expr>;` in a (single-statement)
    helper body and return `<expr>` text, or None."""
    m = re.search(r'(?<![A-Za-z0-9_])return\s+', body_text)
    if not m:
        return None
    semi = find_top_level_semicolon(body_text, m.end())
    if semi == -1:
        return None
    return body_text[m.end():semi].strip()


def parse_string_helpers(text_masked: str):
    """Return {name: (param_name, return_expr_text)} for
    `std::string NAME(const std::string& PARAM) { ... return <expr>; ... }`."""
    helpers = {}
    pat = re.compile(
        r'std::string\s+(\w+)\s*\(\s*const\s+std::string\s*&\s*(\w+)\s*\)\s*\{'
    )
    for m in pat.finditer(text_masked):
        brace_open = m.end() - 1
        body, close = find_matching(text_masked, brace_open, '{', '}')
        if body is None:
            continue
        ret = extract_return_expr(body)
        if ret is not None:
            helpers[m.group(1)] = (m.group(2), ret)
    return helpers


def parse_bool_harness_wrappers(text_masked: str, harness_names):
    """Return {name: param_name} for
    `bool NAME(const std::string& PARAM) { return HARNESS(PARAM)...; }`
    where HARNESS is one of `harness_names` — i.e. a boolean pass-through
    wrapper whose sole job is to forward its argument into the harness and
    return `.diagnostics.hasErrors()`. A call to `NAME(x)` in a test body is
    then equivalent to a direct `HARNESS(x)` call for source-resolution
    purposes."""
    wrappers = {}
    pat = re.compile(
        r'bool\s+(\w+)\s*\(\s*const\s+std::string\s*&\s*(\w+)\s*\)\s*\{'
    )
    for m in pat.finditer(text_masked):
        brace_open = m.end() - 1
        body, close = find_matching(text_masked, brace_open, '{', '}')
        if body is None:
            continue
        ret = extract_return_expr(body)
        if ret is None:
            continue
        name, param = m.group(1), m.group(2)
        for hn in harness_names:
            if re.fullmatch(
                re.escape(hn) + r'\(\s*' + re.escape(param) + r'\s*\)'
                r'\.diagnostics\.hasErrors\(\)',
                ret,
            ):
                wrappers[name] = param
                break
    return wrappers


# =============================================================================
# Recursive resolver: turn a C++ string-expression into the plain Bezel
# source text it builds, using the file's consts + string-building helpers.
# =============================================================================


class Unresolvable(Exception):
    pass


def collapse_ws_outside_bezel_strings(src: str) -> str:
    """Collapse runs of whitespace to a single space, EXCEPT inside a Bezel
    (double-quoted) string literal in the resolved source — the C++ literal
    fragments this text was assembled from carry incidental multi-space
    padding at their concatenation seams (e.g. "...; " + "  private...") that
    is cosmetic in Bezel's whitespace-insensitive grammar but must not touch
    an actual Bezel string literal's contents."""
    out = []
    i, n = 0, len(src)
    in_str = False
    while i < n:
        c = src[i]
        if in_str:
            out.append(c)
            if c == '\\' and i + 1 < n:
                out.append(src[i + 1])
                i += 2
                continue
            if c == '"':
                in_str = False
            i += 1
            continue
        if c == '"':
            in_str = True
            out.append(c)
            i += 1
            continue
        if c in ' \t':
            j = i
            while j < n and src[j] in ' \t':
                j += 1
            out.append(' ')
            i = j
            continue
        out.append(c)
        i += 1
    return ''.join(out).strip()


def resolve_expr(expr: str, env: dict, funcs: dict, _depth: int = 0):
    if _depth > 20:
        raise Unresolvable(f"recursion too deep resolving: {expr[:80]!r}")
    parts = split_top_level(expr.strip(), '+')
    out = []
    for part in parts:
        part = part.strip()
        if not part:
            continue
        m = re.fullmatch(r'std::string\((.*)\)', part, re.S)
        if m:
            out.append(resolve_expr(m.group(1), env, funcs, _depth + 1))
            continue
        if re.fullmatch(r'[A-Za-z_]\w*', part):
            if part in env:
                out.append(env[part])
                continue
            raise Unresolvable(f"unbound identifier `{part}` in: {expr[:80]!r}")
        m2 = re.fullmatch(r'([A-Za-z_]\w*)\((.*)\)', part, re.S)
        if m2 and m2.group(1) in funcs:
            fname, argtext = m2.groups()
            argval = resolve_expr(argtext, env, funcs, _depth + 1)
            pname, fbody_expr = funcs[fname]
            out.append(resolve_expr(fbody_expr, {**env, pname: argval}, funcs,
                                     _depth + 1))
            continue
        dec = decode_literal_run(part)
        if dec is not None:
            out.append(dec)
            continue
        raise Unresolvable(f"cannot resolve term {part[:80]!r} in: {expr[:80]!r}")
    return ''.join(out)


# =============================================================================
# Per-test-case extraction.
# =============================================================================

HARNESS_NAMES = ('parseAndCheckOwnership', 'runJit')

# Text fragments that mark a diagnostic as a WARNING-shaped message (bezelc's
# CLI output carries no explicit severity word — see lit.cfg.py's comment —
# so this is a best-effort textual heuristic used only to prefer an ERROR-
# shaped line as the CHECK anchor when a test fires more than one diagnostic
# and the C++ source gave no specific substring to disambiguate with).
WARNING_ISH_PHRASES = (
    "implicitly disposed at the end of",
    "auto-drop",
    "after a dominating explicit Dispose",
    "§15.3",  # move-only reassignment-leak warning family
)


@dataclass
class CaseResult:
    suite: str
    case: str
    verdict: str  # 'reject' | 'accept'
    source: str | None = None
    status: str = 'unresolved'  # unresolved | ok | mismatch | error
    detail: str = ''
    bz_path: str | None = None
    needles: list = field(default_factory=list)


def classify_verdict(body: str, bool_wrappers: dict):
    """Return 'reject', 'accept', or None (ambiguous/undetected).
    Looks for the primary `BEZEL_EXPECT_(TRUE|FALSE)(...)` call whose
    argument is `<expr>.diagnostics.hasErrors()` or a call to a registered
    boolean harness-wrapper (a wrapper around the SOURCE string).

    FALLBACK (only consulted when no primary signal exists): some files
    assert via a locally-defined predicate over an ALREADY-COMPUTED
    OwnershipResult, e.g. `BEZEL_EXPECT_TRUE(hasFieldMovedFromError(r))` or
    `BEZEL_EXPECT_TRUE(hasOwnedTransferError(r))` — a helper that scans
    `r.diagnostics.diagnostics()` for a specific message substring rather
    than calling the boolean `hasErrors()` accessor directly. These are
    still a diagnostic-presence assertion (and the ownership-pass diagnostic
    text they hunt for is always an ERROR, per the source file's own
    "_rejected"/"_accepted" test-name suffix and comments), so a call whose
    callee name contains `Error`/`Reject` (case-insensitive-ish, matching the
    heuristic already validated against every tests/ownership file by this
    extractor's companion classify.py survey) is accepted as a secondary
    verdict signal — TRUE => reject, FALSE => accept — but ONLY when it is
    the sole kind of signal in the body, to stay conservative."""
    primary = set()
    fallback = set()
    for m in re.finditer(r'BEZEL_EXPECT_(TRUE|FALSE)\(', body):
        arg, close = find_matching(body, m.end() - 1, '(', ')')
        if arg is None:
            continue
        argn = arg.strip()
        sense = 'reject' if m.group(1) == 'TRUE' else 'accept'
        is_primary = bool(re.search(r'\.diagnostics\.hasErrors\(\)\s*$', argn))
        if not is_primary:
            m2 = re.fullmatch(r'(\w+)\((.*)\)', argn, re.S)
            if m2 and m2.group(1) in bool_wrappers:
                is_primary = True
        if is_primary:
            primary.add(sense)
            continue
        m3 = re.fullmatch(r'(\w+)\(.*\)', argn, re.S)
        if m3 and re.search(r'[Ee]rror|[Rr]eject', m3.group(1)):
            fallback.add(sense)
    if len(primary) == 1:
        return next(iter(primary))
    if not primary and len(fallback) == 1:
        return next(iter(fallback))
    return None


def find_harness_call(body: str, bool_wrappers: dict):
    """Return (source_arg_text) for the first call in `body` to a harness
    name or a registered boolean wrapper, or None."""
    names = list(HARNESS_NAMES) + list(bool_wrappers)
    best = None
    for name in names:
        calls = find_calls(body, name)
        if calls:
            arg, start, _end = calls[0]
            if best is None or start < best[1]:
                best = (arg, start)
    return best[0] if best else None


def collect_needles(body: str, harness_call_end: int):
    """Best-effort collection of literal-string needles used in predicate
    assertions AFTER the harness call (e.g. `diagContains(r, "§28.9")`,
    `.find("owned local")`, a helper called with a literal 2nd arg) — used
    only to disambiguate which diagnostic line to anchor the CHECK on when a
    test fires more than one diagnostic."""
    tail = body[harness_call_end:]
    needles = []
    for m in re.finditer(r'"((?:[^"\\]|\\.)*)"', tail):
        lit = decode_literal_run('"' + m.group(1) + '"')
        if lit and len(lit) >= 3:
            needles.append(lit)
    return needles


def run_bezelc(bezelc: str, source: str, tmpdir: str):
    """Write `source` to a temp .bz, compile it with bezelc -c, return
    (exit_code, stderr_lines: list[(line, col, msg)], raw_stderr)."""
    path = os.path.join(tmpdir, 'case.bz')
    with open(path, 'w') as fh:
        fh.write(source)
    proc = subprocess.run(
        [bezelc, '-c', '-o', os.path.join(tmpdir, 'case.o'), path],
        capture_output=True, text=True, timeout=30,
    )
    diags = []
    lineprefix = path + ':'
    for line in proc.stderr.splitlines():
        if line.startswith(lineprefix):
            rest = line[len(lineprefix):]
            m = re.match(r'(\d+):(\d+):\s(.*)$', rest)
            if m:
                diags.append((int(m.group(1)), int(m.group(2)), m.group(3)))
    return proc.returncode, diags, proc.stderr


def pick_diag(diags, needles):
    """Choose the anchor diagnostic among `diags` (list of (line,col,msg)).
    Prefer one matching ALL needles; else prefer a non-warning-ish message;
    else the first."""
    if needles:
        matches = [d for d in diags if all(nd in d[2] for nd in needles)]
        if len(matches) == 1:
            return matches[0]
        if len(matches) > 1:
            return matches[0]
    non_warnish = [d for d in diags
                   if not any(p in d[2] for p in WARNING_ISH_PHRASES)]
    if non_warnish:
        return non_warnish[0]
    return diags[0] if diags else None


def format_check_lines(line: int, col: int, msg: str, prefix: str = 'CHECK'):
    """Split `msg` at its first `(§` (section-citation open) into a
    CHECK + CHECK-SAME pair, mirroring tests/lit's hand-written exemplars;
    falls back to a single CHECK line if no citation is present."""
    loc = f":{{{{[0-9]+}}}}:{{{{[0-9]+}}}}: "
    idx = msg.find('(§')
    if idx == -1:
        return [f"// {prefix}: {loc}{msg}"]
    head = msg[:idx].rstrip()
    tail = msg[idx:]
    # tail up to the closing paren of the citation (or whole tail if no ')')
    close = tail.find(')')
    cite = tail[:close + 1] if close != -1 else tail
    return [
        f"// {prefix}: {loc}{head}",
        f"// {prefix}-SAME: {cite}",
    ]


def leading_comment(text: str, decl_start: int):
    """Return the contiguous block of `//` comment lines immediately above
    `decl_start` (the `BEZEL_TEST(` token), trimmed, or ''."""
    upto = text[:decl_start]
    lines = upto.splitlines()
    out = []
    i = len(lines) - 1
    while i >= 0:
        s = lines[i].strip()
        if s.startswith('//'):
            out.append(s[2:].strip())
            i -= 1
            continue
        if s == '':
            break
        break
    out.reverse()
    return '\n'.join(out)


def emit_bz(case: CaseResult, src_file: str, out_dir: str, comment: str):
    fname = f"{case.suite}__{case.case}.bz"
    path = os.path.join(out_dir, fname)
    lines = []
    ported_from = src_file
    if case.verdict == 'reject':
        lines.append("// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s")
        lines.append("//")
        lines.append(f"// Ported from {ported_from}")
        lines.append(f"//   (BEZEL_TEST {case.suite}, {case.case}).")
        if comment:
            lines.append("//")
            for cl in comment.splitlines():
                lines.append(f"// {cl}" if cl else "//")
        lines.append("")
        lines.append(case.source)
        lines.append("")
        lines.extend(case.check_lines)  # type: ignore[attr-defined]
    else:
        header = []
        header.append(f"// Ported from {ported_from}")
        header.append(f"//   (BEZEL_TEST {case.suite}, {case.case}).")
        if comment:
            header.append("//")
            for cl in comment.splitlines():
                header.append(f"// {cl}" if cl else "//")
        header.append("//")
        if case.status_kind == 'clean':  # type: ignore[attr-defined]
            header.append(
                "// Clean-compile ACCEPT control: bezelc -c must exit zero "
                "and emit no diagnostics."
            )
            header.append("//")
            header.append("// RUN: %bezelc -c -o %t.o %s")
            header.append(
                "// RUN: %bezelc -c %s 2>&1 | FileCheck "
                "--check-prefix=NODIAG --allow-empty %s"
            )
        else:
            header.append(
                "// ACCEPT-WITH-WARNING control: bezelc -c must exit zero "
                "(the program is legal), but it DOES emit a diagnostic — "
                "pinned below so the warning itself stays observed."
            )
            header.append("//")
            header.append("// RUN: %bezelc -c -o %t.o %s")
            header.append("// RUN: %bezelc -c %s 2>&1 | FileCheck %s")
        lines.extend(header)
        lines.append("")
        lines.append(case.source)
        lines.append("")
        if case.status_kind == 'clean':  # type: ignore[attr-defined]
            lines.append("// NODIAG-NOT: error")
            lines.append("// NODIAG-NOT: §")
        else:
            lines.extend(case.check_lines)  # type: ignore[attr-defined]
    with open(path, 'w') as fh:
        fh.write('\n'.join(lines) + '\n')
    return path


def process_file(cpp_path: str, bezelc: str, out_dir: str, dry_run: bool):
    with open(cpp_path, errors='replace') as fh:
        text = fh.read()

    tests = find_bezel_tests(text)
    masked = mask_spans(text, [(t['body_start'], t['body_end']) for t in tests])

    consts = parse_consts(masked)
    string_helpers = parse_string_helpers(masked)
    bool_wrappers = parse_bool_harness_wrappers(masked, HARNESS_NAMES)

    results = []
    # relative "tests/..." path for the header comment (search from the END so
    # a worktree path containing an earlier literal "tests" segment can't
    # produce a doubled "tests/tests/..." prefix).
    idx = cpp_path.rfind('tests' + os.sep)
    src_for_header = cpp_path[idx:] if idx != -1 else cpp_path

    with tempfile.TemporaryDirectory(dir='/var/tmp/bezel-scratch') as tmpdir:
        for t in tests:
            verdict = classify_verdict(t['body'], bool_wrappers)
            case = CaseResult(suite=t['suite'], case=t['case'], verdict=verdict or '?')
            if verdict is None:
                case.status = 'unresolved'
                case.detail = 'verdict not detected (no primary hasErrors() signal)'
                results.append(case)
                continue

            call = find_harness_call(t['body'], bool_wrappers)
            if call is None:
                case.status = 'unresolved'
                case.detail = 'no harness call found'
                results.append(case)
                continue

            try:
                source = resolve_expr(call, consts, string_helpers)
            except Unresolvable as e:
                case.status = 'unresolved'
                case.detail = f'source resolution failed: {e}'
                results.append(case)
                continue

            case.source = collapse_ws_outside_bezel_strings(source)

            rc, diags, raw_err = run_bezelc(bezelc, case.source, tmpdir)
            actual_verdict = 'reject' if rc != 0 else 'accept'
            if actual_verdict != verdict:
                case.status = 'mismatch'
                case.detail = (
                    f'expected {verdict}, bezelc exit={rc} '
                    f'(actual {actual_verdict}); stderr={raw_err.strip()[:300]!r}'
                )
                results.append(case)
                continue

            if verdict == 'reject':
                needles = collect_needles(
                    t['body'], t['body'].find(call) + len(call)
                )
                case.needles = needles
                anchor = pick_diag(diags, needles)
                if anchor is None:
                    case.status = 'unresolved'
                    case.detail = 'reject but no diagnostic line captured'
                    results.append(case)
                    continue
                line, col, msg = anchor
                case.check_lines = format_check_lines(line, col, msg)  # type: ignore[attr-defined]
                case.status = 'ok'
            else:
                if not diags:
                    case.status_kind = 'clean'  # type: ignore[attr-defined]
                    case.status = 'ok'
                else:
                    case.status_kind = 'warn'  # type: ignore[attr-defined]
                    line, col, msg = diags[0]
                    case.check_lines = format_check_lines(line, col, msg)  # type: ignore[attr-defined]
                    case.status = 'ok'

            if case.status == 'ok' and not dry_run:
                comment = leading_comment(text, t['decl_start'])
                case.bz_path = emit_bz(case, src_for_header, out_dir, comment)

            results.append(case)

    return results, len(tests)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('cpp_files', nargs='+')
    ap.add_argument('--bezelc', required=True)
    ap.add_argument('--out-dir', required=True)
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('--report', default=None, help='write a JSON report here')
    args = ap.parse_args()

    os.makedirs(args.out_dir, exist_ok=True)

    all_report = {}
    total_ok = total_cases = total_files_clean = 0
    for cpp in args.cpp_files:
        results, ntests = process_file(cpp, args.bezelc, args.out_dir, args.dry_run)
        ok = sum(1 for r in results if r.status == 'ok')
        total_ok += ok
        total_cases += len(results)
        file_clean = ok == len(results) == ntests
        if file_clean:
            total_files_clean += 1
        print(f"{cpp}: {ok}/{len(results)} cases resolved+emitted"
              f"{' (FULL FILE COVERAGE)' if file_clean else ''}")
        for r in results:
            if r.status != 'ok':
                print(f"    [{r.status}] {r.suite}.{r.case}: {r.detail}")
        all_report[cpp] = {
            'total_tests': ntests,
            'ok': ok,
            'cases': [
                {
                    'suite': r.suite, 'case': r.case, 'verdict': r.verdict,
                    'status': r.status, 'detail': r.detail, 'bz_path': r.bz_path,
                }
                for r in results
            ],
            'full_file_coverage': file_clean,
        }

    print(f"\nTOTAL: {total_ok}/{total_cases} cases across {len(args.cpp_files)} files; "
          f"{total_files_clean}/{len(args.cpp_files)} files fully covered")

    if args.report:
        with open(args.report, 'w') as fh:
            json.dump(all_report, fh, indent=2)


if __name__ == '__main__':
    main()
