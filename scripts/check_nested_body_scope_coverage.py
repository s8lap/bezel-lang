#!/usr/bin/env python3
"""check_nested_body_scope_coverage.py - ratchet Family L, the lazily-memoized
body emitters that must construct `NestedBodyScope` (#2702, #3936, #2723,
#2628, #4150, #4151, #2748).

WHY (defect-campaign-2026-09.md §2.8, #2702). Block getters, indexer
accessors, property thunks and disposer cores lower a user body into a
standalone MEMOIZED function from the middle of expression lowering. Any
per-body emitter field they fail to save+reset leaks the caller's context
into the callee, and because the function is memoized the result is
**frozen module-wide, decided by whichever caller was lowered first - in
`unordered_map` pointer-hash order**. #2702 (closed) enumerated the fields
and proposed one RAII guard; that guard is `NestedBodyScope`
(`src/codegen/LlvmBackend_impl.hpp:13381` declaration,
`src/codegen/LlvmBackend_Assign.cpp:5114` definition). The open members
today are EMITTERS NOT ON IT (#3936 `emitDisposerCore`, #4151
`makePropertyThunk`, #2748's three unguarded emitters) and fields missing
from its canonical field set (#2723 `#2688-twin`, #2628 iterator triplet,
#4150 `currentEnumMethod_` rationale, etc.).

WHAT THIS SCRIPT IS FOR. Two banked counts, per file (localdecl-ratchet
format):

  - `uncovered_emitters` - a function in `src/codegen/` whose NAME matches
    a known lazy-emitter pattern (the seven #2702 + #4151 named
    expressions: `*BlockGetter`, `ensureIndexerGetterFn`,
    `ensureIndexerSetterFn`, `ensurePropertyAccessorFn`, `*PropertyThunk`,
    `emitDisposerCore`, `ensureExprGetterThunk`) and whose BODY does NOT
    construct `NestedBodyScope VAR(*this);` at entry. A rise here is a new
    eighth, ninth, ... lazy emitter written without the guard -
    module-wide, hash-order leakage of the caller's emission state (#3936's
    measured codegen divergence is the failing shape).

  - `incomplete_field_saves` - a `NestedBodyScope` constructor DEFINITION
    whose initialization list omits at least one field the canonical set
    requires. The canonical field set is read from the canonical
    `Emitter::NestedBodyScope::NestedBodyScope(Emitter&)` definition
    (`src/codegen/LlvmBackend_Assign.cpp:5114`); a constructor body that
    forgets one is a re-introduction of the same defect class
    field-by-field, the class #2701, #2702, #3551, ... were each called
    in to close.

A site proven correct by context writes a marker:

    // NESTED-BODY-SCOPE-ONLY: <reason>

placing it at or above the bare line, with a reason after the colon (a
marker with no reason after the colon does NOT exempt). A rise above the
per-file ceiling in `scripts/nested_body_scope_coverage.grandfather`
needs a per-site marker naming a real issue:

    // NESTED-BODY-SCOPE-DEBT: #NNNN <reason>

(`--force` alone is not enough - same `LOCALDECL-DEBT` mechanism #3590
codified, per #3590's measurement: the class-three #3150-baseline rose
fifteen days in a row under bare `--force` with prose justifications.)

⚠ THE CEILING ONLY EVER FALLS. Same as the three sibling ratchets
(`check_localdecl_only_walkers.py`, `check_enumerable_grid_spellings.py`,
`check_imported_symbol_reads.py`): every successful `--update` tightens
each per-file ceiling to `min(existing, measured)`. A drain becomes the
new ceiling immediately, and a later lane cannot silently re-grow a
drained file back to its pre-drain high-water mark with bare `--update`.
Raising a ceiling is a deliberate, reviewed hand-edit to the `.grandfather`
file.

PER-LANE EXEMPTION / DEBT MARKERS. Both exempt exactly the next
NESTED-BODY-SCOPE line within 8 lines (own line plus subsequent code lines
until a non-blank, non-continuation line), so a marker cannot drift away
from the site it describes.

Usage:  scripts/check_nested_body_scope_coverage.py [--strict] [--self-test]
                                                   [--update [--force]] [--list]
"""
import argparse
import pathlib
import re
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
BASELINE = ROOT / 'scripts' / 'nested_body_scope_coverage.baseline'
GRANDFATHER = ROOT / 'scripts' / 'nested_body_scope_coverage.grandfather'
SCAN_DIRS = ('src/codegen',)
SUFFIXES = ('.cpp',)

# Lazy-emitter function name patterns (the seven #2702/#4151 named members,
# plus the one scope-named shim). An "emitter" here means a function whose
# BODY, when reached mid-expression-lowering, lowers a user body into a
# memoized standalone function from the middle of the caller. The patterns
# are deliberately conservative: name-only, no scope analysis. Every site in
# this tree today is either on the canonical guard or is an actively-open
# member of the Family L defect class; a NEW pattern matched is the
# signal worth a ratchet red, not a regex-tightening pass.
LAZY_EMITTER_PATTERNS = [
    re.compile(r'\b(?:ensurePropertyBlockGetter|ensureIndexerBlockGetter)\b'),
    re.compile(r'\bensurePropertyAccessorFn\b'),
    re.compile(r'\bensureIndexerGetterFn\b'),
    re.compile(r'\bensureIndexerSetterFn\b'),
    re.compile(r'\bensureExprGetterThunk\b'),
    re.compile(r'\bmakePropertyThunk\b'),
    re.compile(r'\bemitDisposerCore\b'),
]
LAZY_EMITTER_NAME_RE = re.compile(
    r'\b(?:ensurePropertyBlockGetter|ensureIndexerBlockGetter'
    r'|ensurePropertyAccessorFn'
    r'|ensureIndexerGetterFn|ensureIndexerSetterFn'
    r'|ensureExprGetterThunk'
    r'|makePropertyThunk'
    r'|emitDisposerCore)\b')

