#!/usr/bin/env python3
"""check_enumerable_grid_spellings.py - ratchet the enumerable-wrap grid seam.

WHY (umbrella #3419). The §29.2 iterator-format wrap is decided on a GRID:

    declared/target head   x   source family        x   binding form
    (IEnumerable /             (Array, OwningArray,     (plain `var`,
     IOwningEnumerable /        OwningBuffer, Stack,     annotated decl,
     IAsyncEnumerable /         OwningStack, Queue,      `using var`, for-in
     IOwningAsyncEnumerable)    OwningQueue, Set,        source, property read,
                                OwningSet, LinkedList,   indexer read, RETURN,
                                OwningLinkedList, a      argument coercion)
                                lazy pipeline, a user
                                class)

The MINT site allocates a wrapper frame; the CLAIM site frees it. When the two
spell the grid independently they drift, and the drift is silent: a claim that
is NARROWER than the mint leaks, a claim that is WIDER double-frees. Six such
defects landed in the 2026-08-27 run alone (#3376, #3392, #3383-vs-#3364,
#3384, #3418, #3395) and FOUR of the six were found by accident, by a lane
sweeping for something else. Neither sharded oracle can see the class: every
spelling answers correctly for the cells its own tests cover, and wrongly only
for the cells the OTHER spelling covers.

WHAT THIS SCRIPT IS FOR. #3384's fix is the shape that works - the claim site
reads the coercion's own self-report (`iterFormatWrappedDecls_`) instead of
re-deriving the grid, so claim = mint BY CONSTRUCTION. This is the ratchet that
keeps the re-derivations from growing back. A COUNT THAT CAN ONLY GO DOWN is
the cheap structural alternative to finding the seventh spelling by accident.

TWO CLASSES ARE DETECTED. Both are INLINE COMPARISON CHAINS - a `==`/`!=`
against a type-head string literal. A TABLE of the same names is NOT a
spelling: a table is how an authority is written (see
`src/common/BuiltinRegistry.cpp`), and flagging the authority is how a ratchet
teaches people to route around it.

  CLASS H - THE HEAD AXIS (ratcheted, per file).
    One comparison chain naming TWO OR MORE of the eight iterator/enumerator
    interface heads. This is the grid's declared-head axis: "does this head
    take the §29.2 vtable-backed enumerator wrap, and is it the owning form?"
    #3355 widened two wrap-CREATOR gates from `IEnumerable` to
    `IEnumerable || IOwningEnumerable` and left the ownership-CLAIM predicate
    behind; that is #3392, and it is exactly a class-H disagreement.

    The authorities are name-level predicates in `src/common/BuiltinRegistry.hpp`:
    `isIterFormatEnumerableHead` (the four enumerable heads),
    `isSyncIterFormatEnumerableHead` (the two synchronous ones),
    `isIterableInterfaceHead` (all eight), `isOwningIterableInterfaceHead`.

  CLASS F - THE SOURCE AXIS (ratcheted, per file).
    One comparison chain naming `OwningBuffer` together with `Array` or
    `OwningArray` - the §29.2 array-ENUMERATOR-wrap source family
    (`Array`, `OwningArray`, `OwningBuffer`, `Stack`, `OwningStack`, `Queue`,
    `OwningQueue`). Authority: `isArrayEnumeratorWrapSourceName`.

    WHY THE `OwningBuffer` ANCHOR, AND ITS BLIND SPOT. The compiler spells a
    collection-family name list in ~50 places for questions that have nothing
    to do with the wrap (arity, sendability, purity, the §28 borrowing
    families). Ratcheting all of them would be noise, and a noisy ratchet gets
    muted. `OwningBuffer` is the member that distinguishes the wrap-source list
    from those: it is absent from `kHeapPointerReferenceBuiltins`' shared set
    and it is the name #1177 extended the wrap grid with. The cost is a blind
    spot - a NARROWER new copy that omits `OwningBuffer` is not class F.
    CLASS H is what closes it: a grid site must test the head axis too, so a
    new spelling of the grid raises the class-H count even when its family list
    is short. A family list with NO head test is not a grid spelling.

  `common::isArrayBackedCollectionName` is deliberately NOT the class-F
  authority. It answers "is this bz_array_t*-backed" and so also contains
  `Set`, `OwningSet`, `SortedSet` and `FrozenArray` - right for its own
  question, wrong for "does this take the array-enumerator wrap" (a
  hash-backed `Set` is snapshotted, not wrapped; `SortedSet` is deliberately
  excluded because the snapshot is in hash order and would break §4.18's
  ordered-iteration contract). Two different questions over overlapping name
  sets is the reason both predicates must exist and be named for their
  question.

WHEN A FAMILY LANDS. Counts go down; re-baseline with `--update`, which REFUSES
to raise a count. Raising one is a deliberate act that needs `--force`.

⚠ `--force` ALONE IS NOT ENOUGH (#3590 - the twin of the identical gap in
check_imported_symbol_reads.py, ported here because the fix is the same shape).
A rise past what the tree already carried when this mechanism landed needs a
per-site, CHECKABLE marker, not just a sentence in a commit body:

    // GRID-DEBT: #NNNN <reason>

placed like `GRID-AUTHORITY` (attaches to the next spelling within 8 lines).
Unlike `GRID-AUTHORITY`, `GRID-DEBT` does NOT remove the site from the count -
the spelling is still an offender, still baselined, still meant to be
consolidated onto the shared classifier. It only justifies a rise ABOVE
`scripts/enumerable_grid_spellings.grandfather`, a ceiling file recording the
per-file count that already existed when #3590 landed - the tree's existing
class-H / class-F sites need no marker, because they sit at or below their own
file's ceiling. Every spelling a file carries above that ceiling must be
covered by a `GRID-DEBT` marker naming a real issue, and coverage is
recomputed from the LIVE TREE on every `--strict` run, not just at the moment
`--force` was typed. See `check_debt_floor` below.

⚠ THE CEILING ONLY EVER FALLS (#3590 review finding 1, ported from the
identical fix in check_imported_symbol_reads.py). A file's ceiling is not
frozen forever at #3590's launch value - that would protect nothing during a
drain campaign, since a later lane could silently re-grow a drained file back
toward its old high-water mark with a bare `--force` and no marker. EVERY
successful `--update` TIGHTENS the ceiling to `min(existing, measured)`, per
file - never raises one. Raising a ceiling stays a deliberate, reviewed
hand-edit to `scripts/enumerable_grid_spellings.grandfather`. See
`tighten_ceiling` / `write_grandfather` below.

PER-SITE EXEMPTION. A site that IS the authority - the one function that owns
the question - writes the marker at the site, with a reason:

    // GRID-AUTHORITY: <why this spelling is the one everyone else reads>

A marker with no reason after the colon does NOT exempt. Each marker exempts
exactly ONE spelling: the next one at or below it, within eight lines.

⚠ CONTROL-RUN BEFORE TRUSTING A GREEN. `--self-test` runs FIRST in the gate
phase and is not optional. `scripts/check_vacuous_tests.py`'s first draft
reported the tree clean because its regex matched zero tests; a green from a
matcher that matched nothing is worth nothing. The fixtures here include the
multi-LINE spelling of both classes, because that is how most of the real sites
are written and it is exactly what a naive per-line regex misses.

Usage:  scripts/check_enumerable_grid_spellings.py [--strict] [--self-test]
                                                   [--update [--force]] [--list]
"""
import argparse, pathlib, re, sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
BASELINE = ROOT / 'scripts' / 'enumerable_grid_spellings.baseline'
GRANDFATHER = ROOT / 'scripts' / 'enumerable_grid_spellings.grandfather'
SCAN_DIRS = ('src',)
SUFFIXES = ('.cpp', '.hpp')

