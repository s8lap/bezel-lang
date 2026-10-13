#!/usr/bin/env python3
"""check_localdecl_only_walkers.py - ratchet the LocalDecl-only walker family.

WHY (#3591, census hotspot 8). Sec21.1's runtime `const` local is
`ast::LocalConstDecl` - a DIFFERENT AST node from `ast::LocalDecl` (the `var`
form), with differently-named fields (`value` vs `initializer`, etc; both
derive directly from `ast::Stmt`, with no shared intermediate base). A
statement walker spelled

    dynamic_cast<const ast::LocalDecl*>(s)

with no sibling arm that also considers `LocalConstDecl` silently treats every
`const` local as if it does not exist - not a null-deref, a silent no-op for a
whole binding form. Whatever the walker exists to build (escape analysis,
spill/frame bookkeeping, string/collection/closure-reclaim eligibility, ...)
simply never fires for a `const` local.

Six prior instances of exactly this shape were found and fixed ONE AT A TIME,
each its own bug report, each fixing only its own consumer, symptoms ranging
from a 420 B leak to a use-after-free: #2802, #3375, #3402, #3416, #3417,
#3427. The canonical fix shape is recorded in-source at
LlvmBackend_Setup.cpp:24776 (the #2802 fix comment) - widen the candidate
vector from `const ast::LocalDecl*` to `const ast::Node*`; the consumers
compare by `const void*` pointer identity, so the widening is safe by
construction. A second site (:23682-23689) self-counts as "the FIFTH instance
of that LocalDecl-only pattern" - the pattern is well understood and
repeatedly REDISCOVERED, which is exactly what a ratchet is for: nothing
currently stops a SEVENTH, EIGHTH, ... instance from being written tomorrow.

WHAT THIS SCRIPT IS FOR. Unlike its three siblings (each retiring one family
of duplicate call sites onto a shared authority), this family has no single
shared authority every site could call instead - `Emitter::localDeclInitView`
(LlvmBackend_impl.hpp) is the ONE accessor that exists, and only a handful of
codegen sites use it. Most of the 76+ bare sites are small, ad-hoc,
special-purpose walkers (ownership dataflow, escape analysis, ...) that will
each need their own individual fix, exactly like the six before them. So this
ratchet does not point at one shared function the way the other three do; it
freezes the COUNT of unreviewed bare sites so the next one added by ordinary
feature work fails the gate immediately, instead of shipping and waiting to be
independently rediscovered as leak/UAF #7.

CLASSIFICATION: SCOPE-BASED, NOT A LINE WINDOW. The census's own recipe (grep
+ a fixed +/-12-line window) is explicitly labelled an UPPER BOUND, not a
defect count - a bare site can be perfectly correct if the enclosing function
handles `const` locals elsewhere, arbitrarily far away in the same function
body, or if the enclosing WALKER (often a `scan`/`visit` recursive lambda, the
dominant shape in this codebase - see LlvmBackend_Setup.cpp's `scan` lambdas)
is a small, self-contained scope where a `const` local structurally cannot
reach at all. A fixed line window both over- and under-counts: it can bleed
into an unrelated ADJACENT function, and it can miss a sibling arm that is
correct but more than 12 lines away in a long function.

This script instead builds a brace-depth SCOPE INDEX per file (`_looks_like_
function_or_lambda_header` classifies each `{` as a function/lambda body root
or a mere control-flow/aggregate block) and, for each `dynamic_cast<[const]
ast::LocalDecl*>` site, finds its SMALLEST enclosing function-or-lambda-body
scope. The site is "bare" iff no `LocalConstDecl` token appears anywhere in
that scope (measured identical, on this tree, to the stricter test of an
actual `dynamic_cast<... ast::LocalConstDecl ...>` in scope - see the
docstring of `scan_text` for the two real-tree cases that motivate the
distinction anyway). A cast inside a nested `scan`/lambda is scoped to THAT
LAMBDA, not its enclosing function - deliberately: the real bugs are walkers
that are themselves small self-contained recursive closures, and the
enclosing function doing something unrelated to `const` elsewhere does not
make the lambda's own omission safe.

⚠ THIS IS STILL AN UPPER BOUND, NOT A DEFECT COUNT. A bare site can be
correct-by-context (§21.1 forbids `const` from ever being an ASSIGNMENT
TARGET, so a walker that only fires on assignment targets - e.g. tracking
REASSIGNED locals - cannot see a `const` there structurally, even though nothing
in ITS OWN SCOPE mentions `LocalConstDecl`). Each bare site still needs
individual triage before it is treated as a defect, exactly as the six prior
instances were each individually confirmed with a measured leak/UAF before
fixing. The `LOCALDECL-ONLY:` marker below is how a genuinely-correct site is
recorded, reviewably, instead of silently shrinking the count.

WHEN A FAMILY LANDS. Counts go down; re-baseline with `--update`, which
REFUSES to raise a count. Raising one is a deliberate act that needs `--force`.

⚠ `--force` ALONE IS NOT ENOUGH (ported from #3590, the identical mechanism in
check_imported_symbol_reads.py / check_enumerable_grid_spellings.py - built
against the POST-#3590 versions of both, not the pre-#3590 ones, so this
ratchet starts with the same both-directions guarantee its siblings only
earned after a review round). A rise past what the tree carries when this
ratchet lands needs a per-site, CHECKABLE marker:

    // LOCALDECL-DEBT: #NNNN <reason>

placed like `LOCALDECL-ONLY` (attaches to the next cast line within 8 lines).
Unlike `LOCALDECL-ONLY`, `LOCALDECL-DEBT` does NOT remove the site from the
count - it is still an offender, still baselined, still meant to be fixed. It
only justifies a rise ABOVE `scripts/localdecl_only_walkers.grandfather`, the
per-file ceiling recording the count that already existed when this ratchet
landed. The tree's existing bare sites need no marker - they sit at or below
their file's ceiling. Coverage is recomputed from the LIVE TREE on every
`--strict` run, not just at the moment `--force` was typed.

⚠ THE CEILING ONLY EVER FALLS. A file's ceiling is not frozen forever at
launch - EVERY successful `--update` TIGHTENS it to `min(existing, measured)`,
per file, monotonically downward, so a drain becomes the new ceiling
immediately and a later lane cannot silently re-grow a drained file back
toward its old high-water mark with a bare `--force`. Raising a ceiling stays
a deliberate, reviewed hand-edit to the `.grandfather` file.

PER-SITE EXEMPTION. A site where a `const` local is structurally unreachable
(e.g. an assignment TARGET walk, or a loop-INDUCTION-variable-only context -
neither can ever be `const`, by grammar/§21.1) writes the marker at the site,
with a reason:

    // LOCALDECL-ONLY: <why a const local cannot reach here>

A marker with no reason after the colon does NOT exempt. Each marker exempts
exactly the next cast line within eight lines.

⚠ CONTROL-RUN BEFORE TRUSTING A GREEN. `--self-test` runs FIRST in the gate
phase and is not optional. `scripts/check_vacuous_tests.py`'s first draft
reported the tree clean because its regex matched zero tests; a green from a
matcher that matched nothing is worth nothing. The fixtures below include the
non-const spelling, the multi-line spelling, and - the case that matters most
for THIS detector specifically - a fixture proving the scope index does not
leak a sibling arm across an unrelated function boundary.

Usage:  scripts/check_localdecl_only_walkers.py [--strict] [--self-test]
                                                [--update [--force]] [--list]
"""
import argparse, pathlib, re, sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
BASELINE = ROOT / 'scripts' / 'localdecl_only_walkers.baseline'
GRANDFATHER = ROOT / 'scripts' / 'localdecl_only_walkers.grandfather'
SCAN_DIRS = ('src',)
SUFFIXES = ('.cpp', '.hpp')