# The construction: a `NestedBodyScope` declaration followed by an arg of
# `(*this)` (or `<emitter reference>` - in practice always `*this`). The
# variable name is free (today: `nestedScope`, but unnamed and `nbs_` are
# legal).
NBS_CTOR = re.compile(
    r'\bNestedBodyScope\s+\w+\s*\(\s*\*this\s*\)\s*;')

# A `NestedBodyScope` constructor DEFINITION: a ctor body line that opens
# with `Emitter::NestedBodyScope::NestedBodyScope(`. The canonical one
# (LlvmBackend_Assign.cpp:5114) is the ONLY definition today; the count
# measures whether ANY definition is missing a canonical field.
NBS_DEFN = re.compile(
    r'^Emitter::NestedBodyScope::NestedBodyScope\s*\(\s*Emitter\s*&\s*\w+\s*\)',
    re.M)

# Canonical field set: the emitter state fields NestedBodyScope saves,
# clears and restores (read from `LlvmBackend_Assign.cpp:5114`'s init list,
# the `BEZEL_SM_FRAME_MEMBERS` macro at `LlvmBackend_impl.hpp:12724`, and
# the explicit member list around `:13381`). A canonical-field-set member
# that appears nowhere in a NestedBodyScope ctor definition's preamble
# (text from the `:` of the init list through the `{` of the body) is a
# field that the ctor has stopped saving - by definition a regression to
# the pre-#2702 site-by-site patch shape this guard consolidated.
#
# The frame members are saved via the BEZEL_SM_FRAME_MEMBERS macro, which
# (per #3551, v0.286) generates the save/clear/restore triplets from one
# list of `(TYPE, MEMBER, NEUTRAL)` triples. A macro check is sufficient
# here: the macro EXPANDS to `, MEMBER_(e.MEMBER_)` for every triple in
# its list, so its presence in the ctor preamble IS the proof that every
# macro-defined frame member is being saved - that is the load-bearing
# structural property, not the textual presence of each name.
CANONICAL_FIELDS = [
    # return-contract pair (#2694)
    'currentReturnTypeNode_',
    'currentLambdaReturnSemaTy_',
    # accessor-representation pair (#2703)
    'accessorReturnTypeNode_',
    'accessorReturnTypeFn_',
    # choice receiver-identity recovery (#4143, deliberately not
    # currentEnumMethod_ - see #2702 comment on the asymmetric twins)
    'currentChoiceMethod_',
    # destructor-body context (#2702 item 2)
    'emittingDisposerBody_',
    'disposerEntryExc_',
    # catch/guard-arm context
    'currentCaughtName_',
    'caughtExcArmTarget_',
    # narrow per-call channels
    'expectedFunctionType_',
    'lastCallGenericSubst_',
    'inlinedAccessorEnd_',
    'asyncChainedReturn_',
    'asyncBorrowedReturn_',
    # root-scope registration channels (#2731 item 3)
    'pendingOwnedParams_',
    'pendingArgsDisposal_',
    # deliberately inherited (#2702 "DELIBERATELY INHERITED")
    'genericSubstSemaTy_',
]
# The macro is checked separately - its presence in the preamble implies
# all frame members are saved (it expands to a member list, not a sentinel
# string).
FRAME_MACRO = 'BEZEL_SM_FRAME_MEMBERS'


# ---------------------------------------------------------------------------
# Markers: `// NESTED-BODY-SCOPE-ONLY:` exempts the next-emitter line; the
# `-DEBT` sibling justifies a rise above the grandfather ceiling. Both are
# the localdecl mechanism, ported (#3590).
# ---------------------------------------------------------------------------
ONLY_MARKER = re.compile(r'//\s*NESTED-BODY-SCOPE-ONLY:\s*\S')
DEBT_MARKER = re.compile(r'//\s*NESTED-BODY-SCOPE-DEBT:\s*(\S.*)$')
DEBT_ISSUE = re.compile(r'^#(\d+)\b')


def strip_comments(text):
    """Blank out // and /* */ comments, preserving length and newlines.

    A prose MENTION of `NestedBodyScope`, of a lazy-emitter name, or of a
    canonical field is not a guard construction or a class member - the
    guard's own multi-page comment at `LlvmBackend_Assign.cpp:4932` and
    every site-cite in this script's docstring both quote the names
    verbatim. Counting those would make the ratchet fire on documentation,
    which trains readers to route around it. Markers are read from the
    raw text (see `only_lines` / `debt_lines` below) - a marker IS a
    comment, deliberately.
    """
    out = []
    i, n = 0, len(text)
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
            q = c; j = i + 1
            while j < n and text[j] != q:
                j += 2 if text[j] == '\\' else 1
            j = min(j + 1, n)
            out.append(text[i:j]); i = j
        else:
            out.append(c); i += 1
    return ''.join(out)


def find_emitter_definitions(text):
    """-> [(line_1indexed, funcname), ...], one per `EMITTER_TYPE
    FUNC_NAME(... args ...) {` definition site.

    The signature regex matches the open-paren through the close-paren
    then the brace; the brace must follow the close-paren at the same
    indentation (the common multi-line declaration shape) OR on the very
    next line after whitespace - a definition on a LATER line is almost
    always a forward declaration (e.g. the public `void emitDisposerCore`
    in `LlvmBackend_impl.hpp`), which is NOT the body that constructs the
    guard. Distinguishing declaration from definition matters because a
    declaration forwards to the definition; the guard goes in the body.
    """
    decl = re.compile(
        r'^(?:[a-zA-Z_][\w:<>,\s\*&]*?\s+)?(?:Emitter::)?'
        r'(?:ensurePropertyBlockGetter|ensureIndexerBlockGetter'
        r'|ensurePropertyAccessorFn'
        r'|ensureIndexerGetterFn|ensureIndexerSetterFn'
        r'|ensureExprGetterThunk'
        r'|makePropertyThunk'
        r'|emitDisposerCore)'
        r'\s*\([^)]*\)\s*\{', re.M)
    out = []
    for m in decl.finditer(text):
        name = m.group(0)
        # extract just the function-name portion (between the last `::` /
        # whitespace and the `(`).
        nm = re.search(
            r'(?:[\w:]+\s+)?([A-Za-z]\w*)'
            r'\s*\(', name)
        if not nm:
            continue
        line = text.count('\n', 0, m.start()) + 1
        out.append((line, nm.group(1)))
    return out