# The eight §29.3/§30.7 iterator/enumerator interface heads. `kIterableInterfaceHeads`
# in src/common/BuiltinRegistry.cpp is the same set; keep them in step.
IFACE_HEADS = (
    'IEnumerable', 'IEnumerator',
    'IAsyncEnumerable', 'IAsyncEnumerator',
    'IOwningEnumerable', 'IOwningEnumerator',
    'IOwningAsyncEnumerable', 'IOwningAsyncEnumerator',
)
# The §29.2 array-enumerator-wrap source families (`kArrayEnumeratorWrapSources`).
WRAP_SOURCES = (
    'Array', 'OwningArray', 'OwningBuffer',
    'Stack', 'OwningStack', 'Queue', 'OwningQueue',
)
# NAMES REPORTED BUT NEVER CLASSIFIED ON. A chain that also compares against
# these is answering a WIDER question than the wrap grid, and a report that hid
# them would show such a site as an exact copy of the authority's seven names —
# which is exactly the mistake that gets a site "consolidated" when it must not
# be. `LlvmBackend_Exprs.cpp`'s `Capacity` test is the live example: its list is
# the seven wrap sources PLUS `Set`/`OwningSet`, because it answers "does
# `bz_array_capacity` accept this backing", not "does this take the array-
# enumerator wrap". They do not affect either count.
CONTEXT_NAMES = (
    'Set', 'OwningSet', 'SortedSet', 'FrozenArray',
    'LinkedList', 'OwningLinkedList',
    'Dictionary', 'OwningDictionary', 'SortedDictionary',
    'IReadOnlyCollection', 'IReadOnlyList', 'ICollection', 'IList',
    'IOwningCollection', 'IOwningList', 'IGrouping',
)
VOCAB = IFACE_HEADS + WRAP_SOURCES
ALL_NAMES = VOCAB + CONTEXT_NAMES

_ALT = '|'.join(sorted(ALL_NAMES, key=len, reverse=True))
# A COMPARISON against the literal, either order. A table entry (`"Array",`) or a
# call argument (`f("Array")`) is not a classification spelling.
CMP = re.compile(r'(?:[=!]=\s*"(%s)")|(?:"(%s)"\s*[=!]=)' % (_ALT, _ALT))

MARKER = re.compile(r'//\s*GRID-AUTHORITY:\s*\S')

# GRID-DEBT (#3590). A SIBLING marker, not a replacement — see check_debt_floor
# and the module docstring. GRID-AUTHORITY says "this spelling IS the
# authority" and removes it from the count; GRID-DEBT says "this is a known
# offender, deliberately left past its file's grandfathered ceiling" and does
# NOT remove it from the count. The `#NNNN` must sit immediately after the
# colon; a marker with a reason but no issue number is malformed (reported by
# malformed_debt_marker_lines, always a violation) — a marker that cannot be
# checked cannot justify a rise.
GRID_DEBT = re.compile(r'//\s*GRID-DEBT:\s*(\S.*)$')
GRID_DEBT_ISSUE = re.compile(r'^#(\d+)\b')

# The authority predicates this ratchet points every offender at. If one is
# deleted the ratchet would report a lovely green while the seam had no owner at
# all, so their absence is a hard error (the same inversion as the precedent's
# SANCTIONED accessors in check_imported_symbol_reads.py).
AUTHORITY_HEADER = ROOT / 'src' / 'common' / 'BuiltinRegistry.hpp'
REQUIRED_AUTHORITIES = (
    'isIterFormatEnumerableHead',
    'isSyncIterFormatEnumerableHead',
    'isArrayEnumeratorWrapSourceName',
    'isIterableInterfaceHead',
    'isIteratorFormatDispatchHead',
)