# The site: a cast to `ast::LocalDecl*`, const-qualified or not (one bare
# non-const instance is known: ScopedAwaitBind.cpp). Whitespace-tolerant
# across newlines so the common wrapped-call spelling is still one match.
CAST = re.compile(r'dynamic_cast\s*<\s*(?:const\s+)?ast::LocalDecl\s*\*\s*>\s*\(', re.S)
# The "is this scope covered" signal. A real `LocalConstDecl` cast, not merely
# the identifier - see scan_text's docstring for why the two are equivalent on
# this tree today (both give 58 bare / 45 covered) and why the stricter one is
# used as the definition rather than the coincidence.
CONST_DECL_CAST = re.compile(
    r'dynamic_cast\s*<\s*(?:const\s+)?ast::LocalConstDecl\s*\*\s*>\s*\(', re.S)

MARKER = re.compile(r'//\s*LOCALDECL-ONLY:\s*\S')
# LOCALDECL-DEBT: a SIBLING marker, not a replacement - see check_debt_floor.
# LOCALDECL-ONLY says "this site IS correct" and removes it from the count;
# LOCALDECL-DEBT says "this is a known offender, deliberately left past its
# file's grandfathered ceiling" and does NOT remove it. The `#NNNN` must sit
# immediately after the colon; a marker with a reason but no issue number is
# malformed and justifies nothing.
DEBT = re.compile(r'//\s*LOCALDECL-DEBT:\s*(\S.*)$')
DEBT_ISSUE = re.compile(r'^#(\d+)\b')


def strip_comments(text):
    """Blank out // and /* */ comments, preserving length and newlines.

    A prose MENTION of the cast (or of `LocalConstDecl`) is not a cast or a
    handling arm. LlvmBackend_Setup.cpp:23682-89's own "FIFTH instance" note
    and this file's docstring both quote the pattern verbatim; counting those
    would make the ratchet fire on documentation, which trains readers to
    route around it. It is also why `LOCALDECL-ONLY`/`LOCALDECL-DEBT` markers
    are read from the RAW text (exempt_lines / debt_lines below), not this
    stripped copy - a marker IS a comment, deliberately.
    """
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
        elif c in '"\'':
            q, j = c, i + 1
            while j < n and text[j] != q:
                j += 2 if text[j] == '\\' else 1
            j = min(j + 1, n)
            out.append(text[i:j]); i = j
        else:
            out.append(c); i += 1
    return ''.join(out)


# ---------------------------------------------------------------------------
# SCOPE INDEX. Brace-depth scan (over the comment-stripped text) that finds,
# for every `{`...`}` block, whether it is a FUNCTION-OR-LAMBDA-BODY root
# (where a walker's per-scope `const`-handling would live) or a mere
# control-flow / aggregate-init / class-namespace block. `;` only resets the
# "header" boundary at paren-depth 0, so a for-loop's internal `;`s do not
# fragment its own header.
# ---------------------------------------------------------------------------
_CONTROL_HEADER = re.compile(r'^(else\s+)?(if|for|while|switch|catch)\s*\(', re.S)
_SCOPE_KEYWORD_HEADER = re.compile(
    r'^(namespace|class|struct|enum(\s+class)?|union)\b')