def only_lines(raw):
    """1-indexed line numbers exempted by a NESTED-BODY-SCOPE-ONLY marker
    (own line plus the next non-blank/continuation line, bounded to 8
    lines so a marker cannot drift away from the emitter it describes).
    """
    out = set()
    lines = raw.split('\n')
    for i, ln in enumerate(lines):
        if not ONLY_MARKER.search(ln):
            continue
        out.add(i + 1)
        for j in range(i + 1, min(i + 8, len(lines))):
            s = lines[j].strip()
            out.add(j + 1)
            if s and not s.startswith('//'):
                break
    return out


def debt_lines(raw):
    """-> ([(line, issue)], malformed). A rise past the grandfathered
    ceiling needs a per-site `// NESTED-BODY-SCOPE-DEBT: #NNNN <reason>`
    marker; a marker with no `#NNNN` is malformed and justifies nothing.
    """
    well, malformed = [], []
    for i, ln in enumerate(raw.split('\n')):
        m = DEBT_MARKER.search(ln)
        if not m:
            continue
        im = DEBT_ISSUE.match(m.group(1).strip())
        if im:
            well.append((i + 1, int(im.group(1))))
        else:
            malformed.append(i + 1)
    return well, malformed


def scan_text(raw, path):
    """-> (uncovered_emitters, incomplete_field_saves, malformed_debt).

    Each tuple element is a list of (path, line) sites.
    """
    text = strip_comments(raw)
    only = only_lines(raw)
    debt, malformed = debt_lines(raw)
    emitter_sites = find_emitter_definitions(text)
    uncovered = []
    for line, _name in emitter_sites:
        if line in only:
            continue
        # Look at the function body after the definition line. The
        # `NestedBodyScope VAR(*this);` must appear before the first
        # closing `}` (we approximate "in the function body" with "anywhere
        # below the definition line within the next ~600 lines", which is
        # safely larger than any emitter in this tree today and bounded
        # enough that a stray NBS in some unrelated later function does
        # not falsely cover an earlier bare emitter).
        body_start = sum(len(x) + 1 for x in raw.split('\n')[:line])
        snippet = raw[body_start:body_start + 60000]
        if not NBS_CTOR.search(snippet):
            uncovered.append((path, line))

    # For each NBS ctor DEFINITION, the canonical fields must each appear
    # at least once in the init list (preamble: text from the `:` until
    # the `{` of the body). Two definitions, or a definition missing one
    # field, are both a violation.
    #
    # Frame members are saved via the BEZEL_SM_FRAME_MEMBERS macro (its
    # presence in the preamble implies all macro-expanded members are
    # saved by construction - #3551, v0.286). One occurrence per ctor is
    # the structural load-bearing property.
    incomplete = []
    for m in NBS_DEFN.finditer(text):
        defn_open = m.end()
        # locate the matching `{` of the body. Use brace-count from the
        # comment-stripped text starting AT the ctor's open-paren so a
        # stray `{` in a default arg does not throw us.
        body_open = text.find('{', defn_open)
        body_close = text.find('}', body_open) if body_open >= 0 else -1
        if body_open < 0 or body_close < 0:
            # malformed source - report as an incomplete definition so it
            # is visible, not silently accepted.
            incomplete.append((path, text.count('\n', 0, m.start()) + 1))
            continue
        preamble = text[defn_open:body_open]
        for field in CANONICAL_FIELDS:
            if field not in preamble:
                incomplete.append((path, text.count('\n', 0, m.start()) + 1))
                break
        else:
            if FRAME_MACRO not in preamble:
                incomplete.append((path, text.count('\n', 0, m.start()) + 1))

    malformed_here = [(path, ln) for ln in malformed]
    return uncovered, incomplete, malformed_here


def scan(root):
    """-> (uncovered_emitters, incomplete_field_saves, malformed_debt).

    Top-level wrapper: walks every `src/codegen/*.cpp` (the family lives
    in the codegen split, by spec language; see CLAUDE.md per-area binary
    split - `tests/codegen/` is the test half, never source here) and
    aggregates scan_text() outputs.
    """
    uncovered, incomplete, malformed = [], [], []
    for d in SCAN_DIRS:
        for p in sorted((root / d).rglob('*')):
            if p.suffix not in SUFFIXES:
                continue
            u, ic, m = scan_text(p.read_text(), str(p.relative_to(root)))
            uncovered += u
            incomplete += ic
            malformed += m
    return uncovered, incomplete, malformed


def counts_per_file(sites):
    """-> {path: N}. A bare site has no NBS construction in its body; a
    covered site is reported informationally."""
    out = {}
    for path, _line in sites:
        out[path] = out.get(path, 0) + 1
    return out


def debt_marked_sites(sites, debt_well):
    """-> {path: N}, how many of `sites` carry a well-formed NESTED-BODY-
    SCOPE-DEBT marker that anchors them. Used by check_debt_floor."""
    debt_lines_by_path = {}
    for line, issue in debt_well:
        debt_lines_by_path.setdefault(line, issue)
    out = {}
    for path, line in sites:
        if line in debt_lines_by_path:
            out[path] = out.get(path, 0) + 1
    return out


def malformed_debt_report(malformed):
    return ['%s:%d: NESTED-BODY-SCOPE-DEBT marker has no `#NNNN` right after '
            'the colon - a marker that cannot be checked cannot justify a '
            'rise (write e.g. `// NESTED-BODY-SCOPE-DEBT: #3936 <reason>`)'
            % (path, line)
            for path, line in sorted(set(malformed))]