def strip_comments(text):
    """Blank out // and /* */ comments, preserving length and newlines.

    A prose MENTION of a spelling is not a spelling. This file's own docstring
    quotes the pattern verbatim in order to explain it, and the compiler's
    comments quote the grid constantly - `LlvmBackend_Setup.cpp`'s #3384 note
    alone names every family in the list. Counting those would make the ratchet
    fire on documentation, which trains readers to route around it.
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


def marker_lines(raw):
    """1-indexed line numbers carrying a GRID-AUTHORITY marker WITH a reason.

    A marker with no reason after the colon does not appear here: an unexplained
    exemption is the escape hatch this tool exists to avoid, and the precedent's
    F6 lane showed how fast a hatch becomes the standard move.
    """
    return [i + 1 for i, ln in enumerate(raw.split('\n')) if MARKER.search(ln)]


def debt_marker_lines(raw):
    """[(line, issue), ...] for WELL-FORMED GRID-DEBT markers (#3590)."""
    out = []
    for i, ln in enumerate(raw.split('\n')):
        m = GRID_DEBT.search(ln)
        if not m:
            continue
        im = GRID_DEBT_ISSUE.match(m.group(1).strip())
        if im:
            out.append((i + 1, int(im.group(1))))
    return out


def malformed_debt_marker_lines(raw):
    """1-indexed line numbers carrying a GRID-DEBT marker with NO `#NNNN`
    right after the colon. Always a violation - see malformed_debt_report."""
    out = []
    for i, ln in enumerate(raw.split('\n')):
        m = GRID_DEBT.search(ln)
        if m and not GRID_DEBT_ISSUE.match(m.group(1).strip()):
            out.append(i + 1)
    return out


def _assign_statement_debt(markers, lit_lines):
    """{line: issue} - maps each (marker_line, issue) to the next unused
    literal-line at or after it, within 8 lines. Mirrors
    `_exempt_statement_lines`' attachment exactly, but RECORDS an issue number
    instead of removing the site from the count."""
    out, used = {}, set()
    for m, issue in markers:
        for ln in lit_lines:
            if ln in used or ln < m or ln > m + 8:
                continue
            out[ln] = issue
            used.add(ln)
            break
    return out


def _exempt_statement_lines(markers, lit_lines):
    """-> the set of statement first-literal lines a marker exempts.

    ATTACHMENT IS TO THE NEXT SPELLING, NOT TO A LINE RANGE. The first version
    of this exempted a line SET (the marker's own line plus the next non-blank
    one) and asked whether the statement's line SPAN intersected it - which
    silently exempted the statement AFTER the intended one too, because a
    statement's span starts on the line where the previous statement's `;` sits.
    The self-test fixture `exempt_marker_does_not_leak` is that bug, pinned: one
    marker must cover exactly one spelling.

    Bounded to 8 lines so a marker cannot drift away from the spelling it
    describes and quietly cover a different one.
    """
    out, used = set(), set()
    for m in markers:
        for ln in lit_lines:
            if ln in used or ln < m or ln > m + 8:
                continue
            out.add(ln)
            used.add(ln)
            break
    return out


def statements(text):
    """-> [(first_line, last_line, chunk)] split on `;`, `{` and `}`.

    A classification spelling is one EXPRESSION - `a == "X" || a == "Y"` - and a
    C++ expression never crosses a `;`/`{`/`}`. Splitting there is what makes a
    MULTI-LINE chain one unit (the common spelling: five of the six sites in
    `LlvmBackend_Setup.cpp` wrap across three lines) while keeping two adjacent
    but independent single-head tests apart.
    """
    out, buf, line, start = [], [], 1, 1
    for ch in text:
        if ch in ';{}':
            out.append((start, line, ''.join(buf)))
            buf, start = [], line
        else:
            buf.append(ch)
        if ch == '\n':
            line += 1
    out.append((start, line, ''.join(buf)))
    return out


def scan_text(raw, path):
    """-> (classH_sites, classF_sites, malformed_debt).

    A site is (path, line, names, debt) - `debt` is the GRID-DEBT issue number
    covering that spelling, or None. `malformed_debt` is [(path, line), ...]
    for every malformed GRID-DEBT marker in this file.
    """
    text = strip_comments(raw)
    found = []
    for first, _last, chunk in statements(text):
        hits = [(m.start(), m.group(1) or m.group(2)) for m in CMP.finditer(chunk)]
        if not hits:
            continue
        line = first + chunk.count('\n', 0, hits[0][0])
        found.append((line, set(n for _, n in hits)))
    lit_lines = [ln for ln, _ in found]
    exempt = _exempt_statement_lines(marker_lines(raw), lit_lines)
    debt = _assign_statement_debt(debt_marker_lines(raw), lit_lines)
    malformed = [(path, ln) for ln in malformed_debt_marker_lines(raw)]
    h, f = [], []
    for line, names in found:
        if line in exempt:
            continue
        # Report the WHOLE matched name set, not just the axis that classified
        # the site. A classF row printing only its wrap-source names reads as an
        # exact seven-name copy of the authority even when the real chain also
        # holds `Set`/`OwningSet` and is answering a different question — which
        # is precisely the mistake that would make a later lane "consolidate" a
        # site it must not touch.
        if len(names & set(IFACE_HEADS)) >= 2:
            h.append((path, line, sorted(names), debt.get(line)))
        if 'OwningBuffer' in names and (names & {'Array', 'OwningArray'}):
            f.append((path, line, sorted(names), debt.get(line)))
    return h, f, malformed


def scan(root):
    h, f, malformed = [], [], []
    for d in SCAN_DIRS:
        for p in sorted((root / d).rglob('*')):
            if p.suffix not in SUFFIXES:
                continue
            a, b, m = scan_text(p.read_text(), str(p.relative_to(root)))
            h += a
            f += b
            malformed += m
    return h, f, malformed


def counts(sites):
    out = {}
    for path, _, _, _ in sites:
        out[path] = out.get(path, 0) + 1
    return out


def debt_marked_paths(sites):
    """{path: N} - how many of `sites` (class-H or class-F tuples) carry a
    WELL-FORMED GRID-DEBT marker. Used by `check_debt_floor`."""
    out = {}
    for path, _, _, issue in sites:
        if issue is not None:
            out[path] = out.get(path, 0) + 1
    return out


def malformed_debt_report(malformed):
    """-> violation strings for every malformed GRID-DEBT marker. Unconditional
    - runs on every invocation, rise or not (#3590)."""
    return ['%s:%d: GRID-DEBT marker has no `#NNNN` right after the colon - a '
            'marker that cannot be checked cannot justify a rise (write e.g. '
            '`// GRID-DEBT: #3600 <reason>`)' % (path, line)
            for path, line in sorted(set(malformed))]


def check_debt_floor(name, cur, floor, sites):
    """-> violation strings. A rise above the GRANDFATHERED ceiling needs a
    per-site GRID-DEBT marker; `--force` alone is not enough (#3590). See the
    identical mechanism (and its rationale) in check_imported_symbol_reads.py.
    """
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
                '%s: %s has %d spelling(s), %d above its grandfathered ceiling '
                'of %d (scripts/enumerable_grid_spellings.grandfather) - %d of '
                'the %d excess spelling(s) carry a `// GRID-DEBT: #NNNN <reason>` '
                'marker, %d do not. --force does not substitute for the marker.'
                % (name, path, have, excess, ceiling, got, excess, excess - got))
    return bad