def _is_function_or_lambda_header(header):
    """True if the text immediately before a `{` looks like it opens a
    function or lambda BODY (where a walker's own const-handling would live),
    false for a control-flow block, bare/else/do/try block, or a class/struct/
    namespace/enum/union body.

    Misclassifying an OUTER container (struct/namespace) as root is harmless:
    the smallest-enclosing-root search (see `enclosing_root_scope`) still
    finds the innermost TRUE function/lambda body nested inside it, because
    that body gets its own, smaller, correctly-classified frame. The one
    misclassification that matters is a control-flow block read as root -
    that would wrongly shrink the search window to just an `if`/`for` body
    and could false-positive a site whose sibling arm is a few lines outside
    it in the same function - so those are the ones enumerated explicitly.
    """
    h = header.strip()
    if not h:
        return False  # bare `{ ... }` scope block
    if h in ('do', 'else', 'try'):
        return False
    if _CONTROL_HEADER.match(h):
        return False
    if _SCOPE_KEYWORD_HEADER.match(h):
        return False
    return True


def build_scope_index(text):
    """-> [(open_pos, close_pos, is_root), ...], properly nested via a stack
    over comment-stripped `text`. `open_pos`/`close_pos` are the offsets of
    the `{` and matching `}`.
    """
    stack, blocks = [], []
    i, n = 0, len(text)
    last_boundary = 0
    paren_depth = 0
    while i < n:
        c = text[i]
        if c == '(':
            paren_depth += 1
        elif c == ')':
            if paren_depth > 0:
                paren_depth -= 1
        elif c == '{':
            is_root = _is_function_or_lambda_header(text[last_boundary:i])
            stack.append((i, is_root))
            last_boundary = i + 1
            paren_depth = 0
        elif c == '}':
            if stack:
                open_pos, is_root = stack.pop()
                blocks.append((open_pos, i, is_root))
            last_boundary = i + 1
            paren_depth = 0
        elif c == ';' and paren_depth == 0:
            last_boundary = i + 1
        i += 1
    return blocks


def enclosing_root_scope(blocks, pos):
    """-> (start, end) of the SMALLEST root (function-or-lambda-body) block
    containing `pos`, or None if `pos` is not inside any root block (should
    not happen for a real cast site - every one is inside some function)."""
    candidates = [(s, e) for s, e, root in blocks if root and s < pos < e]
    if not candidates:
        return None
    return min(candidates, key=lambda p: p[1] - p[0])


def exempt_lines(raw):
    """1-indexed line numbers exempted by a LOCALDECL-ONLY marker (own line
    plus the next non-blank/continuation line, bounded to 8 lines so a marker
    cannot drift away from the cast it describes)."""
    out, lines = set(), raw.split('\n')
    for i, ln in enumerate(lines):
        if not MARKER.search(ln):
            continue
        out.add(i + 1)
        for j in range(i + 1, min(i + 8, len(lines))):
            s = lines[j].strip()
            out.add(j + 1)
            if s and not s.startswith('//'):
                break
    return out


def debt_lines(raw):
    """[(line, issue)] for WELL-FORMED LOCALDECL-DEBT markers, and separately
    the malformed ones (a reason with no leading `#NNNN`, which justifies
    nothing)."""
    well, malformed = [], []
    for i, ln in enumerate(raw.split('\n')):
        m = DEBT.search(ln)
        if not m:
            continue
        im = DEBT_ISSUE.match(m.group(1).strip())
        if im:
            well.append((i + 1, int(im.group(1))))
        else:
            malformed.append(i + 1)
    return well, malformed


def _assign_next_cast_line(markers, cast_lines):
    """{marker_line: payload} -> {cast_line: payload}, attaching each marker
    to the next unused cast line at or after it, within 8 lines. Shared shape
    for both LOCALDECL-ONLY (payload=True, folded into exempt_lines above,
    kept separate here for LOCALDECL-DEBT's payload=issue) and LOCALDECL-DEBT.
    """
    out, used = {}, set()
    for m, payload in markers:
        for ln in cast_lines:
            if ln in used or ln < m or ln > m + 8:
                continue
            out[ln] = payload
            used.add(ln)
            break
    return out


def scan_text(raw, path):
    """-> (sites, malformed_debt).

    site = (path, line, bare, debt_issue_or_None).

    `bare` is scope-based (see module docstring), not a line window. On the
    real tree today the token test (`\\bLocalConstDecl\\b` anywhere in scope)
    and this stricter test (an actual `dynamic_cast<...LocalConstDecl...>` in
    scope) give IDENTICAL counts (58 bare / 45 covered of 103 real sites) -
    every covered scope's mention is a real cast, never a stray identifier.
    The stricter test is used anyway because it is the more defensible
    definition of "this walker also handles LocalConstDecl", and because it
    stays correct if that coincidence ever stops holding.
    """
    text = strip_comments(raw)
    blocks = build_scope_index(text)
    cast_positions = [(m.start(), text.count('\n', 0, m.start()) + 1)
                      for m in CAST.finditer(text)]
    cast_lines = [ln for _, ln in cast_positions]
    exempt = exempt_lines(raw)
    debt_markers, malformed = debt_lines(raw)
    debt = _assign_next_cast_line(debt_markers, cast_lines)

    sites = []
    for pos, line in cast_positions:
        if line in exempt:
            continue
        scope = enclosing_root_scope(blocks, pos)
        if scope is None:
            bare = True  # no enclosing function found - flag it, do not hide it
        else:
            s, e = scope
            bare = not CONST_DECL_CAST.search(text[s:e])
        sites.append((path, line, bare, debt.get(line)))
    malformed_here = [(path, ln) for ln in malformed]
    return sites, malformed_here


