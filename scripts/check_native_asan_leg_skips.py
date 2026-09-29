#!/usr/bin/env python3
"""check_native_asan_leg_skips.py - ratchet the native ASan-leg skip surface.

WHY (#3682, adjacent #3676, spec §2 of docs/proposals/oracle-ratchets-2026-09.md).
The per-file native ASan leg funnels through `tests/support/run_under_asan.hpp`
(or one of the per-file copies of that helper, of which ~82 still exist).
Its first act is
    if (rtObj.empty()) return -1;
so a missing/broken ASan runtime reads as "toolchain unavailable: SKIP" to
every caller. ~165 test sites have a `if (<var> == -1) return;` (or `< 0`)
guarding the ASan leg's result, and that guard was the load-bearing defect:
when the ASan runtime is unavailable, the guard lets the row report a clean
PASS while the leg is completely off, which is exactly how #3653's
`detect_leaks` regression reached the gate (#3682, R12 gate 1, 2026-09-03).

The runtime fix lands in commit 2: `sanitizerRuntimeMaster()` refuses the
empty result when `bezel::test::forceSanitizeAddress()` is true (under
`gate.sh --full` / `agent_check.sh --probe --asan` it now fails the test
instead of returning `""` for the caller to swallow). That fix is
load-bearing but small — it changes one chokepoint. THIS ratchet freezes
the SOURCE surface so the bug cannot silently regrow under the runtime fix.

Two banked per-file counts (spec §2 "What the ratchet asserts"):

  swallowing_guards
    A site is a swallowing guard when a variable bound from an ASan-leg
    call (`runUnderAsan*`, `runUnderASan*`, a direct `asanRuntimeObject()`,
    or `buildRuntimeSanitizerObject()`) is tested against a negative
    literal (`== -1`, `< 0`) and the guarded statement RETURNS without
    asserting on the ASan-leg variable. Both spellings count:
    `if (x == -1) return;` and `if (x < 0) return;`.

    EXEMPT, and why:
      - `if (x != -1) BEZEL_EXPECT_EQ(x, 0);`   - asserts on non-skip codes.
      - `if (x < 0) { BEZEL_EXPECT_TRUE(false); }`  - asserts unconditionally.
      - The shared implementation in `tests/support/run_under_asan.hpp`
        itself — exempt by path, exactly as the sibling scripts exempt
        `tests/support/`.

  conflating_helpers
    The #3676 shape: a function body that contains BOTH a
    `-fsanitize=address` link command AND a `return -1;` AFTER the link
    command (the post-link return is the bug — pre-link `return -1` for
    `if (rtObj.empty())` is the canonical "toolchain unavailable" return
    and stays correct). After #3676 closed, link failure is `-3`, so any
    helper still emitting `return -1;` for a link failure is conflating
    two failure classes.

MARKERS (per spec §2; the LOCALDECL-ONLY / LOCALDECL-DEBT mechanism,
check_localdecl_only_walkers.py:75-100):

    // ASAN-LEG-ONLY: <reason>
        Removes the site from the count. For sites where the swallow is
        genuinely correct — a plain developer run on a box with no ASan
        runtime, where the silent skip is the right behaviour. A marker
        without a reason after the colon does NOT exempt.

    // ASAN-LEG-DEBT: #NNNN <reason>
        Does NOT remove it from the count. Justifies a rise above the
        per-file ceiling in scripts/native_asan_leg_skips.grandfather,
        which only ever falls. The `#NNNN` must sit immediately after the
        colon.

THE BASELINE IS EXACT, NOT A CEILING — same rule every ratchet in this
file's family uses. A count BELOW baseline fails as STALE HEADROOM
(check_deprecated_jit_oracle.py:243-247); a count above the grandfathered
ceiling needs a per-site `// ASAN-LEG-DEBT: #NNNN <reason>` marker.

SEEDS (spec §2 "What the baseline file holds", measured 2026-09-17):
  - swallowing_guards ≈ 165 lines (upper bound from a line-based grep;
    the script must bind the variable to its call so the real number is
    a strict subset — see Q5 in the spec)
  - conflating_helpers = 1 (just `domain_spawn_choice_valuearg_3181_test.cpp`,
    fixed by commit 1; re-measure, the spec's seed predates the fix's
    broader audit and the tree has moved)

Usage:  scripts/check_native_asan_leg_skips.py [--strict] [--self-test]
                                            [--update [--force]] [--list]
"""
import argparse, pathlib, re, sys, tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
BASELINE = ROOT / 'scripts' / 'native_asan_leg_skips.baseline'
GRANDFATHER = ROOT / 'scripts' / 'native_asan_leg_skips.grandfather'

# The native ASan-leg is the support header `tests/support/run_under_asan.hpp`
# (per #3653) plus the family-B/C per-cause helpers it exposes. Every test
# file that participates in the leg calls one of these — direct, qualified,
# or through a using declaration. The shape is the same: a function whose
# first act is `if (rtObj.empty()) return -1;` and which then links under
# `-fsanitize=address` and runs under `ASAN_OPTIONS=...`.
#
# Spec §2 names the call tokens explicitly: `runUnderAsan`,
# `runUnderAsanValue`, `runUnderAsanExitCode`, any identifier matching
# `runUnderASan\\w*`, plus direct `asanRuntimeObject()` and
# `buildRuntimeSanitizerObject()`. We accept an optional
# `bezel::test::`/`bezel::native_test::` qualifier (the codebase's own
# idiom, several converted copies use the qualified form).
ASAN_CALL = re.compile(
    r'\b(?:bezel::test::|bezel::native_test::)?'
    r'(?:runUnderAsan(?:Value|ExitCode)?|runUnderASan\w*|asanRuntimeObject|'
    r'buildRuntimeSanitizerObject)\s*\(')

# The swallow shape: `if (<var> < 0) ...;` OR `if (<var> == -1) ...;` where
# the body is a `return;` (possibly with a preceding comment). The variable
# MUST be bound from an ASAN_CALL earlier in the same BEZEL_TEST body —
# the script enforces that, the grep the spec quotes cannot.
SWALLOW = re.compile(
    r'\bif\s*\(\s*(\w+)\s*(?:==\s*-?1\b|<\s*0\b)\s*\)\s*'
    r'(?://[^\n]*\n\s*)?return\b')

BEZEL_TEST = re.compile(r'BEZEL_TEST\s*\(\s*(\w+)\s*,\s*(\w+)\s*\)\s*\{', re.S)

# An assertion against the same variable INSIDE the guarded body would
# mean the guard is not swallowing. The grep'd shape `if (x < 0) return;`
# with NO `BEZEL_EXPECT_*` token on the same line is the load-bearing
# signal of a swallow. (A braced `if (x < 0) { BEZEL_EXPECT_TRUE(false); }`
# is also exempt — `BEZEL_EXPECT_TRUE` on the same line as `return` is the
# unusual-but-not-impossible shape, see test fixtures.)
ASSERT_TOKEN = re.compile(r'\bBEZEL_EXPECT_(?:TRUE|FALSE|EQ|NE|STREQ|GE|LE)\b')

MARKER_ONLY = re.compile(r'//\s*ASAN-LEG-ONLY:\s*\S')
MARKER_DEBT = re.compile(r'//\s*ASAN-LEG-DEBT:\s*(\S.*)$')
DEBT_ISSUE = re.compile(r'^#(\d+)\b')