def authority_report():
    """-> (lines, ok). A missing authority is an error, not a green.

    The inverted check. Every violation message this script prints tells the
    reader to call one of these instead; if one is gone the advice is wrong and
    the seam has no owner, which is precisely the state in which the counts stop
    meaning anything while still reporting clean.
    """
    lines, ok = [], True
    if not AUTHORITY_HEADER.exists():
        return (['  AUTHORITY HEADER %s is missing' %
                 AUTHORITY_HEADER.relative_to(ROOT)], False)
    txt = AUTHORITY_HEADER.read_text()
    for name in REQUIRED_AUTHORITIES:
        if re.search(r'\b%s\s*\(' % re.escape(name), txt):
            lines.append('  authority %-34s declared' % name)
        else:
            ok = False
            lines.append('  AUTHORITY %s is NOT declared in %s - every violation '
                         'message points at it' %
                         (name, AUTHORITY_HEADER.relative_to(ROOT)))
    return lines, ok


class BaselineConflict(Exception):
    pass


def _parse_counts_file(text, source_name):
    """Parse a `# classH` / `# classF`-sectioned `<int> <path>` file. Shared by
    the mutable baseline and the frozen grandfather ceiling (#3590); one parser
    serves both, `source_name` is what a merge-conflict error names."""
    h, f, section = {}, {}, 'classH'
    for raw in text.splitlines():
        line = raw.strip()
        # A merge-conflicted file used to reach int() and die with an
        # unhandled ValueError - a traceback where the answer is one sentence.
        # It happens on EVERY multi-family merge, because two lanes that both
        # re-baselined always conflict here.
        if line.startswith(('<<<<<<<', '=======', '>>>>>>>')):
            raise BaselineConflict(
                '%s has unresolved merge conflict markers.\n'
                '  Do NOT hand-merge the counts.\n'
                '  For the mutable baseline (scripts/enumerable_grid_spellings.\n'
                '  baseline): take the CURRENT branch\'s version (the ratchet is a\n'
                '  floor, so the lower counts are right), then regenerate exactly:\n'
                '      git checkout --ours scripts/enumerable_grid_spellings.baseline\n'
                '      python3 scripts/check_enumerable_grid_spellings.py --update\n'
                '  For the frozen grandfather ceiling (scripts/enumerable_grid_\n'
                '  spellings.grandfather): there is no --update for this file - it\n'
                '  changes only by a deliberate, reviewed edit, so resolve it by hand.'
                % source_name)
        if not line or line.startswith('#'):
            if line.startswith('# classF'):
                section = 'classF'
            elif line.startswith('# classH'):
                section = 'classH'
            continue
        n, path = line.split(None, 1)
        {'classH': h, 'classF': f}[section][path] = int(n)
    return h, f


def read_baseline():
    if not BASELINE.exists():
        return {}, {}
    return _parse_counts_file(BASELINE.read_text(),
                               'scripts/enumerable_grid_spellings.baseline')


def read_grandfather():
    """The per-file ceiling (#3590): class-H/class-F counts already present in
    the tree when the GRID-DEBT mechanism landed, TIGHTENED DOWNWARD by every
    successful `--update` since (see `tighten_ceiling` / `write_grandfather`).
    `--update` may LOWER a ceiling; it may never RAISE one. A missing file
    reads as an ALL-ZERO ceiling (fail closed)."""
    if not GRANDFATHER.exists():
        return {}, {}
    return _parse_counts_file(GRANDFATHER.read_text(),
                               'scripts/enumerable_grid_spellings.grandfather')


def tighten_ceiling(existing, cur):
    """{path: N} - the new grandfather ceiling after a successful `--update`
    (#3590 review finding 1, identical to the twin in
    check_imported_symbol_reads.py). MONOTONE DOWNWARD ONLY:
    `new[path] = min(existing.get(path, 0), cur.get(path, 0))`. Can never
    raise a ceiling; a path whose tightened ceiling reaches 0 is dropped."""
    out = {}
    for path in set(existing) | set(cur):
        new = min(existing.get(path, 0), cur.get(path, 0))
        if new > 0:
            out[path] = new
    return out


def write_grandfather(existingH, existingF, curH, curF):
    """Persist the tightened ceiling (#3590 review finding 1) and report what
    changed as [(cls, path, old, new), ...]. Called on EVERY successful
    `--update`, alongside `write_baseline` - a drain must become the new
    ceiling immediately, or the mechanism protects nothing during a drain
    campaign. Can only ever lower a value already on disk; never raises one.
    """
    newH = tighten_ceiling(existingH, curH)
    newF = tighten_ceiling(existingF, curF)
    lines = [
        '# enumerable_grid_spellings.grandfather - the ceiling for #3590.',
        '#',
        '# THE CEILING ONLY EVER FALLS. `--update` MAY LOWER a per-file ceiling',
        '# (to min(existing, measured) - a drain becomes the new ceiling on the',
        '# very next --update) and MAY NEVER RAISE one; raising a ceiling stays a',
        '# deliberate, reviewed hand-edit to this file. A path whose tightened',
        '# ceiling reaches 0 is dropped entirely. See tighten_ceiling() /',
        '# check_debt_floor() in check_enumerable_grid_spellings.py, and #3590',
        '# (same rationale as scripts/imported_symbol_reads.grandfather).',
        '#',
        '# classH: one comparison chain naming >=2 of the eight iterator/enumerator',
        '#         interface heads - the enumerable-wrap grid\'s DECLARED-HEAD axis.',
        '#         Authority: common::isIterFormatEnumerableHead /',
        '#         isSyncIterFormatEnumerableHead / isIterableInterfaceHead.',
    ]
    lines += ['%-4d %s' % (n, p) for p, n in sorted(newH.items())]
    lines += [
        '',
        '# classF: one comparison chain naming OwningBuffer together with Array or',
        '#         OwningArray - the grid\'s SOURCE-FAMILY axis for the §29.2',
        '#         array-enumerator wrap. Authority:',
        '#         common::isArrayEnumeratorWrapSourceName.',
    ]
    lines += ['%-4d %s' % (n, p) for p, n in sorted(newF.items())]
    GRANDFATHER.write_text('\n'.join(lines) + '\n')
    changed = []
    for cls, existing, new in (('classH', existingH, newH), ('classF', existingF, newF)):
        for path in sorted(set(existing) | set(new)):
            old_v, new_v = existing.get(path, 0), new.get(path, 0)
            if new_v < old_v:
                changed.append((cls, path, old_v, new_v))
    return newH, newF, changed