def scan(root):
    sites, malformed = [], []
    for d in SCAN_DIRS:
        for p in sorted((root / d).rglob('*')):
            if p.suffix not in SUFFIXES:
                continue
            s, m = scan_text(p.read_text(), str(p.relative_to(root)))
            sites += s
            malformed += m
    return sites, malformed


def counts(sites):
    """{path: N} - BARE sites only. A covered site is not baselined; it is
    reported informationally (see main())."""
    out = {}
    for path, _, bare, _ in sites:
        if not bare:
            continue
        out[path] = out.get(path, 0) + 1
    return out


def debt_marked_paths(sites):
    """{path: N} - how many BARE sites in `sites` carry a well-formed
    LOCALDECL-DEBT marker. Used by check_debt_floor."""
    out = {}
    for path, _, bare, issue in sites:
        if bare and issue is not None:
            out[path] = out.get(path, 0) + 1
    return out


def malformed_debt_report(malformed):
    return ['%s:%d: LOCALDECL-DEBT marker has no `#NNNN` right after the colon'
            ' - a marker that cannot be checked cannot justify a rise (write '
            'e.g. `// LOCALDECL-DEBT: #3600 <reason>`)' % (path, line)
            for path, line in sorted(set(malformed))]


def check_debt_floor(cur, floor, sites):
    """-> violation strings. A rise above the GRANDFATHERED ceiling needs a
    per-site LOCALDECL-DEBT marker; `--force` alone is not enough. Identical
    mechanism to check_enumerable_grid_spellings.py / check_imported_symbol_
    reads.py post-#3590."""
    bad = []
    marked = debt_marked_paths(sites)
    for path in sorted(cur):
        have = cur[path]
        ceiling = floor.get(path, 0)
        if have <= ceiling:
            continue
        excess = have - ceiling
        got = marked.get(path, 0)
        if got < excess:
            bad.append(
                '%s has %d bare site(s), %d above its grandfathered ceiling '
                'of %d (scripts/localdecl_only_walkers.grandfather) - %d of '
                'the %d excess site(s) carry a `// LOCALDECL-DEBT: #NNNN '
                '<reason>` marker, %d do not. --force does not substitute '
                'for the marker.'
                % (path, have, excess, ceiling, got, excess, excess - got))
    return bad


class BaselineConflict(Exception):
    pass


def _parse_counts_file(text, source_name):
    """Parse a flat `<int> <path>` file. Shared by the mutable baseline and
    the frozen grandfather ceiling; `source_name` is what a merge-conflict
    error names."""
    out = {}
    for raw in text.splitlines():
        line = raw.strip()
        if line.startswith(('<<<<<<<', '=======', '>>>>>>>')):
            raise BaselineConflict(
                '%s has unresolved merge conflict markers.\n'
                '  Do NOT hand-merge the counts.\n'
                '  For the mutable baseline (scripts/localdecl_only_walkers.\n'
                '  baseline): take the CURRENT branch\'s version (the ratchet is a\n'
                '  floor, so the lower counts are right), then regenerate exactly:\n'
                '      git checkout --ours scripts/localdecl_only_walkers.baseline\n'
                '      python3 scripts/check_localdecl_only_walkers.py --update\n'
                '  For the frozen grandfather ceiling (scripts/localdecl_only_\n'
                '  walkers.grandfather): there is no --update for this file - it\n'
                '  changes only by a deliberate, reviewed edit, so resolve it by hand.'
                % source_name)
        if not line or line.startswith('#'):
            continue
        n, path = line.split(None, 1)
        out[path] = int(n)
    return out


def read_baseline():
    if not BASELINE.exists():
        return {}
    return _parse_counts_file(BASELINE.read_text(),
                               'scripts/localdecl_only_walkers.baseline')


def read_grandfather():
    """The per-file ceiling: bare-site counts already present in the tree
    when this ratchet landed, TIGHTENED DOWNWARD by every successful
    `--update` since. A missing file reads as an ALL-ZERO ceiling (fail
    closed) - see self_test_grandfather_missing_fails_closed."""
    if not GRANDFATHER.exists():
        return {}
    return _parse_counts_file(GRANDFATHER.read_text(),
                               'scripts/localdecl_only_walkers.grandfather')


def tighten_ceiling(existing, cur):
    """{path: N} - MONOTONE DOWNWARD ONLY: new[path] = min(existing.get(path,
    0), cur.get(path, 0)). Can never raise a ceiling; a path whose tightened
    ceiling reaches 0 is dropped."""
    out = {}
    for path in set(existing) | set(cur):
        new = min(existing.get(path, 0), cur.get(path, 0))
        if new > 0:
            out[path] = new
    return out