# -----------------------------------------------------------------------
# Conflating-helper detection. The shape: a function body containing BOTH
# `-fsanitize=address` (the link command, usually inside `linkCmd = "..."`)
# AND `return -1;` AFTER the link command (the bug — pre-link `return -1`
# for `if (rtObj.empty())` is correct). One row per offending function.
#
# Implementation is intentionally simple: per-function brace-balanced scan
# over the comment-stripped text. Detects the bug pattern without false-
# positives from prose, raw strings, or character literals.
# -----------------------------------------------------------------------

def strip_strings_and_comments(text):
    """Blank out // and /* */ comments and string/char literals, preserving
    length and newlines. A prose mention of `return -1` in a comment
    must not count as a code return."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c == '/' and i + 1 < n and text[i + 1] == '/':
            j = text.find('\n', i)
            j = n if j < 0 else j
            out.append(' ' * (j - i)); i = j
        elif c == '/' and i + 1 < n and text[i + 1] == '*':
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append(''.join(ch if ch == '\n' else ' ' for ch in text[i:j]))
            i = j
        elif c == 'R' and i + 1 < n and text[i + 1] == '"':
            m = re.match(r'R"([^()\\\s]{0,16})\(', text[i:])
            if m:
                delim = m.group(1)
                end_marker = ')' + delim + '"'
                j = text.find(end_marker, i + m.end())
                j = n if j < 0 else j + len(end_marker)
                out.append(''.join(ch if ch == '\n' else ' ' for ch in text[i:j]))
                i = j
                continue
            out.append(c); i += 1
        elif c == '"':
            q, j = c, i + 1
            while j < n and text[j] != q:
                j += 2 if text[j] == '\\' else 1
            j = min(j + 1, n)
            out.append(text[i:j]); i = j
        elif c == "'":
            q, j = c, i + 1
            while j < n and text[j] != q:
                j += 2 if text[j] == '\\' else 1
            j = min(j + 1, n)
            out.append(text[i:j]); i = j
        else:
            out.append(c); i += 1
    return ''.join(out)


def _is_function_header(header):
    """True if the text immediately before a `{` looks like it opens a
    function or lambda body — i.e., has an argument list (parens) before
    the brace. Excludes control-flow blocks (`if`, `for`, `while`, ...),
    bare `else`/`do`/`try`, namespace/class/enum/struct/union bodies."""
    h = header.strip()
    if not h:
        return False
    # Quick reject: control-flow keywords and aggregate-init keywords
    if h in ('do', 'else', 'try'):
        return False
    if re.match(r'^(else\s+)?(if|for|while|switch|catch)\s*\(', h):
        return False
    if re.match(r'^(namespace|class|struct|enum(\s+class)?|union)\b', h):
        return False
    # Must contain parens (argument list) — best signal of a function body.
    if '(' not in h:
        return False
    return True


def build_function_index(text):
    """Return [(open_pos, close_pos), ...] for every function/lambda body
    in `text`, properly nested via a brace stack over the stripped text.
    `open_pos`/`close_pos` are the offsets of the `{` and matching `}`.

    Pre-fix, this pushed only function-header `{` onto a stack and popped
    on every `}`, so a nested non-function `}` (e.g. the inline
    `{ bezel::DiagnosticEngine diags; ... }` block in runUnderASan3181)
    would pop the outer function's `{` and truncate the body before any
    `-fsanitize=address` link string — the seed at #3676 was structurally
    invisible to the ratchet. Now every `{` is pushed with its
    function-header flag, and a `}` pops the topmost `{` (which is its
    actual match by brace balance); the block is recorded only if the
    popped `{` was a function header. Inner non-function `{}` pairs close
    against themselves, leaving the outer function body intact.
    """
    blocks, stack = [], []
    n = len(text)
    last_boundary = 0
    paren_depth = 0
    for i in range(n):
        c = text[i]
        if c == '(':
            paren_depth += 1
        elif c == ')':
            if paren_depth > 0:
                paren_depth -= 1
        elif c == '{':
            header = text[last_boundary:i]
            is_fn = _is_function_header(header)
            stack.append((i, is_fn))
            last_boundary = i + 1
            paren_depth = 0
        elif c == '}':
            if stack:
                open_pos, is_fn = stack.pop()
                if is_fn:
                    blocks.append((open_pos, i))
            last_boundary = i + 1
            paren_depth = 0
        elif c == ';' and paren_depth == 0:
            last_boundary = i + 1
    return blocks


def scan_conflating_helpers(text, path):
    """Return a list of (line, function_name) for each function body that
    contains BOTH a `-fsanitize=address` link command AND a post-link
    `return -1;`.

    PRE-LINK CHECK (Bug 2 fix, ratchet-3682 detector): the body must also
    contain a `return -1;` BEFORE the link command. Every canonical
    ASan-leg helper (`runUnderAsan`, `runUnderAsanValue`,
    `runUnderAsanExitCode`, the per-file copies like `runUnderLeakSanitizer`
    that the #3676 fix has not yet migrated, and the named helpers
    `asanRuntimeObject` / `buildRuntimeSanitizerObject` /
    `sanitizerRuntimeMaster`) opens with the toolchain-unavailable check
    `if (rtObj.empty()) return -1;` (per `tests/support/run_under_asan.hpp`
    and its per-file copies). The `return -1;` after that check is
    canonical and semantically correct (it is the `-1` code that the
    downstream `if (... == -1) return;` swallowing guard reads as
    "toolchain unavailable"). The `return -1;` AFTER the link command
    is the #3676 bug — link failure should be `-3`, not `-1`.

    Requiring BOTH a pre-link and a post-link `return -1;` filters out a
    hypothetical false-positive class: a function that happens to
    compose a `linkCmd = "clang++ -fsanitize=address -g ..."` string for
    an unrelated purpose (a doc helper, a build-Doc script, an unrelated
    early-exit) and has an unrelated post-link `return -1;` — but is NOT
    an ASan-leg helper, so it has no canonical pre-link toolchain check.
    The rebanked baseline (73 sites across 71 files) was measured against
    functions that all have the canonical structure, so the pre-link
    check keeps the count at 73 while excluding the hypothetical class.

    The pre-link check matches `return -1;` anywhere before the link
    command's text position, not just as the first statement: a few
    helpers bind `rtObj` first then check empty (the canonical shape),
    and one (tests/codegen/async_lambda_488_test.cpp) uses a
    clang-availability check followed by a `buildRuntimeSanitizerObject`
    failure check rather than `rtObj.empty()` — both forms have a
    pre-link `return -1;` and all three match the filter. Anything that
    lacks a pre-link `return -1;` is, by construction, not one of those
    helpers and not a conflation.
    """
    stripped = strip_strings_and_comments(text)
    blocks = build_function_index(stripped)
    out = []
    PRE_LINK_RE = re.compile(r'\breturn\s+-1\s*;')
    for open_pos, close_pos in blocks:
        body = stripped[open_pos:close_pos + 1]
        fsan_pos = body.find('-fsanitize=address')
        if fsan_pos < 0:
            continue
        # Require a pre-link `return -1;` (the canonical toolchain-unavailable
        # check). Without it, this is not one of the ASan-leg helpers and
        # any post-link `return -1;` is unrelated.
        if not PRE_LINK_RE.search(body, 0, fsan_pos):
            continue
        # Look for a `return -1;` AFTER the link command.
        post = body.find('return -1', fsan_pos)
        if post < 0:
            continue
        # Also confirm the post-link return is not in a nested function body
        # — the inner braces between fsan_pos and post must all close to
        # outer scope. Quick check: count braces between fsan_pos and post.
        depth = 0
        nested = False
        for ch in body[fsan_pos:post]:
            if ch == '{':
                depth += 1
            elif ch == '}':
                depth -= 1
                if depth < 0:
                    nested = True
                    break
        if nested:
            continue
        # Find the function's name from the raw text — best effort.
        raw_header = text[:open_pos].rsplit(';', 1)[-1].rsplit('{', 1)[0]
        # Last identifier in the header before `{`.
        m = re.search(r'(\w+)\s*$', raw_header)
        name = m.group(1) if m else '<anon>'
        # Convert fsan_pos (relative to body) to a 1-indexed line.
        line = text.count('\n', 0, open_pos + fsan_pos) + 1
        out.append((path, line, name))
    return out


# -----------------------------------------------------------------------
# Swallowing-guard detection. Per (file, BEZEL_TEST body, variable): a
# variable bound from an ASAN_CALL, tested against a negative literal,
# guarded statement is a bare `return` (no assertion on the same line).
# -----------------------------------------------------------------------

def _balanced_body(text, open_brace_idx):
    """Return (body_start, body_end) — the text between the brace at
    open_brace_idx and its match. Unmatched returns (None, None)."""
    depth = 0
    for j in range(open_brace_idx, len(text)):
        if text[j] == '{':
            depth += 1
        elif text[j] == '}':
            depth -= 1
            if depth == 0:
                return open_brace_idx + 1, j
    return None, None


def scan_swallowing_guards(text, path):
    """Return a list of (path, line, test_name, var_name) for each
    swallowing guard in `text`. One per (file, BEZEL_TEST body, variable)
    binding that flows into a `if (<var> < 0|<var> == -1) return;`."""
    sites = []
    for m in BEZEL_TEST.finditer(text):
        test_name = m.group(2)
        body_start, body_end = _balanced_body(text, m.end() - 1)
        if body_start is None:
            continue
        body = text[body_start:body_end]
        # Find every `<var> = ASAN_CALL(...)` binding in this body.
        asan_bindings = set()
        for b in ASAN_CALL.finditer(body):
            # Walk backwards to the start of the binding statement.
            # Heuristic: the binding is `<var> = ASAN_CALL(...)` or
            # `auto <var> = ASAN_CALL(...)`. Search backwards for `=`
            # at the same paren-depth.
            eq = body.rfind('=', 0, b.start())
            if eq < 0:
                continue
            # Find the identifier immediately before `=`.
            seg = body[max(0, eq - 80):eq]
            ident_m = re.search(r'(\b\w+)\s*$', seg)
            if ident_m:
                asan_bindings.add(ident_m.group(1))
        if not asan_bindings:
            continue
        # For each binding, look for a swallow guard on the same variable
        # in the same body, after the binding site.
        for var in asan_bindings:
            for s in SWALLOW.finditer(body):
                if s.group(1) != var:
                    continue
                # Same-line assertion => NOT a swallow.
                line_start = body.rfind('\n', 0, s.start()) + 1
                line_end = body.find('\n', s.end())
                if line_end < 0:
                    line_end = len(body)
                line = body[line_start:line_end]
                if ASSERT_TOKEN.search(line):
                    continue
                # The guard is a swallow.
                line_no = text.count('\n', 0, body_start + s.start()) + 1
                sites.append((path, line_no, test_name, var))
    return sites


# -----------------------------------------------------------------------
# Markers: per-site exemption (`ASAN-LEG-ONLY`) and per-site debt
# (`ASAN-LEG-DEBT: #NNNN`). A marker exempts the next matching swallow
# within a small window (8 lines, same as LOCALDECL-ONLY).
# -----------------------------------------------------------------------

MARKER_WINDOW = 8


def exempt_lines_for_only(raw):
    """1-indexed line numbers exempted by ASAN-LEG-ONLY markers."""
    out, lines = set(), raw.split('\n')
    for i, ln in enumerate(lines):
        if not MARKER_ONLY.search(ln):
            continue
        out.add(i + 1)
        for j in range(i + 1, min(i + MARKER_WINDOW, len(lines))):
            s = lines[j].strip()
            out.add(j + 1)
            if s and not s.startswith('//'):
                break
    return out


def debt_lines_in_file(path, raw):
    """([(line, issue)] for WELL-FORMED ASAN-LEG-DEBT markers in this
    file, [line] for malformed ones (a reason with no leading `#NNNN`,
    which justifies nothing)."""
    well, malformed = [], []
    for i, ln in enumerate(raw.split('\n')):
        m = MARKER_DEBT.search(ln)
        if not m:
            continue
        im = DEBT_ISSUE.match(m.group(1).strip())
        if im:
            well.append((i + 1, int(im.group(1))))
        else:
            malformed.append(i + 1)
    return well, malformed


def debt_lines(path, raw):
    """[((file, line, issue), ...)] for well-formed markers — convenience
    wrapper that prepends the file path so callers don't have to. The
    malformed list is returned as a single arg too so this one call
    covers both gates' reports."""
    well, malformed = debt_lines_in_file(path, raw)
    return [(path, line, issue) for line, issue in well], malformed


def attach_markers(sites, exempt_lines):
    """Drop sites whose line is in `exempt_lines` (a swallowing guard with
    an `ASAN-LEG-ONLY:` marker in its window)."""
    return [(p, ln, tn, var) for (p, ln, tn, var) in sites
            if ln not in exempt_lines]


def attach_debt_markers(sites, debt_markers):
    """{site: issue} — for sites carrying a well-formed `ASAN-LEG-DEBT`
    marker in their window. `debt_markers` is a list of (file, line,
    issue) triples — caller is responsible for scanning every file and
    building the list."""
    debt = {}
    by_file = {}
    for s in sites:
        by_file.setdefault(s[0], []).append(s)
    for path, m_line, m_issue in debt_markers:
        for s in by_file.get(path, []):
            if abs(s[1] - m_line) <= MARKER_WINDOW and s not in debt:
                debt[s] = m_issue
                break
    return debt


# -----------------------------------------------------------------------
# File / dir walking. `tests/support/` is exempt for swallowing_guards
# because it IS the shared implementation; it is NOT exempt for
# conflating_helpers because the canonical shapes have been audited.
# -----------------------------------------------------------------------

SCAN_DIR = ROOT / 'tests'
EXEMPT_PATH = 'tests/support/'
SUFFIXES = ('.cpp', '.hpp')


def _is_exempt(path):
    return EXEMPT_PATH in str(path)


def scan(root):
    """Return (swallow_sites, conflate_sites, malformed_debt) for every
    .cpp/.hpp file under `root/tests/`, excluding the shared
    implementation in `tests/support/` for swallowing_guards.

    SCOPE (per spec §2 "What the ratchet asserts"): only `tests/` is in
    scope. A whole-tree `root.rglob('*')` walk would scan `src/codegen/`,
    `src/driver/`, and other non-test TUs that contain `-fsanitize=address`
    for unrelated reasons (e.g., a doc string describing the flag), and
    on a worktree that has nested worktrees under `.claude/worktrees/`,
    those nested worktrees' OWN `tests/` trees get scanned as if they
    were the current tree's tests — which made the detector report 516
    conflations in r18-c7-coord when the rebanked baseline (manual,
    for the current tree) was 73. The `SCAN_DIR` constant at line 438
    names the right scope; this function uses it directly.
    """
    swallow, conflate, malformed = [], [], []
    scan_dir = root / 'tests'
    if not scan_dir.is_dir():
        # No tests/ — nothing in scope. (A worktree without a tests/ tree
        # is a config error elsewhere; here we just return empty.)
        return swallow, conflate, malformed
    for p in sorted(scan_dir.rglob('*')):
        if not p.is_file() or p.suffix not in SUFFIXES:
            continue
        rel = str(p.relative_to(root))
        raw = p.read_text()
        if not _is_exempt(rel):
            swallow += scan_swallowing_guards(raw, rel)
        conflate += scan_conflating_helpers(raw, rel)
        _, mal = debt_lines(rel, raw)
        malformed += [(rel, ln) for ln in mal]
    return swallow, conflate, malformed


def count_per_file(sites):
    """{path: N} for swallowing guards or conflating helpers."""
    out = {}
    for s in sites:
        out[s[0]] = out.get(s[0], 0) + 1
    return out


# -----------------------------------------------------------------------
# Baseline / grandfather file handling. Same shape as the localdecl
# ratchet's `_parse_counts_file` — but this script has TWO per-file
# counts, so the file has two sections.
# -----------------------------------------------------------------------

class BaselineConflict(Exception):
    pass


def _parse_section(lines, source_name):
    out = {}
    for raw in lines:
        line = raw.strip()
        if line.startswith(('<<<<<<<', '=======', '>>>>>>>')):
            raise BaselineConflict(
                '%s has unresolved merge conflict markers.\n'
                '  Do NOT hand-merge the counts.\n'
                '  For the mutable baseline (scripts/native_asan_leg_'
                'skips.baseline):\n'
                '      git checkout --ours scripts/native_asan_leg_skips.baseline\n'
                '      python3 scripts/check_native_asan_leg_skips.py --update\n'
                '  For the frozen grandfather ceiling (scripts/native_asan_'
                'leg_skips.grandfather): there is no --update for this '
                'file — it changes only by a deliberate, reviewed edit, '
                'so resolve it by hand.' % source_name)
        if not line or line.startswith('#'):
            continue
        n, path = line.split(None, 1)
        out[path] = int(n)
    return out


def read_baseline():
    """Per-file rows in two sections. Returns
    (swallow: {path: N}, conflate: {path: N})."""
    if not BASELINE.exists():
        return {}, {}
    text = BASELINE.read_text()
    swallow, conflate = {}, {}
    section = None
    for ln in text.splitlines():
        s = ln.strip()
        if s.startswith('[conflating_helpers]'):
            section = 'conflate'
            continue
        if s.startswith('[swallowing_guards]'):
            section = 'swallow'
            continue
        if not s or s.startswith('#'):
            continue
        n, path = s.split(None, 1)
        if section == 'swallow':
            swallow[path] = int(n)
        elif section == 'conflate':
            conflate[path] = int(n)
    return swallow, conflate


def read_grandfather():
    """Same shape as baseline — per-file rows in two sections."""
    if not GRANDFATHER.exists():
        return {}, {}
    text = GRANDFATHER.read_text()
    swallow, conflate = {}, {}
    section = None
    for ln in text.splitlines():
        s = ln.strip()
        if s.startswith('[conflating_helpers]'):
            section = 'conflate'
            continue
        if s.startswith('[swallowing_guards]'):
            section = 'swallow'
            continue
        if not s or s.startswith('#'):
            continue
        n, path = s.split(None, 1)
        if section == 'swallow':
            swallow[path] = int(n)
        elif section == 'conflate':
            conflate[path] = int(n)
    return swallow, conflate


def write_baseline(swallow, conflate):
    lines = [
        '# native_asan_leg_skips.baseline - the ratchet for #3682 / spec §2.',
        '#',
        '# Regenerate with scripts/check_native_asan_leg_skips.py --update',
        '# after fixing a swallowing guard or a conflation helper. The gate',
        '# requires this file to EQUAL the measured count, so BOTH a rise and',
        '# stale headroom fail — a landed fix that forgot --update is a',
        '# violation exactly like a regression, which is what stops a later',
        '# change from quietly handing back what an earlier one ratcheted',
        '# away. Raising a count needs --force, and once the rise pushes a',
        '# file above its ceiling in scripts/native_asan_leg_skips.grandfather',
        '# (which only ever falls — see that file\'s header), the excess needs',
        '# a `// ASAN-LEG-DEBT: #NNNN <reason>` marker at each new site.',
        '#',
        '# Two per-file sections — one row per file:',
        '#   [swallowing_guards]   if (<asan-var>) < 0|== -1) return;  sites',
        '#   [conflating_helpers]  function bodies with both -fsanitize=address',
        '#                          AND a return -1; AFTER the link command',
        '#                          (the #3676 shape, post-fix zero).',
        '#',
        '# See scripts/check_native_asan_leg_skips.py module docstring and',
        '# docs/proposals/oracle-ratchets-2026-09.md §2 / issue #3682.',
        '',
        '[swallowing_guards]',
    ]
    for path, n in sorted(swallow.items()):
        lines.append('%-4d %s' % (n, path))
    lines.append('')
    lines.append('[conflating_helpers]')
    for path, n in sorted(conflate.items()):
        lines.append('%-4d %s' % (n, path))
    BASELINE.write_text('\n'.join(lines) + '\n')


def tighten_ceiling(existing, cur):
    """{path: N} — MONOTONE DOWNWARD ONLY: new[path] = min(existing.get(
    path, 0), cur.get(path, 0)). When `existing` is empty (the first
    `--update` seeds the file from the current tree), seed with `cur`
    directly — there is no prior ceiling to violate, only the measured
    values to bank."""
    if not existing:
        out = {}
        for path, n in cur.items():
            if n > 0:
                out[path] = n
        return out
    out = {}
    for path in set(existing) | set(cur):
        new = min(existing.get(path, 0), cur.get(path, 0))
        if new > 0:
            out[path] = new
    return out


def write_grandfather(existing_swallow, existing_conflate, cur_swallow, cur_conflate):
    """Tighten per-file ceilings on every successful --update."""
    new_swallow = tighten_ceiling(existing_swallow, cur_swallow)
    new_conflate = tighten_ceiling(existing_conflate, cur_conflate)
    lines = [
        '# native_asan_leg_skips.grandfather - the ceiling for #3682.',
        '#',
        '# THE CEILING ONLY EVER FALLS. `--update` MAY LOWER a per-file',
        '# ceiling (to min(existing, measured) — a drain becomes the new',
        '# ceiling on the very next --update) and MAY NEVER RAISE one;',
        '# raising a ceiling stays a deliberate, reviewed hand-edit to this',
        '# file. See tighten_ceiling() / check_debt_floor() in',
        '# check_native_asan_leg_skips.py (same rationale/mechanism as',
        '# scripts/localdecl_only_walkers.grandfather and scripts/imported_'
        'symbol_reads.grandfather, #3590).',
        '#',
        '# Two per-file sections, same shape as the mutable baseline.',
        '',
        '[swallowing_guards]',
    ]
    for path, n in sorted(new_swallow.items()):
        lines.append('%-4d %s' % (n, path))
    lines.append('')
    lines.append('[conflating_helpers]')
    for path, n in sorted(new_conflate.items()):
        lines.append('%-4d %s' % (n, path))
    GRANDFATHER.write_text('\n'.join(lines) + '\n')
    swallow_changed = [(p, existing_swallow.get(p, 0), new_swallow.get(p, 0))
                       for p in sorted(set(existing_swallow) | set(new_swallow))
                       if new_swallow.get(p, 0) < existing_swallow.get(p, 0)]
    conflate_changed = [(p, existing_conflate.get(p, 0), new_conflate.get(p, 0))
                        for p in sorted(set(existing_conflate) | set(new_conflate))
                        if new_conflate.get(p, 0) < existing_conflate.get(p, 0)]
    return (new_swallow, new_conflate), (swallow_changed, conflate_changed)


def compare_one(cur, base, kind, stale=True):
    """One section's gate-vs-update violations."""
    bad = []
    for path in sorted(cur):
        have, want = cur[path], base.get(path)
        if want is None:
            bad.append('NEW FILE %s introduces %d %s site(s) - the family '
                       'may not grow a new site' % (path, have, kind))
        elif have > want:
            bad.append('%s has %d %s site(s), baseline allows %d (+%d)'
                       % (path, have, kind, want, have - want))
    if stale:
        for path in sorted(base):
            have, want = cur.get(path, 0), base[path]
            if want > have:
                bad.append('%s baseline claims %d %s site(s) but the tree '
                           'has %d - STALE HEADROOM (+%d). The baseline is '
                           'exact, not a ceiling: re-run --update on the '
                           'merged tree.' % (path, want, kind, have, want - have))
    return bad


def check_debt_floor(cur, floor, marked, kind):
    """A rise above the grandfathered ceiling needs a per-site
    `// ASAN-LEG-DEBT: #NNNN <reason>` marker in the same file's
    window. `marked` is a {path: N} dict of already-attached marker
    counts (computed by the caller — main() — for the section under
    check)."""
    bad = []
    for path in sorted(cur):
        have = cur[path]
        ceiling = floor.get(path, 0)
        if have <= ceiling:
            continue
        excess = have - ceiling
        got = marked.get(path, 0)
        if got < excess:
            bad.append(
                '%s has %d %s site(s), %d above its grandfathered ceiling '
                'of %d - %d of the %d excess site(s) carry a `// '
                'ASAN-LEG-DEBT: #NNNN <reason>` marker, %d do not. --force '
                'does not substitute for the marker.'
                % (path, have, kind, excess, ceiling, got, excess, excess - got))
    return bad


def malformed_debt_report(malformed, kind):
    return ['%s:%d: ASAN-LEG-DEBT marker has no `#NNNN` right after the '
            'colon - a marker that cannot be checked cannot justify a '
            'rise (write e.g. `// ASAN-LEG-DEBT: #3682 <reason>`)'
            % (path, line) for path, line in sorted(set(malformed))]


# -----------------------------------------------------------------------
# SELF-TEST. Six fixtures per the spec's "What the self-test plants".
# -----------------------------------------------------------------------

FIXTURES_SWALLOW = [
    # (name, body, want_count)
    ('swallow_basic_3456',
     'BEZEL_TEST(t, swallow_basic_3456) {\n'
     '    int a = ::bezel::test::runUnderAsan(src, "t");\n'
     '    if (a == -1) return;\n'
     '    BEZEL_EXPECT_EQ(a, 0);\n'
     '}\n', 1),
    ('not_swallow_3457_explicit_assert',
     'BEZEL_TEST(t, not_swallow_3457_explicit_assert) {\n'
     '    int a = ::bezel::test::runUnderAsan(src, "t");\n'
     '    if (a != -1) BEZEL_EXPECT_EQ(a, 0);\n'
     '}\n', 0),
    ('not_swallow_3458_braced_assert',
     'BEZEL_TEST(t, not_swallow_3458_braced_assert) {\n'
     '    int a = ::bezel::test::runUnderAsan(src, "t");\n'
     '    if (a < 0) { BEZEL_EXPECT_TRUE(false); }\n'
     '    BEZEL_EXPECT_EQ(a, 0);\n'
     '}\n', 0),
    ('only_marker_does_not_count_3459',
     'BEZEL_TEST(t, only_marker_does_not_count_3459) {\n'
     '    int a = ::bezel::test::runUnderAsan(src, "t");\n'
     '    // ASAN-LEG-ONLY: this row genuinely runs without ASan on dev boxes\n'
     '    if (a == -1) return;\n'
     '    BEZEL_EXPECT_EQ(a, 0);\n'
     '}\n', 0),
    ('marker_no_reason_does_not_exempt_3460',
     'BEZEL_TEST(t, marker_no_reason_does_not_exempt_3460) {\n'
     '    int a = ::bezel::test::runUnderAsan(src, "t");\n'
     '    // ASAN-LEG-ONLY:\n'
     '    if (a == -1) return;\n'
     '    BEZEL_EXPECT_EQ(a, 0);\n'
     '}\n', 1),
    ('multiple_swallow_sites_3461',
     'BEZEL_TEST(t, multiple_swallow_sites_3461) {\n'
     '    int a = ::bezel::test::runUnderAsan(src, "t");\n'
     '    int b = ::bezel::test::runUnderAsan(src2, "u");\n'
     '    if (a == -1) return;\n'
     '    if (b == -1) return;\n'
     '    BEZEL_EXPECT_EQ(a, 0);\n'
     '    BEZEL_EXPECT_EQ(b, 0);\n'
     '}\n', 2),
    ('guard_on_unbound_var_3462',
     'BEZEL_TEST(t, guard_on_unbound_var_3462) {\n'
     '    int x = 7;\n'
     '    if (x == -1) return;  // bound to nothing ASan-related — not a swallow\n'
     '}\n', 0),
    ('lt_zero_swallow_3463',
     'BEZEL_TEST(t, lt_zero_swallow_3463) {\n'
     '    int rc = ::bezel::test::asanRuntimeObject().empty() ? -1 : 0;\n'
     '    if (rc < 0) return;\n'
     '}\n', 1),
]

FIXTURES_CONFLATE = [
    # (name, body, want_count_for_function_body)
    ('conflate_basic_3470',
     'int runUnderAsanA(const std::string& src) {\n'
     '    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();\n'
     '    if (rtObj.empty()) return -1;\n'
     '    std::string linkCmd = "clang++ -fsanitize=address -g ";\n'
     '    if (std::system(linkCmd.c_str()) != 0) { cleanup(); return -1; }\n'  # bug
     '    return 0;\n'
     '}\n', 1),
    ('not_conflate_pre_link_3474',
     'int runUnderAsanB(const std::string& src) {\n'
     '    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();\n'
     '    if (rtObj.empty()) return -1;\n'  # pre-link: toolchain unavailable
     '    std::string linkCmd = "clang++ -fsanitize=address -g ";\n'
     '    if (std::system(linkCmd.c_str()) != 0) { cleanup(); return -3; }\n'  # fixed
     '    return 0;\n'
     '}\n', 0),
    ('not_conflate_no_link_3475',
     'int notAsan() {\n'
     '    if (rtObj.empty()) return -1;\n'
     '    return 0;\n'
     '}\n', 0),
    ('conflate_prose_comment_3476',
     'int runUnderAsanC(const std::string& src) {\n'
     '    // the helper returns -1 on link failure - no wait it should be -3\n'
     '    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();\n'
     '    if (rtObj.empty()) return -1;\n'
     '    std::string linkCmd = "clang++ -fsanitize=address -g ";\n'
     '    if (std::system(linkCmd.c_str()) != 0) { cleanup(); return -3; }\n'  # fixed
     '    return 0;\n'
     '}\n', 0),  # prose -1 not in code
    # #3676 / spec §2 SEED: the runUnderASan3181 shape — an inner non-function
    # `{ bezel::DiagnosticEngine diags; ... }` block followed by a
    # `-fsanitize=address` link command and a post-link `return -1;`. Pre-fix
    # `build_function_index` truncated the body at the inner block's `}`, so the
    # link string was outside the recorded body and the conflating helper was
    # structurally invisible to the ratchet (the spec's named seed). Post-fix
    # the body extends to the function's actual closing `}`, so the link
    # string + post-link `return -1;` are inside the body and the ratchet
    # reports 1. If this fixture ever reports 0, the brace tracking has
    # regressed to the buggy shape and the spec's seed is again invisible.
    ('conflate_inner_block_seed_3682',
     'int runUnderASan3181_shaped(const std::string& source) {\n'
     '    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();\n'
     '    if (rtObj.empty()) return -1;\n'
     '    auto dir = wtDir3181();\n'
     '    std::string stem = unique_stem("bezel_3181_asan_");\n'
     '    std::string objPath = (dir / (stem + ".o")).string();\n'
     '    std::string exe     = (dir / stem).string();\n'
     '    {\n'
     '        bezel::DiagnosticEngine diags;\n'
     '        bezel::Lexer lexer(source, diags);\n'
     '        if (!lexer.ok()) return -2;\n'
     '    }\n'
     '    std::string linkCmd = "clang++ -fsanitize=address -g " + objPath;\n'
     '    if (std::system(linkCmd.c_str()) != 0) {\n'
     '        std::remove(objPath.c_str());\n'
     '        std::remove(exe.c_str());\n'
     '        return -1;\n'  # post-link return -1 — the #3676 bug shape
     '    }\n'
     '    return 0;\n'
     '}\n', 1),
    # Sibling: same shape, post-link returns -3 (the #3676 fix). Must NOT be
    # flagged as a conflation helper even with the brace fix — the inner block
    # is correctly nested now, but the post-link `return -3;` is the fix.
    ('not_conflate_inner_block_post_fix_3682',
     'int runUnderASan3181_post_fix(const std::string& source) {\n'
     '    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();\n'
     '    if (rtObj.empty()) return -1;\n'
     '    {\n'
     '        bezel::DiagnosticEngine diags;\n'
     '        if (!diags.ok()) return -2;\n'
     '    }\n'
     '    std::string linkCmd = "clang++ -fsanitize=address -g " + objPath;\n'
     '    if (std::system(linkCmd.c_str()) != 0) { return -3; }\n'  # post-fix
     '    return 0;\n'
     '}\n', 0),
    # Bug 2 (ratchet-3682 detector, post-link return -1 pre-link check):
    # a helper that composes a `linkCmd = "clang++ -fsanitize=address -g ..."`
    # string AND has a `return -1;` AFTER the link command but is NOT an
    # ASan-leg helper — no pre-link toolchain-unavailable check, so the
    # post-link `return -1;` is for an unrelated purpose. The pre-fix
    # detector matched it (false positive). The Bug 2 fix requires a
    # pre-link `return -1;` (the canonical toolchain-unavailable check),
    # so this is excluded.
    ('not_conflate_post_link_only_unrelated_4150',
     'int buildLinkDoc(const std::string& objPath) {\n'
     '    std::string linkCmd = "clang++ -fsanitize=address -g " + objPath;\n'
     '    // No pre-link toolchain check — this is a doc helper.\n'
     '    if (some_unrelated_check_failed) return -1;\n'  # post-link, unrelated
     '    return 0;\n'
     '}\n', 0),
    # Bug 2: the canonical runUnderAsan helper. The function is an ASan-leg
    # helper (canonical `if (rtObj.empty()) return -1;` pre-link check, then
    # a `-fsanitize=address` link command, then a post-link `return -1;`).
    # Still counts (1) after the Bug 2 fix — the pre-link check is present.
    ('conflate_canonical_runUnderAsan_4151',
     'int runUnderAsan(const std::string& source, const char* tag) {\n'
     '    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();\n'
     '    if (rtObj.empty()) return -1;\n'  # pre-link toolchain check
     '    std::string linkCmd = "clang++ -fsanitize=address -g ";\n'
     '    if (std::system(linkCmd.c_str()) != 0) { return -1; }\n'  # post-link bug
     '    return 0;\n'
     '}\n', 1),
]


def self_test():
    ok = True

    def chk(label, cond, *details):
        """`label` is pre-formatted with %-interpolation done by the
        caller. `cond` is the assertion under test. `details` is
        concatenated into the FAIL line so a failing check shows the
        expected/actual values inline."""
        nonlocal ok
        if not cond:
            ok = False
            print('  FAIL: %s %s' % (label, ' '.join(str(d) for d in details)))
        else:
            print('  ok:   %s' % label)

    # --- swallow fixtures (apply markers per-fixture so the count is the
    # "what gets banked" count, not the raw scanner output) ---
    for name, body, want in FIXTURES_SWALLOW:
        sites = scan_swallowing_guards(body, name)
        exempt = exempt_lines_for_only(body)
        kept = attach_markers(sites, exempt)
        got = len(kept)
        chk('swallow/%s -> got=%d want=%d' % (name, got, want), got == want)

    # --- conflate fixtures (no marker exemption) ---
    for name, body, want in FIXTURES_CONFLATE:
        got = len(scan_conflating_helpers(body, name))
        chk('conflate/%s -> got=%d want=%d' % (name, got, want), got == want)

    # --- ASAN-LEG-ONLY exempts the next guard within the window ---
    raw = ('BEZEL_TEST(t, marker_test) {\n'
           '    int a = ::bezel::test::runUnderAsan(src, "t");\n'
           '    // ASAN-LEG-ONLY: dev-box skip is intended here\n'
           '    if (a == -1) return;\n'
           '    BEZEL_EXPECT_EQ(a, 0);\n'
           '}\n')
    sites = scan_swallowing_guards(raw, 't.cpp')
    exempt = exempt_lines_for_only(raw)
    kept = attach_markers(sites, exempt)
    chk('ASAN-LEG-ONLY exempts the next guard (kept=%d want=0)'
        % len(kept), len(kept) == 0)

    # --- ASAN-LEG-DEBT with malformed marker is reported ---
    raw_bad = ('BEZEL_TEST(t, bad_marker) {\n'
               '    int a = ::bezel::test::runUnderAsan(src, "t");\n'
               '    // ASAN-LEG-DEBT: needs fixing eventually\n'
               '    if (a == -1) return;\n'
               '}\n')
    _, malformed = debt_lines('bad.cpp', raw_bad)
    chk('malformed ASAN-LEG-DEBT reported (n=%d want=1)'
        % len(malformed), len(malformed) == 1)

    # --- ASAN-LEG-DEBT with #NNNN attaches to the site ---
    raw_good = ('BEZEL_TEST(t, good_marker) {\n'
                '    int a = ::bezel::test::runUnderAsan(src, "t");\n'
                '    // ASAN-LEG-DEBT: #3682 tracked\n'
                '    if (a == -1) return;\n'
                '}\n')
    debt_markers, _ = debt_lines('good.cpp', raw_good)
    sites = scan_swallowing_guards(raw_good, 'good.cpp')
    debt = attach_debt_markers(sites, debt_markers)
    chk('ASAN-LEG-DEBT attaches to next site (n=%d want=1)'
        % len(debt), len(debt) == 1)

    # --- compare: rise / stale / new file ---
    bad_rise = compare_one({'a.cpp': 4}, {'a.cpp': 3}, 'swallow', stale=True)
    bad_stale = compare_one({'a.cpp': 2}, {'a.cpp': 3}, 'swallow', stale=True)
    bad_new = compare_one({'b.cpp': 1}, {'a.cpp': 3}, 'swallow', stale=True)
    chk('compare detects rise (n=%d want=1)' % len(bad_rise), len(bad_rise) == 1)
    chk('compare detects stale (n=%d want=1)' % len(bad_stale), len(bad_stale) == 1)
    chk('compare detects new file (n=%d want=2)' % len(bad_new), len(bad_new) == 2)

    # --- tighten_ceiling: monotone downward ---
    chk('tighten: drain lowers ceiling (28->20)',
        tighten_ceiling({'a.cpp': 28}, {'a.cpp': 20}) == {'a.cpp': 20})
    chk('tighten: cannot rise above grandfathered ceiling',
        tighten_ceiling({'a.cpp': 25}, {'a.cpp': 20}) == {'a.cpp': 20})

    # --- check_debt_floor: rise above ceiling without marker is a violation ---
    bad = check_debt_floor({'a.cpp': 4}, {'a.cpp': 3}, {}, 'swallow')
    chk('debt-floor detects unmarked rise (n=%d want=1)' % len(bad), len(bad) == 1)
    bad_marked = check_debt_floor({'a.cpp': 4}, {'a.cpp': 3}, {'a.cpp': 1},
                                  'swallow')
    chk('debt-floor accepts marked rise (n=%d want=0)'
        % len(bad_marked), len(bad_marked) == 0)

    # --- missing baseline fails closed ---
    with tempfile.TemporaryDirectory() as td:
        # No baseline file in this tree.
        root = pathlib.Path(td)
        swallow, conflate, _ = scan(root)
        chk('scan of empty tree returns 0 swallow sites', len(swallow) == 0)
        chk('scan of empty tree returns 0 conflate sites', len(conflate) == 0)

    # --- scope (Bug 1 fix, ratchet-3682 detector): stray src/ + scripts/
    # files outside tests/ MUST NOT be counted, even when they contain
    # `-fsanitize=address` + post-link `return -1;`. Pre-fix the detector
    # did `root.rglob('*')` and walked into src/, docs/, scripts/,
    # build/, AND into any nested worktree under .claude/worktrees/ —
    # the latter alone inflated 73 -> 516 on r18-c7-coord (#3682
    # over-count follow-up). The post-fix `scan()` walks only `tests/`.
    with tempfile.TemporaryDirectory() as td:
        root = pathlib.Path(td)
        (root / 'tests').mkdir()
        (root / 'src').mkdir()
        (root / 'scripts').mkdir()
        # Stray src/ file with the bug pattern — MUST NOT count.
        (root / 'src' / 'stray_src.cpp').write_text(
            'int runUnderAsanXXX() {\n'
            '    std::string linkCmd = "clang++ -fsanitize=address -g /tmp/x.o";\n'
            '    if (std::system(linkCmd.c_str()) != 0) { return -1; }\n'
            '    return 0;\n'
            '}\n')
        # Stray scripts/ file with the bug pattern — MUST NOT count.
        (root / 'scripts' / 'stray_script.cpp').write_text(
            'int helper() {\n'
            '    std::string cmd = "clang++ -fsanitize=address -g";\n'
            '    if (some_check) return -1;\n'
            '    return 0;\n'
            '}\n')
        # Real conflation in tests/ — MUST count.
        (root / 'tests' / 'real.cpp').write_text(
            'int runUnderLeakSanitizer(const std::string& source) {\n'
            '    const std::string& rtObj = '
            '::bezel::native_test::asanRuntimeObject();\n'
            '    if (rtObj.empty()) return -1;\n'
            '    std::string linkCmd = "clang++ -fsanitize=address -g /tmp/x.o";\n'
            '    if (std::system(linkCmd.c_str()) != 0) { return -1; }\n'
            '    return 0;\n'
            '}\n')
        swallow, conflate, _ = scan(root)
        chk('scope: stray src/ helper NOT counted (got=%d want=0)'
            % len([s for s in conflate if 'src/' in s[0]]),
            len([s for s in conflate if 'src/' in s[0]]) == 0)
        chk('scope: stray scripts/ helper NOT counted (got=%d want=0)'
            % len([s for s in conflate if 'scripts/' in s[0]]),
            len([s for s in conflate if 'scripts/' in s[0]]) == 0)
        chk('scope: real tests/ helper IS counted (got=%d want=1)'
            % len([s for s in conflate if 'tests/' in s[0]]),
            len([s for s in conflate if 'tests/' in s[0]]) == 1)

    # --- scope: a nested-worktree `.claude/worktrees/<x>/tests/...` tree
    # is NOT scanned (it belongs to a different worktree, not the current
    # tree's `tests/`). Pre-fix `root.rglob('*')` walked into it; that's
    # how the detector reported 516 on r18-c7-coord (where 5 sibling
    # worktrees each carry their own copy of the test files).
    with tempfile.TemporaryDirectory() as td:
        root = pathlib.Path(td)
        (root / 'tests').mkdir()
        (root / '.claude' / 'worktrees' / 'nested').mkdir(parents=True)
        (root / '.claude' / 'worktrees' / 'nested' / 'tests').mkdir(parents=True)
        # Empty tests/ in current tree — would report 0.
        (root / 'tests' / 'empty.cpp').write_text('// nothing here\n')
        # A conflation in the nested worktree's tests/ — MUST NOT count.
        (root / '.claude' / 'worktrees' / 'nested' / 'tests' / 'nested.cpp'
         ).write_text(
            'int runUnderLeakSanitizer(const std::string& source) {\n'
            '    const std::string& rtObj = '
            '::bezel::native_test::asanRuntimeObject();\n'
            '    if (rtObj.empty()) return -1;\n'
            '    std::string linkCmd = "clang++ -fsanitize=address -g /tmp/x.o";\n'
            '    if (std::system(linkCmd.c_str()) != 0) { return -1; }\n'
            '    return 0;\n'
            '}\n')
        swallow, conflate, _ = scan(root)
        nested_count = len([s for s in conflate if '.claude/' in s[0]])
        chk('scope: nested-worktree tests/ NOT scanned (got=%d want=0)'
            % nested_count, nested_count == 0)

    # --- whole-tree sanity: the real tree MUST show non-zero counts for
    # the families we expect — a matcher that matched nothing is what a
    # false green looks like (#3460 family). 49 swallowing-guard sites is
    # the measured number; 0 conflating_helpers is the spec's seed value
    # MINUS the #3676 fix in commit 1, so a 0 there is correct, not a
    # false green.
    real_swallow, real_conflate, _ = scan(ROOT)
    if not real_swallow:
        ok = False
        print('  FAIL: whole-tree scan found 0 swallowing_guards - a matcher '
              'that matched nothing is what a false green looks like')
    else:
        print('  ok:   whole-tree scan found %d swallowing_guards site(s) '
              'in %d file(s)' % (sum(count_per_file(real_swallow).values()),
                                  len(count_per_file(real_swallow))))
    print('  ok:   whole-tree scan found %d conflating_helpers site(s) '
          'in %d file(s) (spec seed 1, post-#3676 fix expected 0)'
          % (sum(count_per_file(real_conflate).values()),
              len(count_per_file(real_conflate))))

    print('check_native_asan_leg_skips --self-test: ' + ('PASS' if ok else 'FAIL'))
    return 0 if ok else 1


# -----------------------------------------------------------------------
# main(): gate mode / self-test / --update / --list.
# -----------------------------------------------------------------------


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--strict', action='store_true',
                    help='exit 1 on any violation (the gate phase)')
    ap.add_argument('--self-test', action='store_true')
    ap.add_argument('--update', action='store_true',
                    help='rewrite the baseline from the current tree')
    ap.add_argument('--force', action='store_true',
                    help='with --update, permit a count to RISE')
    ap.add_argument('--list', action='store_true',
                    help='print every site (swallow + conflate)')
    a = ap.parse_args()

    if a.self_test:
        return self_test()

    # Walk every test source once; collect everything we need.
    swallow_sites, conflate_sites, malformed = scan(ROOT)

    # Per-file marker data — built once, used twice (swallow filter,
    # debt-marker per-site attachment). SCOPE mirrors `scan()` above:
    # only `tests/` is in scope. The pre-fix walk was whole-tree.
    file_exempt_lines = {}     # file -> set of lines covered by ASAN-LEG-ONLY
    debt_triples = []          # [(file, line, issue)]
    all_malformed = list(malformed)
    marker_scan_dir = ROOT / 'tests'
    if marker_scan_dir.is_dir():
        for p in marker_scan_dir.rglob('*'):
            if not p.is_file() or p.suffix not in SUFFIXES:
                continue
            rel = str(p.relative_to(ROOT))
            raw = p.read_text()
            if not _is_exempt(rel):
                file_exempt_lines[rel] = exempt_lines_for_only(raw)
            well, mal = debt_lines(rel, raw)
            debt_triples.extend(well)
            for ln in mal:
                all_malformed.append((rel, ln))

    # Apply ASAN-LEG-ONLY markers to swallow sites — drop exempted lines.
    swallow_filtered = []
    for s in swallow_sites:
        exempt = file_exempt_lines.get(s[0], set())
        if s[1] in exempt:
            continue
        swallow_filtered.append(s)

    # Per-site debt attachment (used for the grandfather ceiling check).
    debt_marked = attach_debt_markers(swallow_filtered, debt_triples)
    debt_marked_paths_swallow = {}
    for s, _issue in debt_marked.items():
        debt_marked_paths_swallow[s[0]] = \
            debt_marked_paths_swallow.get(s[0], 0) + 1

    # For conflating helpers the debt marker applies at FILE granularity:
    # the function body is small (single ASan run), so any marker in the
    # file's neighbourhood (within MARKER_WINDOW) covers it.
    debt_marked_paths_conflate = {}
    for path, line, _issue in debt_triples:
        if any(s[0] == path and abs(s[1] - line) <= MARKER_WINDOW
               for s in conflate_sites):
            debt_marked_paths_conflate[path] = \
                debt_marked_paths_conflate.get(path, 0) + 1

    cur_swallow = count_per_file(swallow_filtered)
    cur_conflate = count_per_file(conflate_sites)
    base_swallow, base_conflate = read_baseline()
    floor_swallow, floor_conflate = read_grandfather()

    if a.list:
        print('=== swallowing_guards (%d) ===' % len(swallow_filtered))
        for s in sorted(swallow_filtered):
            print('%s:%d  %s.%s  var=%s' % (s[0], s[1], s[2], '', s[3]))
        print('=== conflating_helpers (%d) ===' % len(conflate_sites))
        for s in sorted(conflate_sites):
            print('%s:%d  fn=%s' % (s[0], s[1], s[2]))
        print('=== malformed ASAN-LEG-DEBT markers (%d) ==='
              % len(all_malformed))
        for p, ln in sorted(set(all_malformed)):
            print('%s:%d' % (p, ln))
        return 0

    # Build the per-section violation lists.
    swallow_bad = (compare_one(cur_swallow, base_swallow, 'swallowing_guards')
                   + check_debt_floor(cur_swallow, floor_swallow,
                                       debt_marked_paths_swallow,
                                       'swallowing_guards')
                   + malformed_debt_report(all_malformed, 'swallowing_guards'))
    conflate_bad = (compare_one(cur_conflate, base_conflate,
                                'conflating_helpers')
                    + check_debt_floor(cur_conflate, floor_conflate,
                                       debt_marked_paths_conflate,
                                       'conflating_helpers'))

    if a.update:
        # Refuse to raise — both sections.
        swallow_rises = compare_one(cur_swallow, base_swallow,
                                    'swallowing_guards', stale=False)
        conflate_rises = compare_one(cur_conflate, base_conflate,
                                     'conflating_helpers', stale=False)
        if (swallow_rises or conflate_rises) and not a.force:
            print('REFUSING to update: the baseline may only go DOWN.')
            for v in swallow_rises:
                print('  ' + v)
            for v in conflate_rises:
                print('  ' + v)
            print('Use --force if the rise is deliberate, and say why in '
                  'the commit.')
            return 1
        write_baseline(cur_swallow, cur_conflate)
        (new_swallow, new_conflate), (s_changed, c_changed) = write_grandfather(
            floor_swallow, floor_conflate, cur_swallow, cur_conflate)
        s_total = sum(cur_swallow.values())
        c_total = sum(cur_conflate.values())
        print('baseline updated: %d swallowing_guards in %d file(s); '
              '%d conflating_helpers in %d file(s)'
              % (s_total, len(cur_swallow), c_total, len(cur_conflate)))
        if s_changed or c_changed:
            print('grandfather ceiling tightened:')
            for p, old, new in s_changed:
                print('  [swallow] %s: %d -> %d' % (p, old, new))
            for p, old, new in c_changed:
                print('  [conflate] %s: %d -> %d' % (p, old, new))
        return 0

    bad = swallow_bad + conflate_bad
    if bad:
        print('NATIVE ASAN LEG SKIPS: %d violation(s) (#3682)' % len(bad))
        for v in bad:
            print('  ' + v)
        print('\nA swallowing guard is `if (<asan-var> == -1) return;` '
              '(or `< 0`) — a missing ASan runtime reads as a SKIP and the '
              'row reports a clean PASS while the leg is off. A conflation '
              'helper returns -1 on a -fsanitize=address link failure '
              '(should be -3). See scripts/check_native_asan_leg_skips.py '
              'and docs/proposals/oracle-ratchets-2026-09.md §2 / '
              '#3682.')
        return 1 if a.strict else 0

    s_total = sum(cur_swallow.values())
    c_total = sum(cur_conflate.values())
    print('check_native_asan_leg_skips: clean - %d swallowing_guards in '
          '%d file(s) (baseline %d); %d conflating_helpers in %d file(s) '
          '(baseline %d)'
          % (s_total, len(cur_swallow), sum(base_swallow.values()),
             c_total, len(cur_conflate), sum(base_conflate.values())))
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except BaselineConflict as e:
        print('NATIVE ASAN LEG SKIPS: %s' % e)
        sys.exit(1)