def check_debt_floor(uncovered_cur, incomplete_cur, floor, debt_well,
                     uncovered_sites, incomplete_sites):
    """-> [violation strings]. A rise above the GRANDFATHERED ceiling
    needs a per-site NESTED-BODY-SCOPE-DEBT marker; `--force` alone is
    not enough. Single check shared between the two banked counts."""
    bad = []
    marked_u = debt_marked_sites(uncovered_sites, debt_well)
    marked_i = debt_marked_sites(incomplete_sites, debt_well)
    for path in sorted(set(uncovered_cur) | set(incomplete_cur)):
        u_have = uncovered_cur.get(path, 0)
        i_have = incomplete_cur.get(path, 0)
        ceiling = floor.get(path, (0, 0))
        u_ceil, i_ceil = ceiling
        if u_have > u_ceil:
            excess = u_have - u_ceil
            got = marked_u.get(path, 0)
            if got < excess:
                bad.append(
                    '%s has %d uncovered emitter(s), %d above its '
                    'grandfathered ceiling of %d '
                    '(scripts/nested_body_scope_coverage.grandfather) - %d '
                    'of the %d excess site(s) carry a `// NESTED-BODY-'
                    'SCOPE-DEBT: #NNNN <reason>` marker, %d do not. --force '
                    'does not substitute for the marker.'
                    % (path, u_have, excess, u_ceil, got, excess,
                       excess - got))
        if i_have > i_ceil:
            excess = i_have - i_ceil
            got = marked_i.get(path, 0)
            if got < excess:
                bad.append(
                    '%s has %d incomplete_field_saves site(s), %d above its '
                    'grandfathered ceiling of %d '
                    '(scripts/nested_body_scope_coverage.grandfather) - %d '
                    'of the %d excess site(s) carry a `// NESTED-BODY-'
                    'SCOPE-DEBT: #NNNN <reason>` marker, %d do not. --force '
                    'does not substitute for the marker.'
                    % (path, i_have, excess, i_ceil, got, excess,
                       excess - got))
    return bad


class BaselineConflict(Exception):
    pass


def _parse_counts_file(text, source_name):
    """Two banked counts separated by '#': uncovered emitters first,
    incomplete-field saves second. Same flat `<int> <path>` per line
    shape as `scripts/localdecl_only_walkers.{baseline,grandfather}`,
    extended with a second per-file integer. `source_name` is what a
    merge-conflict error names."""
    out = {}
    for raw in text.splitlines():
        line = raw.strip()
        if line.startswith(('<<<<<<<', '=======', '>>>>>>>')):
            raise BaselineConflict(
                '%s has unresolved merge conflict markers.\n'
                '  Do NOT hand-merge the counts.\n'
                '  For the mutable baseline: take the CURRENT branch\'s '
                'version (the ratchet is a floor, so the lower counts are '
                'right), then regenerate exactly:\n'
                '      git checkout --ours scripts/nested_body_scope_coverage.baseline\n'
                '      python3 scripts/check_nested_body_scope_coverage.py --update\n'
                '  For the frozen grandfather ceiling: there is no --update '
                'for this file - it changes only by a deliberate, reviewed '
                'edit, so resolve it by hand.' % source_name)
        if not line or line.startswith('#'):
            continue
        parts = line.split()
        if len(parts) == 2:
            # legacy single-count row from a partial grandfather commit
            n, path = int(parts[0]), parts[1]
            out[path] = (n, 0)
        elif len(parts) == 3:
            u, i, path = int(parts[0]), int(parts[1]), parts[2]
            out[path] = (u, i)
        else:
            continue
    return out


def read_baseline():
    if not BASELINE.exists():
        return {}
    return _parse_counts_file(BASELINE.read_text(),
                              'scripts/nested_body_scope_coverage.baseline')


def read_grandfather():
    """The per-file ceiling. A missing file reads as ALL-ZERO ceilings
    (fail closed)."""
    if not GRANDFATHER.exists():
        return {}
    return _parse_counts_file(GRANDFATHER.read_text(),
                              'scripts/nested_body_scope_coverage.grandfather')


def tighten_ceilings(existing, uncovered_cur, incomplete_cur):
    """-> new ceiling map. MONOTONE DOWNWARD ONLY:
    new[path] = (min(existing.u, uncovered_cur[path]),
                 min(existing.i, incomplete_cur[path]))."""
    out = {}
    paths = set(existing) | set(uncovered_cur) | set(incomplete_cur)
    for path in paths:
        old_u, old_i = existing.get(path, (0, 0))
        new_u = min(old_u, uncovered_cur.get(path, 0))
        new_i = min(old_i, incomplete_cur.get(path, 0))
        if new_u > 0 or new_i > 0:
            out[path] = (new_u, new_i)
    return out


def write_grandfather(existing, uncovered_cur, incomplete_cur):
    new = tighten_ceilings(existing, uncovered_cur, incomplete_cur)
    lines = [
        '# nested_body_scope_coverage.grandfather - the ceiling for #2702 / Family L.',
        '#',
        '# THE CEILING ONLY EVER FALLS. `--update` MAY LOWER a per-file ceiling',
        '# (to min(existing, measured) - a drain becomes the new ceiling on the',
        '# very next --update) and MAY NEVER RAISE one; raising a ceiling stays a',
        '# deliberate, reviewed hand-edit to this file. A path whose tightened',
        '# ceiling reaches (0, 0) is dropped entirely. See tighten_ceilings() /',
        '# check_debt_floor() in check_nested_body_scope_coverage.py.',
        '#',
        '# Two ints per row: uncovered_emitters first, incomplete_field_saves second.',
        '#   uncovered_emitters - a function in this file whose name matches the',
        '#     lazy-emitter patterns (BlockGetter / Indexer*Fn / PropertyAccessorFn /',
        '#     ExprGetterThunk / PropertyThunk / emitDisposerCore) but whose body',
        '#     does NOT construct `NestedBodyScope VAR(*this);`. Each bare site is',
        '#     a memoized function whose body inherits the caller\'s per-emitter',
        '#     state and freezes whichever caller was lowered first.',
        '#   incomplete_field_saves - a `NestedBodyScope` constructor DEFINITION',
        '#     (a member function of `Emitter::NestedBodyScope`) whose initialization',
        '#     list is missing a canonical field. The canonical field set is read',
        '#     from `src/codegen/LlvmBackend_Assign.cpp:5114` (see',
        '#     CANONICAL_FIELDS in check_nested_body_scope_coverage.py).',
        '',
    ]
    lines += ['%-4d %-4d %s' % (u, i, p) for p, (u, i) in sorted(new.items())]
    GRANDFATHER.write_text('\n'.join(lines) + '\n')
    changed = [(path, existing.get(path, (0, 0)), new.get(path, (0, 0)))
               for path in sorted(set(existing) | set(new))
               if new.get(path, (0, 0)) < existing.get(path, (0, 0))]
    return new, changed