def write_grandfather(existing, cur):
    """Persist the tightened ceiling and report what changed as
    [(path, old, new), ...]. Called on EVERY successful `--update`, alongside
    write_baseline - a drain must become the new ceiling immediately, or the
    mechanism protects nothing during a drain campaign."""
    new = tighten_ceiling(existing, cur)
    lines = [
        '# localdecl_only_walkers.grandfather - the ceiling for #3591.',
        '#',
        '# THE CEILING ONLY EVER FALLS. `--update` MAY LOWER a per-file ceiling',
        '# (to min(existing, measured) - a drain becomes the new ceiling on the',
        '# very next --update) and MAY NEVER RAISE one; raising a ceiling stays a',
        '# deliberate, reviewed hand-edit to this file. A path whose tightened',
        '# ceiling reaches 0 is dropped entirely. See tighten_ceiling() /',
        '# check_debt_floor() in check_localdecl_only_walkers.py (same',
        '# rationale/mechanism as scripts/imported_symbol_reads.grandfather and',
        '# scripts/enumerable_grid_spellings.grandfather, #3590).',
        '#',
        '# One row per file: the number of BARE dynamic_cast<[const] ast::',
        '# LocalDecl*> sites (no LocalConstDecl handling found in the cast\'s',
        '# own enclosing function/lambda scope).',
        '',
    ]
    lines += ['%-4d %s' % (n, p) for p, n in sorted(new.items())]
    GRANDFATHER.write_text('\n'.join(lines) + '\n')
    changed = [(path, existing.get(path, 0), new.get(path, 0))
               for path in sorted(set(existing) | set(new))
               if new.get(path, 0) < existing.get(path, 0)]
    return new, changed


def write_baseline(cur):
    lines = [
        '# localdecl_only_walkers.baseline - the ratchet for #3591.',
        '# Regenerate with scripts/check_localdecl_only_walkers.py --update after a',
        '# bare site is fixed or exempted. The gate requires this file to EQUAL the',
        '# measured count, so BOTH a rise and stale headroom fail - a landed fix that',
        '# forgot to --update is a violation exactly like a regression, which is what',
        '# stops a later change from quietly handing back what an earlier one',
        '# ratcheted away. Raising a count needs --force, and once the rise pushes a',
        '# file above its ceiling in scripts/localdecl_only_walkers.grandfather',
        '# (which only ever falls - see that file\'s header), the excess needs a',
        '# `// LOCALDECL-DEBT: #NNNN <reason>` marker at each new site.',
        '#',
        '# One row per file: the number of BARE dynamic_cast<[const] ast::',
        '# LocalDecl*> sites - see check_localdecl_only_walkers.py\'s module',
        '# docstring for the scope-based classifier and',
        '# docs/proposals/papers/duplication-census-2026-08-30.md hotspot 8 / issue #3591',
        '# for the six prior instances of this exact defect shape.',
        '',
    ]
    lines += ['%-4d %s' % (n, p) for p, n in sorted(cur.items())]
    BASELINE.write_text('\n'.join(lines) + '\n')


def compare(cur, base, stale=True):
    """-> list of violation strings.

    stale=True  (the GATE): the baseline must EQUAL the measured count. Both a
                rise (a new bare site) and stale headroom (regenerated at an
                older base) are violations.
    stale=False (--update): only a rise or a new file matters. A count that
                went DOWN is the whole point of re-baselining.
    """
    bad = []
    for path in sorted(cur):
        have, want = cur[path], base.get(path)
        if want is None:
            bad.append('NEW FILE %s introduces %d bare site(s) - the family '
                       'may not grow a new site' % (path, have))
        elif have > want:
            bad.append('%s has %d bare site(s), baseline allows %d (+%d)'
                       % (path, have, want, have - want))
    for path in sorted(base):
        if not stale:
            continue
        have, want = cur.get(path, 0), base[path]
        if want > have:
            bad.append('%s baseline claims %d bare site(s) but the tree has %d'
                       ' - STALE HEADROOM (+%d). The baseline is exact, not a '
                       'ceiling: re-run --update on the merged tree.'
                       % (path, want, have, want - have))
    return bad