def write_baseline(h, f):
    lines = [
        '# enumerable_grid_spellings.baseline - the ratchet for umbrella #3419.',
        '# Regenerate with scripts/check_enumerable_grid_spellings.py --update after',
        '# a family lands on the shared classifier. The gate requires this file to',
        '# EQUAL the measured count, so BOTH a rise and stale headroom fail - a landed',
        '# family that forgot to --update is a violation exactly like a regression,',
        '# which is what stops a later family from quietly handing back what an',
        '# earlier one ratcheted away. Raising a count needs --force, and (#3590)',
        '# --force alone is not enough once the rise pushes a file above its',
        '# ceiling in scripts/enumerable_grid_spellings.grandfather (which only',
        '# ever falls - see that file\'s header) - the excess needs a',
        '# `// GRID-DEBT: #NNNN <reason>` marker at each new site.',
        '',
        '# classH: one comparison chain naming >=2 of the eight iterator/enumerator',
        '#         interface heads - the enumerable-wrap grid\'s DECLARED-HEAD axis.',
        '#         Authority: common::isIterFormatEnumerableHead /',
        '#         isSyncIterFormatEnumerableHead / isIterableInterfaceHead.',
    ]
    lines += ['%-4d %s' % (n, p) for p, n in sorted(h.items())]
    lines += [
        '',
        '# classF: one comparison chain naming OwningBuffer together with Array or',
        '#         OwningArray - the grid\'s SOURCE-FAMILY axis for the §29.2',
        '#         array-enumerator wrap. Authority:',
        '#         common::isArrayEnumeratorWrapSourceName.',
    ]
    lines += ['%-4d %s' % (n, p) for p, n in sorted(f.items())]
    BASELINE.write_text('\n'.join(lines) + '\n')


def compare(name, cur, base, stale=True):
    """-> list of violation strings.

    `stale` selects which of the two invariants to enforce.

      stale=True  (the GATE): the baseline must EQUAL the measured count. Both a
                  rise (the seam grew) and stale headroom (the file was
                  regenerated at an older base) are violations.
      stale=False (--update): only a rise or a new file matters. A count that
                  went DOWN is the whole point of re-baselining.

    The split is not cosmetic: the precedent conflated them and `--update` then
    refused to LOWER a count while printing "the baseline may only go DOWN".
    """
    bad = []
    for path in sorted(cur):
        have, want = cur[path], base.get(path)
        if want is None:
            bad.append('%s: NEW FILE %s introduces %d spelling(s) - the grid may '
                       'not grow a new site' % (name, path, have))
        elif have > want:
            bad.append('%s: %s has %d spelling(s), baseline allows %d (+%d)'
                       % (name, path, have, want, have - want))
    for path in sorted(base):
        if not stale:
            continue
        have, want = cur.get(path, 0), base[path]
        if want > have:
            bad.append('%s: %s baseline claims %d spelling(s) but the tree has %d '
                       '- STALE HEADROOM (+%d). The baseline is exact, not a '
                       'ceiling: re-run --update on the merged tree.'
                       % (name, path, want, have, want - have))
    return bad