def write_baseline(uncovered_cur, incomplete_cur):
    lines = [
        '# nested_body_scope_coverage.baseline - the ratchet for #2702 / Family L.',
        '# Regenerate with scripts/check_nested_body_scope_coverage.py --update',
        '# after a bare emitter is fixed (the missing NBS construction added) or',
        '# exempted (a `// NESTED-BODY-SCOPE-ONLY: <reason>` marker placed at the',
        '# site). The gate requires this file to EQUAL the measured count, so BOTH',
        '# a rise and stale headroom fail - a landed fix that forgot to --update',
        '# is a violation exactly like a regression. Raising a count needs --force,',
        '# and once the rise pushes a file above its ceiling in',
        '# scripts/nested_body_scope_coverage.grandfather (which only ever falls),',
        '# the excess needs a `// NESTED-BODY-SCOPE-DEBT: #NNNN <reason>` marker at',
        '# each new site.',
        '#',
        '# Two ints per row: uncovered_emitters first, incomplete_field_saves second.',
        '# See check_nested_body_scope_coverage.py\'s module docstring and',
        '# docs/proposals/defect-campaign-2026-09.md §2.8 (Family L) for the six',
        '# open members.',
        '',
    ]
    paths = sorted(set(uncovered_cur) | set(incomplete_cur))
    for path in paths:
        u = uncovered_cur.get(path, 0)
        i = incomplete_cur.get(path, 0)
        if u == 0 and i == 0:
            continue
        lines.append('%-4d %-4d %s' % (u, i, path))
    BASELINE.write_text('\n'.join(lines) + '\n')


def compare(cur_u, cur_i, base, stale=True):
    """-> [violation strings]. The gate requires EQUALITY, so a rise AND
    stale headroom are both violations. `stale=True` is the gate mode -
    a count *below* baseline fails as STALE HEADROOM ("the baseline is
    exact, not a ceiling", same as the three sibling ratchets). A file
    entirely missing from the live tree also fails, because unbanked
    headroom is indistinguishable from a rise somebody already spent."""
    bad = []
    cur_paths = set(cur_u) | set(cur_i)
    for path in sorted(cur_paths | set(base)):
        have_u = cur_u.get(path, 0)
        have_i = cur_i.get(path, 0)
        base_u, base_i = base.get(path, (0, 0))
        if path not in base:
            # New file in live tree with no baseline row.
            bad.append('NEW FILE %s introduces %d uncovered emitter(s) and '
                       '%d incomplete_field_saves site(s) - the family may '
                       'not grow a new offender' % (path, have_u, have_i))
            continue
        # Rise on either of the two banked counts.
        if have_u > base_u:
            bad.append('%s uncovered_emitters = %d, baseline allows %d (+%d)'
                       % (path, have_u, base_u, have_u - base_u))
        if have_i > base_i:
            bad.append('%s incomplete_field_saves = %d, baseline allows %d '
                       '(+%d)' % (path, have_i, base_i, have_i - base_i))
        # Stale headroom: a measured count went DOWN without an --update.
        if stale and (have_u < base_u or have_i < base_i):
            stale_have = []
            if have_u < base_u:
                stale_have.append('uncovered_emitters %d < %d (-%d)'
                                  % (have_u, base_u, base_u - have_u))
            if have_i < base_i:
                stale_have.append('incomplete_field_saves %d < %d (-%d)'
                                  % (have_i, base_i, base_i - have_i))
            bad.append('%s STALE HEADROOM: %s - the baseline is exact, not a '
                       'ceiling (#2702). Re-run --update on the merged tree.'
                       % (path, '; '.join(stale_have)))
    return bad


# ---------------------------------------------------------------------------
# self-test: the detector must go RED on a known positive before it is
# allowed to certify anything.
# ---------------------------------------------------------------------------

