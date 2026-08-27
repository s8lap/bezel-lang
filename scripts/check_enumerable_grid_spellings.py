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
to raise a count. Raising one is a deliberate act that needs `--force` and an
explanation in the commit message.

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
    """-> (classH_sites, classF_sites); site = (path, line, names)."""
    text = strip_comments(raw)
    found = []
    for first, _last, chunk in statements(text):
        hits = [(m.start(), m.group(1) or m.group(2)) for m in CMP.finditer(chunk)]
        if not hits:
            continue
        line = first + chunk.count('\n', 0, hits[0][0])
        found.append((line, set(n for _, n in hits)))
    exempt = _exempt_statement_lines(marker_lines(raw), [ln for ln, _ in found])
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
            h.append((path, line, sorted(names)))
        if 'OwningBuffer' in names and (names & {'Array', 'OwningArray'}):
            f.append((path, line, sorted(names)))
    return h, f


def scan(root):
    h, f = [], []
    for d in SCAN_DIRS:
        for p in sorted((root / d).rglob('*')):
            if p.suffix not in SUFFIXES:
                continue
            a, b = scan_text(p.read_text(), str(p.relative_to(root)))
            h += a
            f += b
    return h, f


def counts(sites):
    out = {}
    for path, _, _ in sites:
        out[path] = out.get(path, 0) + 1
    return out


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


def _parse_baseline(text):
    h, f, section = {}, {}, 'classH'
    for raw in text.splitlines():
        line = raw.strip()
        # A merge-conflicted baseline used to reach int() and die with an
        # unhandled ValueError - a traceback where the answer is one sentence.
        # It happens on EVERY multi-family merge, because two lanes that both
        # re-baselined always conflict here.
        if line.startswith(('<<<<<<<', '=======', '>>>>>>>')):
            raise BaselineConflict(
                'scripts/enumerable_grid_spellings.baseline has unresolved merge '
                'conflict markers.\n'
                '  Resolve by taking the CURRENT branch\'s version (the ratchet is '
                'a floor, so the lower\n'
                '  counts are the right ones), then re-run with --update to make it '
                'exact:\n'
                '      git checkout --ours scripts/enumerable_grid_spellings.baseline\n'
                '      python3 scripts/check_enumerable_grid_spellings.py --update\n'
                '  Do NOT hand-merge the counts: --update measures the merged tree, '
                'which is the\n'
                '  only source that can be right.')
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
    return _parse_baseline(BASELINE.read_text())


def write_baseline(h, f):
    lines = [
        '# enumerable_grid_spellings.baseline - the ratchet for umbrella #3419.',
        '# Regenerate with scripts/check_enumerable_grid_spellings.py --update after',
        '# a family lands on the shared classifier. The gate requires this file to',
        '# EQUAL the measured count, so BOTH a rise and stale headroom fail - a landed',
        '# family that forgot to --update is a violation exactly like a regression,',
        '# which is what stops a later family from quietly handing back what an',
        '# earlier one ratcheted away. Raising a count needs --force and an',
        '# explanation in the commit message.',
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


def self_test():
    ok = self_test_compare()
    for name, body, wantH, wantF in FIXTURES:
        gotH, gotF = scan_text(body, name)
        good = (len(gotH) == wantH and len(gotF) == wantF)
        ok = ok and good
        print('  [%s] %-38s expected (H=%d,F=%d), got (H=%d,F=%d)'
              % ('ok  ' if good else 'FAIL', name, wantH, wantF,
                 len(gotH), len(gotF)))
    # A detector that cannot see the real tree is also broken.
    h, f = scan(ROOT)
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

    h, f = scan(ROOT)
    curH, curF = counts(h), counts(f)
    baseH, baseF = read_baseline()

    if a.list:
        for path, line, names in h:
            print('classH  %s:%d  %s' % (path, line, ','.join(names)))
        for path, line, names in f:
            print('classF  %s:%d  %s' % (path, line, ','.join(names)))

    if a.update:
        rises = ([v for v in compare('classH', curH, baseH, stale=False) if baseH] +
                 [v for v in compare('classF', curF, baseF, stale=False) if baseF])
        if rises and not a.force:
            print('REFUSING to update: the baseline may only go DOWN.')
            for v in rises:
                print('  ' + v)
            print('Use --force if the rise is deliberate, and say why in the commit.')
            return 1
        write_baseline(curH, curF)
        print('baseline updated: %d class-H spelling(s) in %d file(s), '
              '%d class-F in %d file(s)'
              % (sum(curH.values()), len(curH), sum(curF.values()), len(curF)))
        return 0

    auth_lines, auth_ok = authority_report()
    bad = compare('classH', curH, baseH) + compare('classF', curF, baseF)
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