# ---------------------------------------------------------------------------
# self-test: the detector must go RED on a known positive before it is
# allowed to certify anything.
# ---------------------------------------------------------------------------
FIXTURES = [
    # (name, body, want_bare_count)
    ('positive_bare_simple.cpp',
     'void f(const ast::Stmt* s) {\n'
     '    if (auto* ld = dynamic_cast<const ast::LocalDecl*>(s)) {\n'
     '        use(ld);\n'
     '    }\n'
     '}\n', 1),
    ('positive_bare_non_const.cpp',
     'void f(ast::Stmt* s) {\n'
     '    if (auto* ld = dynamic_cast<ast::LocalDecl*>(s)) {\n'
     '        use(ld);\n'
     '    }\n'
     '}\n', 1),
    ('positive_bare_multiline.cpp',
     'void f(const ast::Stmt* s) {\n'
     '    const auto* ld =\n'
     '        dynamic_cast<\n'
     '            const ast::LocalDecl*>(\n'
     '                s);\n'
     '    use(ld);\n'
     '}\n', 1),
    # A sibling LocalConstDecl arm in the SAME function -> covered, not bare.
    ('negative_covered_same_function.cpp',
     'void f(const ast::Stmt* s) {\n'
     '    if (auto* ld = dynamic_cast<const ast::LocalDecl*>(s)) {\n'
     '        use(ld);\n'
     '    }\n'
     '    if (auto* lc = dynamic_cast<const ast::LocalConstDecl*>(s)) {\n'
     '        use(lc);\n'
     '    }\n'
     '}\n', 0),
    # THE LOAD-BEARING FIXTURE: a sibling LocalConstDecl arm in a DIFFERENT,
    # UNRELATED function must NOT cover this one - proves the scope index
    # does not leak across a function boundary. A fixed +/-N-line window
    # would get this wrong for a short-enough gap; that is exactly the
    # census's own "upper bound, not a defect count" caveat this script is
    # built to do better than.
    ('negative_sibling_in_other_function_does_not_leak.cpp',
     'void f(const ast::Stmt* s) {\n'
     '    if (auto* ld = dynamic_cast<const ast::LocalDecl*>(s)) {\n'
     '        use(ld);\n'
     '    }\n'
     '}\n'
     '\n'
     'void g(const ast::Stmt* s) {\n'
     '    if (auto* lc = dynamic_cast<const ast::LocalConstDecl*>(s)) {\n'
     '        use(lc);\n'
     '    }\n'
     '}\n', 1),
    # A cast inside a NESTED LAMBDA is scoped to the lambda, not the
    # enclosing function - the enclosing function's own (unrelated) const
    # handling does not cover a walker lambda that omits it.
    ('negative_lambda_scope_not_covered_by_enclosing_function.cpp',
     'void f(const std::vector<std::unique_ptr<ast::Stmt>>& body) {\n'
     '    if (auto* lc = dynamic_cast<const ast::LocalConstDecl*>(body[0].get())) {\n'
     '        use(lc);\n'
     '    }\n'
     '    std::function<void(const ast::Stmt*)> scan = [&](const ast::Stmt* s) {\n'
     '        if (auto* ld = dynamic_cast<const ast::LocalDecl*>(s)) {\n'
     '            use(ld);\n'
     '        }\n'
     '    };\n'
     '    for (auto& s : body) scan(s.get());\n'
     '}\n', 1),
    # ...but a lambda that DOES handle both, in its own body, is covered.
    ('positive_lambda_scope_covered_within_itself.cpp',
     'void f(const std::vector<std::unique_ptr<ast::Stmt>>& body) {\n'
     '    std::function<void(const ast::Stmt*)> scan = [&](const ast::Stmt* s) {\n'
     '        if (auto* ld = dynamic_cast<const ast::LocalDecl*>(s)) {\n'
     '            use(ld);\n'
     '        }\n'
     '        if (auto* lc = dynamic_cast<const ast::LocalConstDecl*>(s)) {\n'
     '            use(lc);\n'
     '        }\n'
     '    };\n'
     '    for (auto& s : body) scan(s.get());\n'
     '}\n', 0),
    # A for-loop header's internal `;`s must not fragment the scope index -
    # the cast inside the loop body still sees a LocalConstDecl arm later in
    # the SAME enclosing function.
    ('negative_for_loop_header_semicolons_do_not_fragment_scope.cpp',
     'void f(std::vector<ast::Stmt*>& v) {\n'
     '    for (std::size_t i = 0; i < v.size(); ++i) {\n'
     '        if (auto* ld = dynamic_cast<const ast::LocalDecl*>(v[i])) {\n'
     '            use(ld);\n'
     '        }\n'
     '    }\n'
     '    if (auto* lc = dynamic_cast<const ast::LocalConstDecl*>(v[0])) {\n'
     '        use(lc);\n'
     '    }\n'
     '}\n', 0),
    # Prose - a comment quoting the cast is not a cast.
    ('negative_line_comment.cpp',
     '// dynamic_cast<const ast::LocalDecl*>(s) is the pattern to avoid\n',
     0),
    ('negative_block_comment.cpp',
     '/* explains dynamic_cast<const ast::LocalDecl*>(s) at length */\n',
     0),
    # ...but real code beside a comment still counts.
    ('positive_code_beside_comment.cpp',
     'void f(const ast::Stmt* s) {\n'
     '    if (auto* ld = dynamic_cast<const ast::LocalDecl*>(s)) {  // #3591\n'
     '        use(ld);\n'
     '    }\n'
     '}\n', 1),
    # LOCALDECL-ONLY exemption, with and without a reason.
    ('exempt_marker_with_reason.cpp',
     'void f(const ast::Stmt* s) {\n'
     '    // LOCALDECL-ONLY: assignment targets can never be const (§21.1)\n'
     '    if (auto* ld = dynamic_cast<const ast::LocalDecl*>(s)) {\n'
     '        use(ld);\n'
     '    }\n'
     '}\n', 0),
    ('exempt_marker_no_reason_does_not_exempt.cpp',
     'void f(const ast::Stmt* s) {\n'
     '    // LOCALDECL-ONLY:\n'
     '    if (auto* ld = dynamic_cast<const ast::LocalDecl*>(s)) {\n'
     '        use(ld);\n'
     '    }\n'
     '}\n', 1),
    ('exempt_marker_does_not_leak.cpp',
     'void f(const ast::Stmt* s, const ast::Stmt* t) {\n'
     '    // LOCALDECL-ONLY: only the next one\n'
     '    if (auto* a = dynamic_cast<const ast::LocalDecl*>(s)) use(a);\n'
     '    if (auto* b = dynamic_cast<const ast::LocalDecl*>(t)) use(b);\n'
     '}\n', 1),
    # LOCALDECL-DEBT does NOT exempt - it still counts, unlike LOCALDECL-ONLY.
    ('debt_marker_still_counts.cpp',
     'void f(const ast::Stmt* s) {\n'
     '    // LOCALDECL-DEBT: #3600 tracked, will consolidate\n'
     '    if (auto* ld = dynamic_cast<const ast::LocalDecl*>(s)) {\n'
     '        use(ld);\n'
     '    }\n'
     '}\n', 1),
]


def self_test_fixtures():
    ok = True
    for name, body, want in FIXTURES:
        sites, _malformed = scan_text(body, name)
        got = sum(1 for _, _, bare, _ in sites if bare)
        good = (got == want)
        ok = ok and good
        print('  [%s] %-58s bare=%d (want %d)'
              % ('ok  ' if good else 'FAIL', name, got, want))
    return ok