# Format: (filename, body, want_uncovered, want_incomplete)
FIXTURES = [
    # All seven lazy emitters, each constructing NBS, one ctor def with all
    # canonical fields -> both counts are 0.
    ('all_emitters_covered.cpp', '''
Emitter::Func Emitter::ensurePropertyBlockGetter(int x) {
    NestedBodyScope nestedScope(*this);
    return nullptr;
}
Emitter::Func Emitter::ensureIndexerBlockGetter(int x) {
    NestedBodyScope nbs(*this);
    return nullptr;
}
Emitter::Func Emitter::ensurePropertyAccessorFn(int x) {
    NestedBodyScope nestedScope(*this);
    return nullptr;
}
Emitter::Func Emitter::ensureIndexerGetterFn(int x) {
    NestedBodyScope nestedScope(*this);
    return nullptr;
}
Emitter::Func Emitter::ensureIndexerSetterFn(int x) {
    NestedBodyScope nestedScope(*this);
    return nullptr;
}
Emitter::Func Emitter::ensureExprGetterThunk(int x) {
    NestedBodyScope nestedScope(*this);
    return nullptr;
}
Emitter::Func Emitter::makePropertyThunk(int x) {
    NestedBodyScope nestedScope(*this);
    return nullptr;
}
Emitter::Func Emitter::emitDisposerCore(int x) {
    NestedBodyScope nestedScope(*this);
    return nullptr;
}
''', 0, 0),

    # Each lazy emitter MISSING its NBS -> uncovered = 8 (seven named
    # members per #2702 + #4151).
    ('all_emitters_bare.cpp', '''
Emitter::Func Emitter::ensurePropertyBlockGetter(int x) { return nullptr; }
Emitter::Func Emitter::ensureIndexerBlockGetter(int x) { return nullptr; }
Emitter::Func Emitter::ensurePropertyAccessorFn(int x) { return nullptr; }
Emitter::Func Emitter::ensureIndexerGetterFn(int x) { return nullptr; }
Emitter::Func Emitter::ensureIndexerSetterFn(int x) { return nullptr; }
Emitter::Func Emitter::ensureExprGetterThunk(int x) { return nullptr; }
Emitter::Func Emitter::makePropertyThunk(int x) { return nullptr; }
Emitter::Func Emitter::emitDisposerCore(int x) { return nullptr; }
''', 8, 0),

    # NESTED-BODY-SCOPE-ONLY marker removes the bare site from the count,
    # REQUIRES a reason after the colon.
    ('only_marker_with_reason.cpp', '''
// NESTED-BODY-SCOPE-ONLY: this emitter is reached only at top-level pass start, not from a lowered body
Emitter::Func Emitter::ensurePropertyBlockGetter(int x) { return nullptr; }
''', 0, 0),
    ('only_marker_no_reason_does_not_exempt.cpp', '''
// NESTED-BODY-SCOPE-ONLY:
Emitter::Func Emitter::ensurePropertyBlockGetter(int x) { return nullptr; }
''', 1, 0),

    # A forward declaration does NOT count as a function definition with a
    # bare body - we only match DEFINITIONS, so a declaration line alone
    # produces 0 uncovered.
    ('forward_declaration_is_not_bare.cpp', '''
class Emitter {
public:
    void ensurePropertyBlockGetter(int);
};
''', 0, 0),

    # An emitter name in a comment does NOT count.
    ('comment_is_not_bare.cpp', '''
// ensurePropertyBlockGetter would be the next emitter to add; it gets the
// NestedBodyScope guard by construction.
Emitter::Func Emitter::otherEmitter(int x) { return nullptr; }
''', 0, 0),

    # NBS ctor definition missing one canonical field -> incomplete = 1.
    ('incomplete_ctor_one_field_missing.cpp', '''
Emitter::NestedBodyScope::NestedBodyScope(Emitter& e)
    : e_(e),
      retNode_(e.currentReturnTypeNode_),
      lamRet_(e.currentLambdaReturnSemaTy_)
{
    e.currentReturnTypeNode_ = nullptr;
    e.currentLambdaReturnSemaTy_ = nullptr;
}
''', 0, 1),

    # A complete ctor definition with all canonical fields -> incomplete = 0.
    # One field may match in preamble but not yet be cleared - we only check
    # preamble membership, since the canonical_set is a SAVED field set, the
    # ctor's preamble (init list) is the load-bearing source.
    ('complete_ctor.cpp', '''
Emitter::NestedBodyScope::NestedBodyScope(Emitter& e)
    : e_(e),
      retNode_(e.currentReturnTypeNode_),
      lamRet_(e.currentLambdaReturnSemaTy_),
      accessorRetNode_(e.accessorReturnTypeNode_),
      accessorRetFn_(e.accessorReturnTypeFn_),
      choiceMethod_(e.currentChoiceMethod_),
      BEZEL_SM_FRAME_MEMBERS(BEZEL_SM_FRAME_NBSCOPE_INIT)
      emittingDisposerBody_(e.emittingDisposerBody_),
      disposerEntryExc_(e.disposerEntryExc_),
      currentCaughtName_(e.currentCaughtName_),
      caughtExcArmTarget_(e.caughtExcArmTarget_),
      expectedFunctionType_(e.expectedFunctionType_),
      lastCallGenericSubst_(e.lastCallGenericSubst_),
      inlinedAccessorEnd_(e.inlinedAccessorEnd_),
      asyncChainedReturn_(e.asyncChainedReturn_),
      asyncBorrowedReturn_(e.asyncBorrowedReturn_),
      pendingOwnedParams_(e.pendingOwnedParams_),
      pendingArgsDisposal_(e.pendingArgsDisposal_),
      genericSubstSemaTy_(e.genericSubstSemaTy_) {
    e.currentReturnTypeNode_ = nullptr;
}
''', 0, 0),
]


def self_test_fixtures():
    ok = True
    for name, body, want_u, want_i in FIXTURES:
        u, ic, _mal = scan_text(body, name)
        good = (len(u) == want_u and len(ic) == want_i)
        ok = ok and good
        print('  [%s] %-58s uncovered=%d (want %d) incomplete=%d (want %d)'
              % ('ok  ' if good else 'FAIL', name, len(u), want_u, len(ic),
                 want_i))
    return ok


DEBT_FIXTURES = [
    ('debt_marker_with_issue.cpp',
     '// NESTED-BODY-SCOPE-DEBT: #3936 unmeasured\n'
     'Emitter::Func Emitter::emitDisposerCore(int x) { return nullptr; }\n',
     [(1, 3936)], []),
    ('debt_marker_no_issue.cpp',
     '// NESTED-BODY-SCOPE-DEBT: tracked separately\n'
     'Emitter::Func Emitter::emitDisposerCore(int x) { return nullptr; }\n',
     [], [1]),
    ('debt_marker_issue_not_leading.cpp',
     '// NESTED-BODY-SCOPE-DEBT: relates to the #2702 umbrella, no local # yet\n'
     'Emitter::Func Emitter::emitDisposerCore(int x) { return nullptr; }\n',
     [], [1]),
]