# ---------------------------------------------------------------------------
# self-test: the detector must go RED on a known positive before it is allowed
# to certify anything. Fixtures cover the multi-line spelling, the negation
# spelling (`!=` chains are how three of the real sites are written), the table
# form that must NOT count, and the prose mention that must NOT count.
# ---------------------------------------------------------------------------
FIXTURES = [
    # --- CLASS H positives -------------------------------------------------
    ('h_single_line.cpp',
     'return n == "IEnumerable" || n == "IOwningEnumerable";\n', 1, 0),
    ('h_multi_line.cpp',
     'bool isIterFmt =\n'
     '    (lt->headName() == "IEnumerable" ||\n'
     '     lt->headName() == "IOwningEnumerable" ||\n'
     '     lt->headName() == "IAsyncEnumerable" ||\n'
     '     lt->headName() == "IOwningAsyncEnumerable");\n', 1, 0),
    ('h_negated_chain.cpp',
     'if (name != "IEnumerable" && name != "IOwningEnumerable")\n'
     '    return false;\n', 1, 0),
    ('h_literal_on_the_left.cpp',
     'if ("IEnumerable" == n || "IOwningEnumerator" == n) return true;\n', 1, 0),
    ('h_enumerator_pair.cpp',
     'return h == "IEnumerator" || h == "IOwningEnumerator";\n', 1, 0),
    # --- CLASS H negatives -------------------------------------------------
    # ONE head is not a grid spelling - it is an ordinary single-name test.
    ('h_single_head.cpp',
     'return rt->headName() == "IEnumerable";\n', 0, 0),
    # A TABLE is how the authority is written; flagging it teaches people to
    # route around the ratchet.
    ('h_table_is_not_a_spelling.cpp',
     'constexpr std::array<std::string_view, 8> kIterableInterfaceHeads = {\n'
     '    "IEnumerable", "IEnumerator",\n'
     '    "IOwningEnumerable", "IOwningEnumerator",\n'
     '};\n', 0, 0),
    # Two heads passed as ARGUMENTS, not compared.
    ('h_call_arguments.cpp',
     'ancestors.push_back(makeBuiltin("IEnumerable", {T}));\n'
     'ancestors.push_back(makeBuiltin("IOwningEnumerable", {T}));\n', 0, 0),
    # Prose - line and block form alike.
    ('h_line_comment.cpp',
     '// keyed on n == "IEnumerable" || n == "IOwningEnumerable" before #3355\n',
     0, 0),
    ('h_block_comment.cpp',
     '/* the gate was `h == "IEnumerable" || h == "IOwningEnumerable"` */\n',
     0, 0),
    # ...but a real chain on the SAME line as a comment still counts, so a
    # comment cannot be used to hide one.
    ('h_code_beside_comment.cpp',
     'return n == "IEnumerable" || n == "IOwningEnumerable";  // see #3419\n',
     1, 0),
    # Two INDEPENDENT single-head statements must not merge into one spelling.
    ('h_two_independent_statements.cpp',
     'if (a == "IEnumerable") return 1;\n'
     'if (b == "IOwningEnumerable") return 2;\n', 0, 0),
    # --- CLASS F -----------------------------------------------------------
    ('f_seven_name_list.cpp',
     'bool actualIsArray =\n'
     '    (an == "Array" || an == "OwningArray" || an == "OwningBuffer" ||\n'
     '     an == "Stack" || an == "OwningStack" ||\n'
     '     an == "Queue" || an == "OwningQueue");\n', 0, 1),
    ('f_owning_only.cpp',
     'return h == "OwningArray" || h == "OwningBuffer";\n', 0, 1),
    # No OwningBuffer -> the documented blind spot; class H is what covers a
    # grid site that spells a short family list.
    ('f_no_owning_buffer.cpp',
     'return h == "Array" || h == "OwningArray" || h == "Stack";\n', 0, 0),
    # OwningBuffer alone, with no Array member, is a different question.
    ('f_owning_buffer_alone.cpp',
     'return h == "OwningBuffer" || h == "OwningSet";\n', 0, 0),
    ('f_table_is_not_a_spelling.cpp',
     'constexpr std::array<std::string_view, 3> k = {\n'
     '    "Array", "OwningArray", "OwningBuffer",\n'
     '};\n', 0, 0),
    # A CONTEXT name widens the report but never a count: this is the
    # `LlvmBackend_Exprs.cpp` `Capacity` shape -- the seven wrap sources plus
    # Set/OwningSet, answering a different question over an overlapping set.
    ('f_context_names_do_not_change_counts.cpp',
     'bool isArrayBacked =\n'
     '    cn == "Array" || cn == "OwningArray" || cn == "OwningBuffer" ||\n'
     '    cn == "Stack" || cn == "OwningStack" ||\n'
     '    cn == "Queue" || cn == "OwningQueue" ||\n'
     '    cn == "Set"   || cn == "OwningSet";\n', 0, 1),
    # A CONTEXT name alone is not a spelling of either axis.
    ('f_context_names_alone.cpp',
     'return n == "Set" || n == "OwningSet" || n == "SortedSet";\n', 0, 0),
    # ...and the richer list interfaces alone are not the head axis, even though
    # two of them appear together (they are NOT iterator-format heads).
    ('h_context_interfaces_alone.cpp',
     'return n == "IReadOnlyList" || n == "IList" || n == "ICollection";\n', 0, 0),
    # A statement that is BOTH axes at once counts once in each class.
    ('hf_both_axes.cpp',
     'bool ok = (d == "IEnumerable" || d == "IOwningEnumerable") &&\n'
     '          (s == "Array" || s == "OwningArray" || s == "OwningBuffer");\n',
     1, 1),
    # --- exemption marker --------------------------------------------------
    ('exempt_marker_above.cpp',
     '// GRID-AUTHORITY: the one head test every consumer reads\n'
     'return n == "IEnumerable" || n == "IOwningEnumerable";\n', 0, 0),
    ('exempt_marker_multiline_reason.cpp',
     '// GRID-AUTHORITY: the one head test every consumer reads, and the\n'
     '// reason deliberately wraps across two lines.\n'
     'return n == "IEnumerable" || n == "IOwningEnumerable";\n', 0, 0),
    ('exempt_marker_same_line.cpp',
     'return n == "IEnumerable" || n == "IOwningEnumerable";'
     '  // GRID-AUTHORITY: ditto\n', 0, 0),
    # A marker with NO reason must NOT exempt - an unexplained exemption is the
    # escape hatch this tool exists to avoid.
    ('exempt_marker_no_reason.cpp',
     '// GRID-AUTHORITY:\n'
     'return n == "IEnumerable" || n == "IOwningEnumerable";\n', 1, 0),
    # ...and it must not leak past the next statement.
    ('exempt_marker_does_not_leak.cpp',
     '// GRID-AUTHORITY: only the next one\n'
     'bool a = n == "IEnumerable" || n == "IOwningEnumerable";\n'
     'bool b = m == "IEnumerable" || m == "IOwningEnumerable";\n', 1, 0),
    # GRID-DEBT (#3590) is NOT GRID-AUTHORITY: it documents a deliberately-taken
    # rise, it does not remove the spelling from the count.
    ('debt_marker_still_counts.cpp',
     '// GRID-DEBT: #3600 tracked, will consolidate with the next family\n'
     'return n == "IEnumerable" || n == "IOwningEnumerable";\n', 1, 0),
]


COMPARE_CASES = [
    # (label, cur, base, gate_violations, update_violations)
    ('exact',    {'a.cpp': 3}, {'a.cpp': 3}, 0, 0),
    ('rise',     {'a.cpp': 4}, {'a.cpp': 3}, 1, 1),
    ('stale',    {'a.cpp': 2}, {'a.cpp': 3}, 1, 0),
    # the gate sees TWO (the new file, plus a.cpp now stale at 0); --update sees
    # only the new file, because staleness is not its business.
    ('new file', {'b.cpp': 1}, {'a.cpp': 3}, 2, 1),
]