DEBT_FIXTURES = [
    ('debt_marker_above.cpp',
     '// LOCALDECL-DEBT: #3600 tracked, will consolidate later\n'
     'x = dynamic_cast<const ast::LocalDecl*>(s);\n',
     [(1, 3600)], []),
    ('debt_marker_no_issue.cpp',
     '// LOCALDECL-DEBT: needs consolidating eventually\n'
     'x = dynamic_cast<const ast::LocalDecl*>(s);\n',
     [], [1]),
    ('debt_marker_issue_not_leading.cpp',
     '// LOCALDECL-DEBT: relates to the #3591 umbrella, no local issue yet\n'
     'x = dynamic_cast<const ast::LocalDecl*>(s);\n',
     [], [1]),
]


def self_test_debt_parsing():
    ok = True
    for name, body, want_markers, want_malformed in DEBT_FIXTURES:
        got_markers, got_malformed = debt_lines(body)
        good = (got_markers == want_markers and got_malformed == want_malformed)
        ok = ok and good
        print('  [%s] debt/%-34s markers=%s (want %s) malformed=%s (want %s)'
              % ('ok  ' if good else 'FAIL', name, got_markers, want_markers,
                 got_malformed, want_malformed))
    return ok


COMPARE_CASES = [
    # (label, cur, base, gate_violations, update_violations)
    ('exact',    {'a.cpp': 3}, {'a.cpp': 3}, 0, 0),
    ('rise',     {'a.cpp': 4}, {'a.cpp': 3}, 1, 1),
    ('stale',    {'a.cpp': 2}, {'a.cpp': 3}, 1, 0),
    ('new file', {'b.cpp': 1}, {'a.cpp': 3}, 2, 1),
]


def self_test_compare():
    ok = True
    for label, cur, base, want_gate, want_upd in COMPARE_CASES:
        g = len(compare(cur, base, stale=True))
        u = len(compare(cur, base, stale=False))
        good = (g == want_gate and u == want_upd)
        ok = ok and good
        print('  [%s] compare/%-8s gate=%d (want %d)  update=%d (want %d)'
              % ('ok  ' if good else 'FAIL', label, g, want_gate, u, want_upd))
    return ok


DEBT_FLOOR_CASES = [
    # (label, cur, floor, sites, want_violations)
    ('at ceiling',      {'a.cpp': 3}, {'a.cpp': 3}, [], 0),
    ('under ceiling',   {'a.cpp': 2}, {'a.cpp': 3}, [], 0),
    ('excess, no marker', {'a.cpp': 4}, {'a.cpp': 3}, [], 1),
    ('excess, fully marked',
     {'a.cpp': 4}, {'a.cpp': 3}, [('a.cpp', 10, True, 9001)], 0),
    ('excess, partially marked',
     {'a.cpp': 5}, {'a.cpp': 3}, [('a.cpp', 10, True, 9001)], 1),
    ('new file, no ceiling on record', {'b.cpp': 1}, {}, [], 1),
    ('new file, marked', {'b.cpp': 1}, {}, [('b.cpp', 5, True, 9002)], 0),
]


def self_test_debt_floor():
    ok = True
    for label, cur, floor, sites, want in DEBT_FLOOR_CASES:
        got = len(check_debt_floor(cur, floor, sites))
        good = (got == want)
        ok = ok and good
        print('  [%s] debt-floor/%-28s violations=%d (want %d)'
              % ('ok  ' if good else 'FAIL', label, got, want))
    return ok


GRANDFATHER_TIGHTEN_CASES = [
    ('drain lowers the ceiling', {'a.cpp': 28}, {'a.cpp': 20}, {'a.cpp': 20}),
    ('unchanged stays unchanged', {'a.cpp': 28}, {'a.cpp': 28}, {'a.cpp': 28}),
    ('marked rise above ceiling does not raise it',
     {'a.cpp': 20}, {'a.cpp': 25}, {'a.cpp': 20}),
    ('new file gets no ceiling for free', {}, {'b.cpp': 5}, {}),
    ('fully drained file is dropped', {'a.cpp': 28}, {}, {}),
    ('cannot rise even when measured is higher and marked',
     {'a.cpp': 20}, {'a.cpp': 30}, {'a.cpp': 20}),
]


def self_test_grandfather_tighten():
    ok = True
    for label, existing, cur, want in GRANDFATHER_TIGHTEN_CASES:
        got = tighten_ceiling(existing, cur)
        good = (got == want)
        ok = ok and good
        print('  [%s] tighten/%-40s got %s (want %s)'
              % ('ok  ' if good else 'FAIL', label, got, want))
    return ok


def self_test_drain_protection():
    """FAIL-WITHOUT/PASS-WITH: a drain must immediately become the new
    ceiling, or a later regrow within the OLD ceiling is silently permitted."""
    old_ceiling = {'a.cpp': 28}
    drained = {'a.cpp': 20}
    new_ceiling = tighten_ceiling(old_ceiling, drained)
    step1_ok = (new_ceiling == {'a.cpp': 20})

    regrown = {'a.cpp': 25}
    violations_without_fix = len(check_debt_floor(regrown, old_ceiling, []))
    violations_with_fix = len(check_debt_floor(regrown, new_ceiling, []))

    good = (step1_ok and violations_without_fix == 0 and violations_with_fix == 1)
    print('  [%s] drain-protection: tighten(28->20)=%s ceiling; regrow to 25 '
          '(<=28) -> WITHOUT tightening: %d violation(s) (bug, want 0 to prove '
          'it reproduces); WITH tightening: %d violation(s) (fix, want 1)'
          % ('ok  ' if good else 'FAIL', step1_ok, violations_without_fix,
             violations_with_fix))
    return good