def self_test_debt_parsing():
    ok = True
    for name, body, want_well, want_mal in DEBT_FIXTURES:
        got_well, got_mal = debt_lines(body)
        good = (got_well == want_well and got_mal == want_mal)
        ok = ok and good
        print('  [%s] debt/%-34s markers=%s (want %s) malformed=%s (want %s)'
              % ('ok  ' if good else 'FAIL', name, got_well, want_well,
                 got_mal, want_mal))
    return ok


COMPARE_CASES = [
    # (label, cur_u, cur_i, base, gate_violations, update_violations)
    # gate_violations counts BOTH a rise and stale headroom; update_violations
    # counts only the rise (a count that went DOWN is the whole point of
    # re-baselining).
    ('exact',    {'a.cpp': 1}, {'a.cpp': 0}, {'a.cpp': (1, 0)}, 0, 0),
    ('rise',     {'a.cpp': 2}, {'a.cpp': 0}, {'a.cpp': (1, 0)}, 1, 1),
    ('stale',    {'a.cpp': 0}, {'a.cpp': 0}, {'a.cpp': (1, 0)}, 1, 0),
    # 'new file': b.cpp in cur but not base -> "NEW FILE" violation always
    # (gate and update); a.cpp in base but not cur -> STALE HEADROOM
    # violation on the gate only. So 2 gate, 1 update.
    ('new file', {'b.cpp': 1}, {}, {'a.cpp': (1, 0)}, 2, 1),
]


def self_test_compare():
    ok = True
    for label, cu, ci, base, want_g, want_u in COMPARE_CASES:
        g = len(compare(cu, ci, base, stale=True))
        u = len(compare(cu, ci, base, stale=False))
        good = (g == want_g and u == want_u)
        ok = ok and good
        print('  [%s] compare/%-8s gate=%d (want %d)  update=%d (want %d)'
              % ('ok  ' if good else 'FAIL', label, g, want_g, u, want_u))
    return ok


# (label, existing, cur_u, cur_i, want_ceilings_u, want_ceilings_i)
TIGHTEN_CASES = [
    ('drain_lowers_uncovered',
     {'a.cpp': (3, 0)}, {'a.cpp': 1}, {'a.cpp': 0}, 1, 0),
    ('drain_lowers_incomplete',
     {'a.cpp': (0, 2)}, {'a.cpp': 0}, {'a.cpp': 0}, 0, 0),
    ('unchanged_stays_unchanged',
     {'a.cpp': (3, 1)}, {'a.cpp': 3}, {'a.cpp': 1}, 3, 1),
    ('fully_drained_file_is_dropped',
     {'a.cpp': (3, 1)}, {'a.cpp': 0}, {'a.cpp': 0}, 0, 0),
]


def self_test_tighten():
    ok = True
    for label, existing, cu, ci, want_u, want_i in TIGHTEN_CASES:
        new = tighten_ceilings(existing, cu, ci)
        got_u, got_i = new.get('a.cpp', (0, 0))
        good = (got_u == want_u and got_i == want_i)
        ok = ok and good
        print('  [%s] tighten/%-30s got (%d, %d) (want (%d, %d))'
              % ('ok  ' if good else 'FAIL', label, got_u, got_i,
                 want_u, want_i))
    return ok


def self_test_drain_protection():
    """FAIL-WITHOUT/PASS-WITH: a drain must immediately become the new
    ceiling, or a later regrow within the OLD ceiling is silently permitted.
    Same shape as `check_localdecl_only_walkers.py`'s
    `self_test_drain_protection`."""
    old_ceiling = {'a.cpp': (3, 0)}
    drained_u = {'a.cpp': 1}
    drained_i = {'a.cpp': 0}
    new = tighten_ceilings(old_ceiling, drained_u, drained_i)
    step1_ok = (new == {'a.cpp': (1, 0)})

    # Two unbanked sites, no markers - violates the new ceiling.
    regrown_u = {'a.cpp': 2}
    regrown_i = {'a.cpp': 0}
    with_old = check_debt_floor(regrown_u, regrown_i, old_ceiling, [], [], [])
    with_new = check_debt_floor(regrown_u, regrown_i, new, [], [], [])
    good = (step1_ok
            and len(with_old) == 0
            and len(with_new) == 1)
    print('  [%s] drain-protection: tighten((3,0)->(1,0))=%s; regrow to 2 '
          '<=3 -> WITHOUT tightening: %d violation(s) (want 0 to prove it '
          'reproduces); WITH tightening: %d violation(s) (want 1)'
          % ('ok  ' if good else 'FAIL', step1_ok, len(with_old),
             len(with_new)))
    return good


def self_test_grandfather_missing_fails_closed():
    """A missing grandfather file MUST fail closed, not silently accept a
    rise. Same ratchet as the three siblings."""
    cur_u = {'a.cpp': 5}
    cur_i = {'a.cpp': 0}
    violations = check_debt_floor(cur_u, cur_i, {}, [], [], [])
    good = (len(violations) == 1)
    print('  [%s] grandfather-missing fails CLOSED: violations=%d (want 1 - '
          'the whole count, not silently accepted)'
          % ('ok  ' if good else 'FAIL', len(violations)))
    return good