def self_test_compare():
    ok = True
    for label, cur, base, want_gate, want_upd in COMPARE_CASES:
        g = len(compare('t', cur, base, stale=True))
        u = len(compare('t', cur, base, stale=False))
        good = (g == want_gate and u == want_upd)
        ok = ok and good
        print('  [%s] compare/%-8s gate=%d (want %d)  update=%d (want %d)'
              % ('ok  ' if good else 'FAIL', label, g, want_gate, u, want_upd))
    return ok


# GRID-DEBT marker parsing (#3590): parallel to the exempt_marker_* FIXTURES
# above for GRID-AUTHORITY, but exercising `debt_marker_lines` /
# `malformed_debt_marker_lines` / `_assign_statement_debt` directly.
DEBT_FIXTURES = [
    # (name, body, want debt_marker_lines() [(line, issue)], want malformed [line])
    ('debt_marker_above.cpp',
     '// GRID-DEBT: #3600 tracked, will consolidate later\n'
     'return n == "IEnumerable" || n == "IOwningEnumerable";\n',
     [(1, 3600)], []),
    ('debt_marker_same_line.cpp',
     'return n == "IEnumerable" || n == "IOwningEnumerable";'
     '  // GRID-DEBT: #3601 tracked\n',
     [(1, 3601)], []),
    ('debt_marker_multiline_reason.cpp',
     '// GRID-DEBT: #3602 this explanation deliberately wraps across two\n'
     '// full lines of prose before the marked spelling follows\n'
     'return n == "IEnumerable" || n == "IOwningEnumerable";\n',
     [(1, 3602)], []),
    # No `#NNNN` right after the colon - malformed, justifies NOTHING.
    ('debt_marker_no_issue.cpp',
     '// GRID-DEBT: needs consolidating eventually\n'
     'return n == "IEnumerable" || n == "IOwningEnumerable";\n',
     [], [1]),
    ('debt_marker_issue_not_leading.cpp',
     '// GRID-DEBT: relates to the #3419 umbrella, no local issue yet\n'
     'return n == "IEnumerable" || n == "IOwningEnumerable";\n',
     [], [1]),
]


def self_test_debt():
    ok = True
    for name, body, want_markers, want_malformed in DEBT_FIXTURES:
        got_markers = debt_marker_lines(body)
        got_malformed = malformed_debt_marker_lines(body)
        good = (got_markers == want_markers and got_malformed == want_malformed)
        ok = ok and good
        print('  [%s] debt/%-30s markers=%s (want %s)  malformed=%s (want %s)'
              % ('ok  ' if good else 'FAIL', name, got_markers, want_markers,
                 got_malformed, want_malformed))
    return ok


# check_debt_floor (#3590): proves the NEW refusal actually fires on a known
# positive AND does not fire when marker coverage is sufficient.
DEBT_FLOOR_CASES = [
    # (label, cur, floor, sites, want_violations)
    ('at ceiling',      {'a.cpp': 3}, {'a.cpp': 3}, [], 0),
    ('under ceiling',   {'a.cpp': 2}, {'a.cpp': 3}, [], 0),
    ('excess, no marker',
     {'a.cpp': 4}, {'a.cpp': 3}, [], 1),
    ('excess, fully marked',
     {'a.cpp': 4}, {'a.cpp': 3}, [('a.cpp', 10, ['x'], 9001)], 0),
    ('excess, partially marked',
     {'a.cpp': 5}, {'a.cpp': 3}, [('a.cpp', 10, ['x'], 9001)], 1),
    ('new file, no ceiling on record',
     {'b.cpp': 1}, {}, [], 1),
    ('new file, marked',
     {'b.cpp': 1}, {}, [('b.cpp', 5, ['y'], 9002)], 0),
]


def self_test_debt_floor():
    ok = True
    for label, cur, floor, sites, want in DEBT_FLOOR_CASES:
        got = len(check_debt_floor('t', cur, floor, sites))
        good = (got == want)
        ok = ok and good
        print('  [%s] debt-floor/%-28s violations=%d (want %d)'
              % ('ok  ' if good else 'FAIL', label, got, want))
    return ok