def self_test_grandfather_missing_fails_closed():
    cur = {'a.cpp': 5}
    violations = check_debt_floor(cur, {}, [])
    good = (len(violations) == 1)
    print('  [%s] grandfather-missing fails CLOSED: violations=%d (want 1 - '
          'the whole count, not silently accepted)'
          % ('ok  ' if good else 'FAIL', len(violations)))
    return good


def self_test():
    ok = self_test_fixtures()
    ok = self_test_debt_parsing() and ok
    ok = self_test_compare() and ok
    ok = self_test_debt_floor() and ok
    ok = self_test_grandfather_tighten() and ok
    ok = self_test_drain_protection() and ok
    ok = self_test_grandfather_missing_fails_closed() and ok
    # A detector that cannot see the real tree is also broken.
    sites, _malformed = scan(ROOT)
    bare = sum(1 for _, _, b, _ in sites if b)
    covered = len(sites) - bare
    if not sites or not bare or not covered:
        ok = False
        print('  [FAIL] whole-tree scan found %d site(s), %d bare, %d covered'
              ' - a matcher that matches nothing (or classifies nothing either'
              ' way) is what a false green looks like' % (len(sites), bare, covered))
    else:
        print('  [ok  ] whole-tree scan found %d site(s): %d bare, %d covered'
              % (len(sites), bare, covered))
    print('check_localdecl_only_walkers --self-test: ' + ('PASS' if ok else 'FAIL'))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--strict', action='store_true',
                    help='exit 1 on any violation (the gate phase)')
    ap.add_argument('--self-test', action='store_true')
    ap.add_argument('--update', action='store_true',
                    help='rewrite the baseline from the current tree')
    ap.add_argument('--force', action='store_true',
                    help='with --update, permit a count to RISE')
    ap.add_argument('--list', action='store_true', help='print every site')
    a = ap.parse_args()

    if a.self_test:
        return self_test()

    sites, malformed = scan(ROOT)
    cur = counts(sites)
    base = read_baseline()
    floor = read_grandfather()

    if a.list:
        for path, line, bare, debt in sites:
            tag = 'BARE   ' if bare else 'covered'
            suffix = '  [LOCALDECL-DEBT #%d]' % debt if (bare and debt) else ''
            print('%s  %s:%d%s' % (tag, path, line, suffix))
        for path, line in sorted(set(malformed)):
            print('malformed-debt-marker  %s:%d' % (path, line))

    # A rise above the grandfathered ceiling needs a per-site LOCALDECL-DEBT
    # marker; checked on EVERY run, not only when --force is passed.
    debt_bad = check_debt_floor(cur, floor, sites) + malformed_debt_report(malformed)

    if a.update:
        rises = [v for v in compare(cur, base, stale=False) if base]
        if rises and not a.force:
            print('REFUSING to update: the baseline may only go DOWN.')
            for v in rises:
                print('  ' + v)
            print('Use --force if the rise is deliberate, and say why in the commit.')
            return 1
        if debt_bad:
            print('REFUSING to update: new bare site(s) above the grandfathered '
                  'ceiling need a per-site marker - --force alone is not enough.')
            for v in debt_bad:
                print('  ' + v)
            print('Add `// LOCALDECL-DEBT: #NNNN <reason>` at each new site, '
                  'naming a real issue that tracks the debt, or bring the count')
            print('back down to scripts/localdecl_only_walkers.grandfather\'s ceiling.')
            return 1
        write_baseline(cur)
        _new, ceiling_changed = write_grandfather(floor, cur)
        bare_total = sum(cur.values())
        covered_total = sum(1 for _, _, b, _ in sites if not b)
        print('baseline updated: %d bare site(s) in %d file(s), %d covered '
              'site(s) (informational, not baselined)'
              % (bare_total, len(cur), covered_total))
        if ceiling_changed:
            print('grandfather ceiling tightened (%d file(s)):' % len(ceiling_changed))
            for path, old_v, new_v in ceiling_changed:
                print('  %s: %d -> %d' % (path, old_v, new_v))
        return 0

    bad = compare(cur, base) + debt_bad
    if bad:
        print('LOCALDECL-ONLY WALKER FAMILY: %d violation(s) (#3591)' % len(bad))
        for v in bad:
            print('  ' + v)
        print('\nA new dynamic_cast<[const] ast::LocalDecl*> site with no sibling')
        print('LocalConstDecl handling in its own enclosing function/lambda scope')
        print('silently skips every §21.1 `const` local - not a null-deref, a')
        print('silent no-op for a whole binding form. Six prior instances of this')
        print('exact shape ranged from a 420 B leak to a use-after-free (#2802,')
        print('#3375, #3402, #3416, #3417, #3427). See LlvmBackend_Setup.cpp:24776')
        print('for the canonical fix shape (widen the candidate vector from')
        print('`const ast::LocalDecl*` to `const ast::Node*`), or mark the site')
        print('`// LOCALDECL-ONLY: <reason>` if a const local truly cannot reach it.')
        return 1 if a.strict else 0
    bare_total, covered_total = sum(cur.values()), sum(1 for _, _, b, _ in sites if not b)
    print('check_localdecl_only_walkers: clean - %d bare site(s) in %d file(s) '
          '(baseline %d), %d covered site(s)'
          % (bare_total, len(cur), sum(base.values()), covered_total))
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except BaselineConflict as e:
        print('LOCALDECL-ONLY WALKER FAMILY: %s' % e)
        sys.exit(1)