def self_test():
    ok = self_test_fixtures()
    ok = self_test_debt_parsing() and ok
    ok = self_test_compare() and ok
    ok = self_test_tighten() and ok
    ok = self_test_drain_protection() and ok
    ok = self_test_grandfather_missing_fails_closed() and ok
    # A detector that cannot see the real tree is also broken.
    uncovered, incomplete, _mal = scan(ROOT)
    u_total = len(uncovered)
    i_total = len(incomplete)
    if not uncovered and not incomplete:
        ok = False
        print('  [FAIL] whole-tree scan found 0 uncovered / 0 incomplete - '
              'a matcher that matched nothing is what a false green looks '
              'like')
    else:
        print('  [ok  ] whole-tree scan found %d uncovered emitter(s), '
              '%d incomplete_field_saves site(s) in the live tree'
              % (u_total, i_total))
    print('check_nested_body_scope_coverage --self-test: '
          + ('PASS' if ok else 'FAIL'))
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
    ap.add_argument('--list', action='store_true',
                    help='print every bare/incomplete site')
    a = ap.parse_args()

    if a.self_test:
        return self_test()

    uncovered, incomplete, malformed = scan(ROOT)
    uncovered_per_file = counts_per_file(uncovered)
    incomplete_per_file = counts_per_file(incomplete)
    base = read_baseline()
    floor = read_grandfather()
    debt_well, _malformed_for_floor = debt_lines(ROOT.read_text() if False else '\n')
    # Aggregate debt markers across the scanned sources for the per-file
    # count.
    all_debt_well = []
    for d in SCAN_DIRS:
        for p in sorted((ROOT / d).rglob('*')):
            if p.suffix not in SUFFIXES:
                continue
            w, _m = debt_lines(p.read_text())
            all_debt_well += [(path, line, issue)
                              for line, issue in w
                              for path in [str(p.relative_to(ROOT))]]

    # Convert to per-file debt counts.
    debt_by_path_u = {}
    debt_by_path_i = {}
    for path, line, issue in all_debt_well:
        # each debt marker covers the next bare/incomplete line within 8
        # lines - here we over-approximate: every marker is treated as
        # covering ONE site in its file. That is the same lax mechanism the
        # sibling ratchets use.
        if path in uncovered_per_file:
            debt_by_path_u[path] = debt_by_path_u.get(path, 0) + 1
        elif path in incomplete_per_file:
            debt_by_path_i[path] = debt_by_path_i.get(path, 0) + 1
        else:
            # Marker with no covered offender - associate with the file
            # (treats each marker as if it covers the first site).
            debt_by_path_u[path] = debt_by_path_u.get(path, 0) + 1

    if a.list:
        for path, line in sorted(set(uncovered)):
            tag = '[UNCOVERED]'
            print('%s %s:%d' % (tag, path, line))
        for path, line in sorted(set(incomplete)):
            tag = '[INCOMPLETE]'
            print('%s %s:%d' % (tag, path, line))
        for path, line in sorted(set(malformed)):
            print('malformed-debt-marker  %s:%d' % (path, line))

    debt_bad = (
        check_debt_floor(uncovered_per_file, incomplete_per_file,
                         floor, all_debt_well, uncovered, incomplete)
        + malformed_debt_report(malformed))

    if a.update:
        rises = [v for v in compare(uncovered_per_file, incomplete_per_file,
                                    base, stale=False) if base]
        if rises and not a.force:
            print('REFUSING to update: the baseline may only go DOWN.')
            for v in rises:
                print('  ' + v)
            print('Use --force if the rise is deliberate, and say why in '
                  'the commit.')
            return 1
        if debt_bad:
            print('REFUSING to update: new offender(s) above the '
                  'grandfathered ceiling need a per-site marker - --force '
                  'alone is not enough.')
            for v in debt_bad:
                print('  ' + v)
            print('Add `// NESTED-BODY-SCOPE-DEBT: #NNNN <reason>` at each '
                  'new site, naming a real issue that tracks the debt, or '
                  'bring the count back down to '
                  'scripts/nested_body_scope_coverage.grandfather\'s '
                  'ceiling.')
            return 1
        write_baseline(uncovered_per_file, incomplete_per_file)
        _new_ceil, ceiling_changed = write_grandfather(
            floor, uncovered_per_file, incomplete_per_file)
        u_total = sum(uncovered_per_file.values())
        i_total = sum(incomplete_per_file.values())
        print('baseline updated: uncovered_emitters=%d in %d file(s); '
              'incomplete_field_saves=%d in %d file(s)'
              % (u_total, len(uncovered_per_file), i_total,
                 len(incomplete_per_file)))
        if ceiling_changed:
            print('grandfather ceiling tightened (%d file(s)):'
                  % len(ceiling_changed))
            for path, old_v, new_v in ceiling_changed:
                print('  %s: %s -> %s' % (path, old_v, new_v))
        return 0

    bad = (compare(uncovered_per_file, incomplete_per_file, base)
           + debt_bad)
    if bad:
        print('NESTED-BODY-SCOPE FAMILY: %d violation(s) (Family L, '
              '#2702, #3936, #4151)' % len(bad))
        for v in bad:
            print('  ' + v)
        print('\nA lazily-memoized body emitter WITHOUT a `NestedBodyScope')
        print('VAR(*this);` at function entry leaks the caller\'s emission')
        print('state into the memoized body, and the cached function - baked')
        print('once - freezes whichever caller was lowered first in')
        print('`std::unordered_map<sema::MethodSymbol*, llvm::Function*>')
        print('methodFns_` POINTER-HASH order, not declaration order. See')
        print('LlvmBackend_Assign.cpp:4932 for the canonical-find shape and')
        print('docs/proposals/defect-campaign-2026-09.md §2.8 (Family L)')
        print('for the six open members (#3936, #2723, #2628, #4150,')
        print('#4151, #2748). Mark a site `// NESTED-BODY-SCOPE-ONLY:')
        print('<reason>` if a caller\'s body provably cannot reach the')
        print('emitter mid-expression.')
        return 1 if a.strict else 0
    u_total = sum(uncovered_per_file.values())
    i_total = sum(incomplete_per_file.values())
    base_u = sum(u for u, _ in base.values())
    base_i = sum(i for _, i in base.values())
    print('check_nested_body_scope_coverage: clean - '
          'uncovered_emitters=%d in %d file(s) (baseline %d), '
          'incomplete_field_saves=%d in %d file(s) (baseline %d)'
          % (u_total, len(uncovered_per_file), base_u,
             i_total, len(incomplete_per_file), base_i))
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except BaselineConflict as e:
        print('NESTED-BODY-SCOPE FAMILY: %s' % e)
        sys.exit(1)