# tighten_ceiling (#3590 review finding 1): the ceiling must only ever fall.
# Identical shape to the twin in check_imported_symbol_reads.py.
GRANDFATHER_TIGHTEN_CASES = [
    # (label, existing_ceiling, measured, want_new_ceiling)
    ('drain lowers the ceiling',
     {'a.cpp': 28}, {'a.cpp': 20}, {'a.cpp': 20}),
    ('unchanged stays unchanged',
     {'a.cpp': 28}, {'a.cpp': 28}, {'a.cpp': 28}),
    ('marked rise above ceiling does not raise it',
     {'a.cpp': 20}, {'a.cpp': 25}, {'a.cpp': 20}),
    ('new file gets no ceiling for free',
     {}, {'b.cpp': 5}, {}),
    ('fully drained file is dropped',
     {'a.cpp': 28}, {}, {}),
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
    """FAIL-WITHOUT/PASS-WITH for review finding 1 (#3590): a drain must
    immediately become the new ceiling. Identical shape to the twin in
    check_imported_symbol_reads.py."""
    old_ceiling = {'a.cpp': 28}
    drained = {'a.cpp': 20}
    new_ceiling = tighten_ceiling(old_ceiling, drained)
    step1_ok = (new_ceiling == {'a.cpp': 20})

    regrown = {'a.cpp': 25}
    violations_without_fix = len(check_debt_floor('t', regrown, old_ceiling, []))
    violations_with_fix = len(check_debt_floor('t', regrown, new_ceiling, []))

    good = (step1_ok and violations_without_fix == 0 and violations_with_fix == 1)
    print('  [%s] drain-protection: tighten(28->20)=%s ceiling; regrow to 25 '
          '(<=28) -> WITHOUT tightening: %d violation(s) (bug, want 0 to prove '
          'it reproduces); WITH tightening: %d violation(s) (fix, want 1)'
          % ('ok  ' if good else 'FAIL', step1_ok, violations_without_fix,
             violations_with_fix))
    return good


def self_test_grandfather_missing_fails_closed():
    """Finding 3 (#3590 review): a MISSING grandfather file fails CLOSED, not
    open - see the twin in check_imported_symbol_reads.py for the full
    rationale."""
    cur = {'a.cpp': 5}
    violations = check_debt_floor('t', cur, {}, [])
    good = (len(violations) == 1)
    print('  [%s] grandfather-missing fails CLOSED: violations=%d (want 1 - '
          'the whole count, not silently accepted)'
          % ('ok  ' if good else 'FAIL', len(violations)))
    return good


def self_test():
    ok = self_test_compare()
    for name, body, wantH, wantF in FIXTURES:
        gotH, gotF, _malformed = scan_text(body, name)
        good = (len(gotH) == wantH and len(gotF) == wantF)
        ok = ok and good
        print('  [%s] %-38s expected (H=%d,F=%d), got (H=%d,F=%d)'
              % ('ok  ' if good else 'FAIL', name, wantH, wantF,
                 len(gotH), len(gotF)))
    ok = self_test_debt() and ok
    ok = self_test_debt_floor() and ok
    ok = self_test_grandfather_tighten() and ok
    ok = self_test_drain_protection() and ok
    ok = self_test_grandfather_missing_fails_closed() and ok
    # A detector that cannot see the real tree is also broken.
    h, f, _malformed = scan(ROOT)
    if not h or not f:
        ok = False
        print('  [FAIL] whole-tree scan found %d class-H and %d class-F spelling(s)'
              ' - a matcher that matches nothing is what a false green looks like'
              % (len(h), len(f)))
    else:
        print('  [ok  ] whole-tree scan found %d class-H and %d class-F spelling(s)'
              % (len(h), len(f)))
    lines, aok = authority_report()
    for l in lines:
        print(l)
    ok = ok and aok
    print('check_enumerable_grid_spellings --self-test: ' + ('PASS' if ok else 'FAIL'))
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
    ap.add_argument('--list', action='store_true', help='print every spelling')
    a = ap.parse_args()

    if a.self_test:
        return self_test()

    h, f, malformed = scan(ROOT)
    curH, curF = counts(h), counts(f)
    baseH, baseF = read_baseline()
    floorH, floorF = read_grandfather()

    if a.list:
        for path, line, names, debt in h:
            print('classH  %s:%d  %s%s' % (path, line, ','.join(names),
                  '  [GRID-DEBT #%d]' % debt if debt else ''))
        for path, line, names, debt in f:
            print('classF  %s:%d  %s%s' % (path, line, ','.join(names),
                  '  [GRID-DEBT #%d]' % debt if debt else ''))
        for path, line in sorted(set(malformed)):
            print('malformed-debt-marker  %s:%d' % (path, line))

    # A rise above the grandfathered ceiling (#3590) needs a per-site GRID-DEBT
    # marker; checked on EVERY run, not only when --force is passed.
    debt_bad = (check_debt_floor('classH', curH, floorH, h) +
                check_debt_floor('classF', curF, floorF, f) +
                malformed_debt_report(malformed))

    if a.update:
        rises = ([v for v in compare('classH', curH, baseH, stale=False) if baseH] +
                 [v for v in compare('classF', curF, baseF, stale=False) if baseF])
        if rises and not a.force:
            print('REFUSING to update: the baseline may only go DOWN.')
            for v in rises:
                print('  ' + v)
            print('Use --force if the rise is deliberate, and say why in the commit.')
            return 1
        # (#3590) --force gets past the "may only go DOWN" refusal above, but
        # NOT past this one. The only way through is a real
        # `// GRID-DEBT: #NNNN <reason>` marker at each new site.
        if debt_bad:
            print('REFUSING to update: new debt above the grandfathered ceiling '
                  '(#3590) needs a per-site marker - --force alone is not enough.')
            for v in debt_bad:
                print('  ' + v)
            print('Add `// GRID-DEBT: #NNNN <reason>` at each new site, naming a')
            print('real issue that tracks the debt, or bring the count back down')
            print('to scripts/enumerable_grid_spellings.grandfather\'s ceiling.')
            return 1
        write_baseline(curH, curF)
        # (#3590 review finding 1) Tighten the ceiling DOWNWARD to match, on
        # EVERY successful update - see the identical rationale in
        # check_imported_symbol_reads.py. Can only lower a value already on
        # disk; never raises one.
        _newH, _newF, ceiling_changed = write_grandfather(floorH, floorF, curH, curF)
        print('baseline updated: %d class-H spelling(s) in %d file(s), '
              '%d class-F in %d file(s)'
              % (sum(curH.values()), len(curH), sum(curF.values()), len(curF)))
        if ceiling_changed:
            print('grandfather ceiling tightened (%d file(s)):' % len(ceiling_changed))
            for cls, path, old_v, new_v in ceiling_changed:
                print('  %s: %s %d -> %d' % (cls, path, old_v, new_v))
        return 0

    auth_lines, auth_ok = authority_report()
    bad = compare('classH', curH, baseH) + compare('classF', curF, baseF) + debt_bad
    if not auth_ok:
        bad += [l.strip() for l in auth_lines if l.strip().startswith('AUTHORITY')]
    if bad:
        print('ENUMERABLE-WRAP GRID: %d violation(s) (#3419)' % len(bad))
        for v in bad:
            print('  ' + v)
        print('\nA new spelling of the (declared head x source family) grid drifts from')
        print('the one that mints the wrapper frame. A CLAIM narrower than the MINT')
        print('leaks; a claim WIDER than the mint double-frees. Read the fact off the')
        print('shared classifier instead - src/common/BuiltinRegistry.hpp declares')
        print('isIterFormatEnumerableHead / isSyncIterFormatEnumerableHead /')
        print('isArrayEnumeratorWrapSourceName / isIterableInterfaceHead - or, at an')
        print('ownership-CLAIM site, read the wrap-creator\'s own self-report')
        print('(iterFormatWrappedDecls_, per #3384) rather than any name list at all.')
        return 1 if a.strict else 0
    for l in auth_lines:
        print(l)
    print('check_enumerable_grid_spellings: clean - %d class-H spelling(s) in %d '
          'file(s), %d class-F in %d file(s)'
          % (sum(curH.values()), len(curH), sum(curF.values()), len(curF)))
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except BaselineConflict as e:
        print('ENUMERABLE-WRAP GRID: %s' % e)
        sys.exit(1)
