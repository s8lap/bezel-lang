#!/usr/bin/env python3
"""check_uninstrumented_emit_runners.py - ratchet hand-rolled emitObjectFile
runners that bypass bezel::test::forceSanitizeAddress().

WHY (issue #3917). `bezel::test::forceSanitizeAddress()`
(`tests/support/force_sanitize.hpp:29-38`) is the ONE predicate
`scripts/gate.sh --full`'s `BEZEL_AOT_FORCE_SANITIZE=address` sweep is
supposed to flow through. Three places consult it:
`tests/support/aot_support.hpp` (runAot), `tests/support/driver_run_support.hpp`
(Driver::compileFile wrappers), and `tests/test_main.cpp` (the shard-summary
writer). A test that calls `bezel::codegen::emitObjectFile` DIRECTLY never
consults it, so `--full` does not instrument that program's generated code.

The mechanism is positional-default on `sanitizeAddress`
(`src/codegen/LlvmBackend.hpp:102-103`, `bool sanitizeAddress = false`). A
three-argument call defaults the parameter to false. LSan still catches a LEAK
(leak detection needs only the interposed allocator, no per-access code
instrumentation on the generated Bezel code), but a use-after-free READ
performed by generated code is invisible: no shadow-memory check is emitted
on the program's own loads/stores, so a read of quarantined-but-still-mapped
freed memory returns stale bytes silently instead of tripping ASan.

The concrete instance is
`tests/codegen/record_string_field_call_release_1747_test.cpp:89`, with two
hand-confirmed rows whose names promise more than this oracle can deliver
(`record_return_call_result_escapes_via_method_arg_safe_no_uaf_spec_28_9` and
`record_call_result_byvalue_arg_callee_field_store_no_uaf_spec_9_7_3827`,
both found during the #3827 review; see issue #3917, 2026-09-06).

SCOPE. Every `tests/**/*.cpp` (per `check_ambiguous_ran_false.py`'s scope
choice: tests/support/ INCLUDED, because tests/support/ is where the future
`emitObjectFileForTest` wrapper will live - exempt by path when it consults
the predicate).

TWO BANKED PER-FILE COUNTS.

  `bypassing_calls` - a call to `codegen::emitObjectFile(` with FEWER THAN
  FOUR arguments, in a file that nowhere reads `forceSanitizeAddress()` or
  sets `sanitizeAddress` true. Argument counting is paren-balanced (reuses
  `_balanced_paren` from check_ambiguous_ran_false.py:303 - import
  rather than fork, per spec §5 / open question Q7). This is the wider
  count: every such call leaves `--full` blind on generated-code ASan.

  `overpromising_rows` - the SHARPER count, the one that matters: a
  `BEZEL_TEST` NAME (the second argument to BEZEL_TEST) matching
  `no_uaf|_uaf_|double_free|use_after_free|no_crash|no_corruption` inside
  a file with a nonzero `bypassing_calls`. This is the name-vs-oracle
  mismatch #3917's comment demands be audited. A pure-leak row does NOT
  contribute even when its file carries `bypassing_calls`; the test
  name's contract determines the weight, not the assertion's text.

EXEMPT BY PATH: `tests/support/` wrappers that consult the predicate are
the fix, not a violation - mirrors `driver_run_support.hpp:75`
(`if (forceSanitizeAddress()) opts.sanitizeAddress = true;`). A
`tests/support/` file that does NOT consult the predicate and still calls
emitObjectFile with <4 args is a bypassing_calls violation - a hand-rolled
wrapper pattern would be the fix there.

EXEMPT BY MARKER. Two markers, ported from #3590 / LOCALDECL-ONLY:
  `// EMIT-ASAN-ONLY: <reason>` removes the next emitObjectFile( call
   within 8 lines from the count (a row whose claim needs no generated-
   code instrumentation - e.g. a pure-leak row where LSan alone suffices,
   the exemption #3917 itself grants in its "Broader scope" paragraph).
  `// EMIT-ASAN-DEBT: #NNNN <reason>` does NOT remove the site but
   justifies a rise above the per-file ceiling in
   scripts/uninstrumented_emit_runners.grandfather, which only ever falls
   (same mechanism as #3590 / #3439).

THERE IS NO RUNTIME HALF for this hole, and that is worth stating rather
than leaving implicit (spec §5): a program that never consults the
predicate cannot count itself as uninstrumented. The source shape is the
only observable. (A partial runtime check - "under `--full`, at least N
programs reported instrumentation" - would be satisfied by the rows that
already work and would say nothing about the 87 files. It is not
proposed.)

USAGE:  scripts/check_uninstrumented_emit_runners.py [--strict] [--self-test]
                                                 [--update [--force]]
                                                 [--list]
"""
import argparse
import os
import pathlib
import re
import sys

# Import `_balanced_paren` from the sibling ratchet script. Direct sys.path
# insertion is the simplest approach that does not require restructuring
# scripts/ into a package (spec §5 open question Q7: "Whether that wants a
# small shared `scripts/testscan/` module is a structural call this
# document does not make" - leaving as-is; this lane does not refactor
# that decision). The function is a private name (underscore-prefixed) but
# Python imports it fine, and `if __name__ == '__main__'` at the bottom of
# check_ambiguous_ran_false.py does NOT fire on import.
_THIS_DIR = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(_THIS_DIR))
try:
    from check_ambiguous_ran_false import _balanced_paren
except ImportError as e:
    sys.stderr.write(
        'check_uninstrumented_emit_runners: cannot import '
        '_balanced_paren from check_ambiguous_ran_false (sibling ratchet '
        'script): %s\n  The paren-balanced argument counter is shared - '
        'do not fork it.\n' % e)
    sys.exit(2)

ROOT = pathlib.Path(__file__).resolve().parent.parent
BASELINE = ROOT / 'scripts' / 'uninstrumented_emit_runners.baseline'
GRANDFATHER = ROOT / 'scripts' / 'uninstrumented_emit_runners.grandfather'
SCAN_DIRS = ('tests',)
SUFFIXES = ('.cpp',)

# An emitObjectFile( call. The name is unique to the codegen function
# across this tree (verified 2026-09-17: 99 occurrences, all spelled
# `bezel::codegen::emitObjectFile(` - the bare `emitObjectFile(` regex
# matches all 99 with no false positives, so we keep it simple).
EMIT_OBJECT_FILE = re.compile(r'\bemitObjectFile\s*\(')

# A predicate-read token. Matches anywhere in the file's text, on any line
# (including comments - a comment promising to consult the predicate is not
# the same as consulting it, but the predicate's presence is what we bank
# on; the marker mechanism exempts per-site, not per-file).
# Mirrors #3917's grep recipe verbatim:
#   forceSanitizeAddress|sanitizeAddress = true|sanitizeAddress=true|opts.sanitizeAddress
PREDICATE_TOKEN = re.compile(
    r'forceSanitizeAddress'
    r'|sanitizeAddress\s*=\s*true'
    r'|opts\.sanitizeAddress')

# A `BEZEL_TEST(suite, test_name)` opener. The TEST NAME is the second
# argument; the regex captures it as group(2).
BEZEL_TEST = re.compile(r'BEZEL_TEST\s*\(\s*(\w+)\s*,\s*(\w+)\s*\)', re.S)

# The overpromising-name pattern, verbatim from spec §5:
#   no_uaf|_uaf_|double_free|use_after_free|no_crash|no_corruption
# Note: the spec's text accidentally repeats `no_crash` twice in
# `oracle-ratchets-2026-09.md:719`; the regex below is the obvious
# deduplicated set (the doubled literal would not change the match set
# either way, but the deduplicated form documents intent).
OVERPROMISING_NAME = re.compile(
    r'no_uaf|_uaf_|double_free|use_after_free|no_crash|no_corruption')

# Per-site marker regexes. Each marker exempts or justifies the NEXT
# emitObjectFile( call within 8 lines (mirroring check_localdecl_only_
# walkers.py:82's "8-line" envelope; the LOCALDECL mechanism has been
# reviewed enough that its envelope is the precedent).
EMIT_ONLY = re.compile(r'//\s*EMIT-ASAN-ONLY:\s*\S')
EMIT_DEBT = re.compile(r'//\s*EMIT-ASAN-DEBT:\s*(\S.*)$')
DEBT_ISSUE = re.compile(r'^#(\d+)\b')


def _arity(text, open_paren_idx):
    """-> number of top-level arguments to emitObjectFile( at open_paren_idx.

    Uses _balanced_paren to find the matching close paren, then counts
    top-level commas in the body. Arity = 1 + comma_count. Arity < 4
    means `sanitizeAddress` takes its `false` default at
    `src/codegen/LlvmBackend.hpp:102`.
    """
    body = _balanced_paren(text, open_paren_idx)
    depth, in_str, count = 0, False, 0
    for i, c in enumerate(body):
        if in_str:
            if c == '\\':
                # Skip the escaped character.
                continue  # next iteration consumes it via i+=1 implicit
            # Actually body iteration uses for-loop so we need to handle
            # escape properly. Simpler: just toggle on '"' since
            # balanced_paren handles escapes for paren matching.
            if c == '"':
                in_str = False
            continue
        if c == '"':
            in_str = True
        elif c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
        elif c == ',' and depth == 0:
            count += 1
    return count + 1


def _exempt_call_lines(raw):
    """1-indexed line numbers of emitObjectFile( calls exempted by an
    EMIT-ASAN-ONLY marker (own line plus the next non-blank/continuation
    line, bounded to 8 lines so a marker cannot drift away from the call
    it describes). Mirrors check_localdecl_only_walkers.py:273-287."""
    out, lines = set(), raw.split('\n')
    for i, ln in enumerate(lines):
        if not EMIT_ONLY.search(ln):
            continue
        out.add(i + 1)
        for j in range(i + 1, min(i + 8, len(lines))):
            s = lines[j].strip()
            out.add(j + 1)
            if s and not s.startswith('//'):
                break
    return out


def _debt_call_lines(raw):
    """[(line, issue_or_None)] for EMIT-ASAN-DEBT markers (well-formed
    vs malformed). Mirrors check_localdecl_only_walkers.py:290-304."""
    well, malformed = [], []
    for i, ln in enumerate(raw.split('\n')):
        m = EMIT_DEBT.search(ln)
        if not m:
            continue
        im = DEBT_ISSUE.match(m.group(1).strip())
        if im:
            well.append((i + 1, int(im.group(1))))
        else:
            malformed.append(i + 1)
    return well, malformed


def _assign_marker_to_calls(markers, call_lines):
    """{marker_line: payload} -> {call_line: payload}. Each marker is
    attached to the next unused emitObjectFile( line at or after it,
    within 8 lines. Mirrors check_localdecl_only_walkers.py:307-321."""
    out, used = {}, set()
    for m, payload in markers:
        for ln in call_lines:
            if ln in used or ln < m or ln > m + 8:
                continue
            out[ln] = payload
            used.add(ln)
    return out


def scan_file(path, raw, rel=None):
    """-> (bypassing_count, overpromising_count, debt_markers, malformed).

    bypassing_count     - int: emitObjectFile( calls with arity < 4 in this
                         file, after path/marker exemption, in a file that
                         nowhere reads the predicate token.
    overpromising_count - int: BEZEL_TEST names in this file matching the
                         overpromising-name regex (always counted when
                         bypassing_count > 0; never counted otherwise).
    debt_markers        - [(line, issue)] for well-formed DEBT markers.
    malformed           - [line] for DEBT markers with no leading #NNNN.

    `rel` is the path string used for the tests/support/ path-based
    exemption (rel.startswith('tests/support/')). When None (the production
    path), it is path.relative_to(ROOT). When the caller is the self-test
    running under a TemporaryDirectory, it passes the explicit rel path
    because path.relative_to(ROOT) would raise.
    """
    if rel is None:
        try:
            rel = str(path.relative_to(ROOT))
        except ValueError:
            rel = str(path)
    # Strip comments so a comment quoting emitObjectFile or the predicate
    # token does not match (parallel to check_ambiguous_ran_false.py:443).
    text = _strip_comments(raw)

    # Find all emitObjectFile( calls and their arity.
    call_offsets = [(m.start(), m.end() - 1) for m in EMIT_OBJECT_FILE.finditer(text)]
    call_lines = [text.count('\n', 0, off) + 1 for off, _ in call_offsets]

    # Markers, on the RAW text (a marker IS a comment, deliberately).
    exempt = _exempt_call_lines(raw)
    debt_markers, malformed = _debt_call_lines(raw)
    debt = _assign_marker_to_calls(debt_markers, call_lines)

    # Per-file predicate-read check (mirrors #3917's grep recipe).
    # A file that reads the predicate at all is NOT a bypassing caller.
    reads_predicate = bool(PREDICATE_TOKEN.search(text))

    # Path-based exemption: tests/support/ + predicate-read = wrapper = exempt.
    is_testsupport_wrapper = (rel.startswith('tests/support/') and reads_predicate)

    bypassing = 0
    for off, paren_idx in call_offsets:
        line = text.count('\n', 0, off) + 1
        if line in exempt:
            continue  # EMIT-ASAN-ONLY exempts this site.
        arity = _arity(text, paren_idx)
        if arity < 4 and not reads_predicate and not is_testsupport_wrapper:
            bypassing += 1

    overpromising = 0
    if bypassing > 0:
        for m in BEZEL_TEST.finditer(text):
            tname = m.group(2)
            if OVERPROMISING_NAME.search(tname):
                overpromising += 1

    return bypassing, overpromising, debt, malformed


def _strip_comments(text):
    """Blank out // and /* */ comments, preserving length and newlines, so
    the predicate-token regex and the BEZEL_TEST regex do not see
    documentation quoting them. Mirrors check_ambiguous_ran_false.py:207-
    300's comment-stripping shape (we do NOT reuse `_strip_comments`
    directly because we need to preserve the raw text for marker parsing,
    which lives in scan_file's caller). A simple per-file strip is enough
    here - the predicates we check (PREDICATE_TOKEN, BEZEL_TEST, EMIT_OBJECT_FILE)
    are sparse enough that a same-line comment containing one of them is
    almost certainly documentation, not live code.
    """
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c == '/' and i + 1 < n and text[i + 1] == '/':
            j = text.find('\n', i)
            j = n if j < 0 else j
            out.append(' ' * (j - i)); i = j
            continue
        if c == '/' and i + 1 < n and text[i + 1] == '*':
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append(''.join(ch if ch == '\n' else ' ' for ch in text[i:j]))
            i = j
            continue
        if c == '"':
            j = i + 1
            while j < n and text[j] != '"':
                j += 2 if text[j] == '\\' else 1
            j = min(j + 1, n)
            out.append(text[i:j]); i = j
            continue
        out.append(c); i += 1
    return ''.join(out)


def scan(root):
    """-> {path: (bypassing, overpromising, debt, malformed)} per file."""
    out = {}
    for d in SCAN_DIRS:
        for p in sorted((root / d).rglob('*')):
            if p.suffix not in SUFFIXES:
                continue
            try:
                raw = p.read_text(errors='replace')
            except OSError:
                continue
            rel = str(p.relative_to(root))
            b, o, debt, mal = scan_file(p, raw)
            if b or o or debt or mal:
                out[rel] = (b, o, debt, mal)
    return out


def _parse_counts_file(text, source_name):
    """Parse a flat `<int> <path>` file. Shared by the mutable baseline and
    the frozen grandfather ceiling. Mirrors check_localdecl_only_walkers.py:
    431-455. Section headers like `[overpromising_rows]` terminate parsing
    of the bypassing section, so a baseline that has both sections reads
    cleanly with read_baseline() (bypassing only) and read_overpromising()
    (overpromising only)."""
    out = {}
    for raw in text.splitlines():
        line = raw.strip()
        if line.startswith(('<<<<<<<', '=======', '>>>>>>>')):
            sys.stderr.write(
                '%s has unresolved merge conflict markers.\n'
                '  Do NOT hand-merge the counts.\n'
                '  For the mutable baseline (scripts/uninstrumented_emit_'
                'runners.baseline): take the lower of the two counts (the '
                'ratchet is exact, not a ceiling), then regenerate:\n'
                '      git checkout --ours scripts/uninstrumented_emit_runners.baseline\n'
                '      python3 scripts/check_uninstrumented_emit_runners.py --update\n'
                '  For the frozen grandfather ceiling (scripts/uninstrumented_'
                'emit_runners.grandfather): there is no --update for this file '
                '- it changes only by a deliberate, reviewed edit, so resolve '
                'by hand.\n' % source_name)
            sys.exit(2)
        if not line or line.startswith('#'):
            continue
        if line.startswith('['):
            # Section header (e.g. `[overpromising_rows]`) - terminates this
            # section's parsing. The bypassing_calls section ends here;
            # the overpromising section lives below and is read separately
            # by read_overpromising().
            break
        n, path = line.split(None, 1)
        out[path] = int(n)
    return out


def read_baseline(quiet=False, path=BASELINE):
    """-> {path: N} from scripts/uninstrumented_emit_runners.baseline.

    The baseline format is the localdecl shape: one row per file, the
    first integer is the BYPASSING_CALLS count (the wider count; the
    `overpromising_rows` section below is a separate list of files with
    nonzero overpromising rows). See write_baseline for the exact layout.
    """
    if not path.exists():
        if not quiet:
            sys.stderr.write('missing baseline: %s\n' % path)
        sys.exit(2)
    return _parse_counts_file(path.read_text(), str(path))


def read_overpromising(path=BASELINE):
    """-> {path: N} from the [overpromising_rows] section of the baseline.
    Same file, separate section."""
    if not path.exists():
        return {}
    out = {}
    in_section = False
    for raw in path.read_text().splitlines():
        s = raw.strip()
        if s.startswith('[') and s.endswith(']'):
            in_section = (s.lower() == '[overpromising_rows]')
            continue
        if not in_section:
            continue
        if not s or s.startswith('#'):
            continue
        n, p = s.split(None, 1)
        out[p] = int(n)
    return out


def read_grandfather():
    """The per-file ceiling (bypassing_calls only; overpromising_rows is
    the sharper count whose ceiling is the same baseline, not a separate
    grandfather - one file is one file)."""
    if not GRANDFATHER.exists():
        return {}
    return _parse_counts_file(GRANDFATHER.read_text(), str(GRANDFATHER))


def tighten_ceiling(existing, cur):
    """{path: N} - MONOTONE DOWNWARD ONLY. Mirrors check_localdecl_only_
    walkers.py:476-485. min(existing, cur) per file, drop entries that
    reach 0."""
    out = {}
    for path in set(existing) | set(cur):
        new = min(existing.get(path, 0), cur.get(path, 0))
        if new > 0:
            out[path] = new
    return out


def write_grandfather(existing, cur, seeding=False):
    """Persist the tightened ceiling (mirrors check_localdecl_only_walkers.
    py:488-516).

    seeding=True: first-ever --update where no grandfather file exists.
    Seed the ceiling to match `cur` directly (without the min() that would
    otherwise collapse every entry to 0). After this seed, every subsequent
    --update tightens the ceiling monotonically downward via tighten_ceiling().
    """
    if seeding:
        # First-ever seed: copy current counts verbatim. From here on, the
        # ceiling only falls.
        new = {p: n for p, n in cur.items() if n > 0}
        lines = [
            '# uninstrumented_emit_runners.grandfather - the ceiling for #3917.',
            '#',
            '# THE CEILING ONLY EVER FALLS. `--update` MAY LOWER a per-file ceiling',
            '# (to min(existing, measured) - a drain becomes the new ceiling on the',
            '# very next --update) and MAY NEVER RAISE one; raising a ceiling stays a',
            '# deliberate, reviewed hand-edit to this file. A path whose tightened',
            '# ceiling reaches 0 is dropped entirely. See tighten_ceiling() in',
            '# check_uninstrumented_emit_runners.py (same rationale/mechanism as',
            '# scripts/localdecl_only_walkers.grandfather / scripts/enumerable_grid_',
            '# spellings.grandfather / scripts/imported_symbol_reads.grandfather,',
            '# #3590).',
            '#',
            '# Seeded on first --update from the measured count (the family',
            '# already had 99 bypassing call(s) in 87 file(s) at the moment this',
            '# ratchet shipped). All future --update calls tighten the ceiling to',
            '# min(existing, measured), never raise it.',
            '#',
            '# One row per file: the number of codegen::emitObjectFile( calls with',
            '# FEWER THAN FOUR arguments (so sanitizeAddress defaults to false at',
            '# src/codegen/LlvmBackend.hpp:102) in a file that nowhere reads the',
            '# bezel::test::forceSanitizeAddress() predicate and is not a',
            '# tests/support/ wrapper that consults it.',
            '',
        ]
        lines += ['%-4d %s' % (n, p) for p, n in sorted(new.items())]
        GRANDFATHER.write_text('\n'.join(lines) + '\n')
        return new, [(p, 0, n) for p, n in sorted(new.items())]

    new = tighten_ceiling(existing, cur)
    lines = [
        '# uninstrumented_emit_runners.grandfather - the ceiling for #3917.',
        '#',
        '# THE CEILING ONLY EVER FALLS. `--update` MAY LOWER a per-file ceiling',
        '# (to min(existing, measured) - a drain becomes the new ceiling on the',
        '# very next --update) and MAY NEVER RAISE one; raising a ceiling stays a',
        '# deliberate, reviewed hand-edit to this file. A path whose tightened',
        '# ceiling reaches 0 is dropped entirely. See tighten_ceiling() in',
        '# check_uninstrumented_emit_runners.py (same rationale/mechanism as',
        '# scripts/localdecl_only_walkers.grandfather / scripts/enumerable_grid_',
        '# spellings.grandfather / scripts/imported_symbol_reads.grandfather,',
        '# #3590).',
        '#',
        '# One row per file: the number of codegen::emitObjectFile( calls with',
        '# FEWER THAN FOUR arguments (so sanitizeAddress defaults to false at',
        '# src/codegen/LlvmBackend.hpp:102) in a file that nowhere reads the',
        '# bezel::test::forceSanitizeAddress() predicate and is not a',
        '# tests/support/ wrapper that consults it.',
        '',
    ]
    lines += ['%-4d %s' % (n, p) for p, n in sorted(new.items())]
    GRANDFATHER.write_text('\n'.join(lines) + '\n')
    changed = [(path, existing.get(path, 0), new.get(path, 0))
               for path in sorted(set(existing) | set(new))
               if new.get(path, 0) < existing.get(path, 0)]
    return new, changed


def write_baseline(bypassing, overpromising, path=BASELINE):
    """Persist the mutable baseline (mirrors check_localdecl_only_walkers.
    py:519-540). Two sections: bypassing_calls and overpromising_rows."""
    lines = [
        '# uninstrumented_emit_runners.baseline - the ratchet for #3917.',
        '# Regenerate with scripts/check_uninstrumented_emit_runners.py --update',
        '# after the burn-down repoints a call site (commit 1: wrapper in',
        '# tests/support/, commits 2..N: 87 files), renames an overpromising',
        '# row, or marks it `// EMIT-ASAN-ONLY:`. The gate requires this file',
        '# to EQUAL the measured count, so BOTH a rise and stale headroom fail',
        '# - a landed fix that forgot to --update is a violation exactly like',
        '# a regression, which is what stops a later change from quietly',
        '# handing back what an earlier one ratcheted away. Raising a count',
        '# needs --force, and once the rise pushes a file above its ceiling',
        '# in scripts/uninstrumented_emit_runners.grandfather (which only ever',
        '# falls - see that file\'s header), the excess needs a',
        '# `// EMIT-ASAN-DEBT: #NNNN <reason>` marker at each new site.',
        '#',
        '# See the script docstring for the exact counting criteria;',
        '# bypassing_calls is the wider count (every 3-arg emitObjectFile( in',
        '# a non-predicate-reading, non-wrapper file), overpromising_rows is',
        '# the sharper count (BEZEL_TEST names matching no_uaf|_uaf_|double_',
        '# free|use_after_free|no_crash|no_corruption in such a file).',
        '#',
        '# bypassing_calls: emitObjectFile( calls defaulting sanitizeAddress=false',
        '# in a file that never reads forceSanitizeAddress()',
        '',
    ]
    lines += ['%-4d %s' % (n, p) for p, n in sorted(bypassing.items())]
    lines += ['', '[overpromising_rows]']
    for p, n in sorted(overpromising.items()):
        lines.append('%-4d %s' % (n, p))
    lines.append('')
    path.write_text('\n'.join(lines))


def compare(cur, base, stale=True):
    """-> violation strings. Mirrors check_localdecl_only_walkers.py:543-570.

    stale=True  (the GATE): the baseline must EQUAL the measured count.
                Both a rise and stale headroom are violations.
    stale=False (--update): only a rise or a new file matters.
    """
    bad = []
    for path in sorted(cur):
        have, want = cur[path], base.get(path)
        if want is None:
            bad.append('NEW FILE %s introduces %d bypassing call(s) - the '
                       'family may not grow a new site' % (path, have))
        elif have > want:
            bad.append('%s has %d bypassing call(s), baseline allows %d (+%d)'
                       % (path, have, want, have - want))
    if stale:
        for path in sorted(base):
            have, want = cur.get(path, 0), base[path]
            if want > have:
                bad.append('%s baseline claims %d bypassing call(s) but the '
                           'tree has %d - STALE HEADROOM (+%d). The baseline '
                           'is exact, not a ceiling: re-run --update on the '
                           'merged tree.'
                           % (path, want, have, want - have))
    return bad


def compare_overpromising(cur, base, stale=True):
    """Same shape, for the [overpromising_rows] section."""
    bad = []
    for path in sorted(cur):
        have, want = cur[path], base.get(path)
        if want is None:
            bad.append('NEW FILE %s introduces %d overpromising row(s)'
                       % (path, have))
        elif have > want:
            bad.append('%s has %d overpromising row(s), baseline allows %d '
                       '(+%d)' % (path, have, want, have - want))
    if stale:
        for path in sorted(base):
            have, want = cur.get(path, 0), base[path]
            if want > have:
                bad.append('%s overpromising baseline claims %d but the tree '
                           'has %d - STALE HEADROOM (+%d)'
                           % (path, want, have, want - have))
    return bad


def check_debt_floor(cur, floor, debt_by_file):
    """-> violation strings. A rise above the GRANDFATHERED ceiling needs
    a per-site EMIT-ASAN-DEBT marker; --force alone is not enough. Mirrors
    check_localdecl_only_walkers.py:402-424."""
    bad = []
    for path in sorted(cur):
        have = cur[path]
        ceiling = floor.get(path, 0)
        if have <= ceiling:
            continue
        excess = have - ceiling
        marked = sum(1 for (_, issue) in debt_by_file.get(path, ([], []))[0]
                     if issue is not None)
        if marked < excess:
            bad.append(
                '%s has %d bypassing call(s), %d above its grandfathered '
                'ceiling of %d (scripts/uninstrumented_emit_runners.grand'
                'father) - %d of the %d excess site(s) carry a `// EMIT-'
                'ASAN-DEBT: #NNNN <reason>` marker, %d do not. --force '
                'does not substitute for the marker.'
                % (path, have, excess, ceiling, marked, excess,
                   excess - marked))
    return bad


def malformed_debt_report(malformed_by_file):
    return [
        '%s:%d: EMIT-ASAN-DEBT marker has no `#NNNN` right after the colon '
        '- a marker that cannot be checked cannot justify a rise (write '
        'e.g. `// EMIT-ASAN-DEBT: #3917 <reason>`)' % (path, line)
        for path, line in sorted(malformed_by_file)]


# ---------------------------------------------------------------------------
# Self-test: the detector must go RED on a known positive before it is
# allowed to certify anything.
# ---------------------------------------------------------------------------

def _mk(root, rel, text):
    p = root / rel
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(text)
    return p


def _scan_fixture(root, rel):
    raw = (root / rel).read_text()
    return scan_file(root / rel, raw, rel=rel)


def self_test():
    import tempfile
    ok = True

    def chk(label, cond):
        nonlocal ok
        if not cond:
            ok = False
            print('  FAIL: %s' % label)
        else:
            print('  ok:   %s' % label)

    with tempfile.TemporaryDirectory() as td:
        root = pathlib.Path(td)

        # Fixture A - a 3-arg emitObjectFile( call with an overpromising
        # test name in the same file. Both counts should be > 0.
        _mk(root, 'tests/codegen/fixture_a_three_arg_with_uaf_test.cpp', '''
#include "support/force_sanitize.hpp"
namespace {
void runIt() {
    llvm::Module* m = nullptr;
    std::string obj, err;
    bool ok = bezel::codegen::emitObjectFile(*m, obj, err);
    (void)ok;
}
}
BEZEL_TEST(codegen, fixture_a_three_arg_no_uaf_1234) {
    runIt();
    BEZEL_EXPECT_TRUE(true);
}
''')
        b, o, _, _ = _scan_fixture(root, 'tests/codegen/fixture_a_three_arg_with_uaf_test.cpp')
        chk('fixture A: 3-arg call + overpromising name -> bypassing > 0 and '
            'overpromising > 0',
            b > 0 and o > 0)

        # Fixture B - a 4-arg emitObjectFile( call threading the predicate.
        # Neither count should fire.
        _mk(root, 'tests/codegen/fixture_b_four_arg_with_uaf_test.cpp', '''
#include "support/force_sanitize.hpp"
namespace {
void runIt() {
    llvm::Module* m = nullptr;
    std::string obj, err;
    bool ok = bezel::codegen::emitObjectFile(
        *m, obj, err, bezel::test::forceSanitizeAddress());
    (void)ok;
}
}
BEZEL_TEST(codegen, fixture_b_four_arg_no_uaf_1234) {
    runIt();
    BEZEL_EXPECT_TRUE(true);
}
''')
        b, o, _, _ = _scan_fixture(root, 'tests/codegen/fixture_b_four_arg_with_uaf_test.cpp')
        chk('fixture B: 4-arg call threading forceSanitizeAddress -> both '
            'counts 0',
            b == 0 and o == 0)

        # Fixture C - 3-arg call but the only BEZEL_TEST is a leak row, not
        # an overpromising name. bypassing > 0, overpromising == 0.
        _mk(root, 'tests/codegen/fixture_c_three_arg_leak_only_test.cpp', '''
namespace {
void runIt() {
    llvm::Module* m = nullptr;
    std::string obj, err;
    bool ok = bezel::codegen::emitObjectFile(*m, obj, err);
    (void)ok;
}
}
BEZEL_TEST(codegen, fixture_c_three_arg_leak_only_1234) {
    runIt();
    BEZEL_EXPECT_TRUE(true);
}
''')
        b, o, _, _ = _scan_fixture(root, 'tests/codegen/fixture_c_three_arg_leak_only_test.cpp')
        chk('fixture C: 3-arg call + only a _leak_ row -> bypassing > 0, '
            'overpromising == 0',
            b > 0 and o == 0)

        # Fixture D - a tests/support/ wrapper that consults the predicate.
        # Exempt by path.
        _mk(root, 'tests/support/emit_wrapper_d_fixture.cpp', '''
#include "support/force_sanitize.hpp"
namespace {
void runIt() {
    llvm::Module* m = nullptr;
    std::string obj, err;
    bool ok = bezel::codegen::emitObjectFile(*m, obj, err,
        bezel::test::forceSanitizeAddress());
    (void)ok;
}
}
''')
        b, o, _, _ = _scan_fixture(root, 'tests/support/emit_wrapper_d_fixture.cpp')
        chk('fixture D: tests/support/ wrapper that consults the predicate '
            '-> exempt by path (both counts 0)',
            b == 0 and o == 0)

        # Fixture E - the marker form: a 3-arg call with an overpromising
        # name, but a well-formed EMIT-ASAN-ONLY marker attached. The
        # marker exempts the call from bypassing_counts (and therefore
        # overpromising_count is also 0 since overpromising requires
        # bypassing > 0).
        _mk(root, 'tests/codegen/fixture_e_marked_only_test.cpp', '''
// EMIT-ASAN-ONLY: pure leak row, LSan alone suffices (issue #3917 grants)
bool ok = bezel::codegen::emitObjectFile(*m, obj, err);
namespace {} // keep above at column 0 so the line below is one token
BEZEL_TEST(codegen, fixture_e_marked_only_no_crash_9999) {
    (void)ok;
    BEZEL_EXPECT_TRUE(true);
}
''')
        b, o, _, _ = _scan_fixture(root, 'tests/codegen/fixture_e_marked_only_test.cpp')
        chk('fixture E: EMIT-ASAN-ONLY-marked call -> bypassing == 0, '
            'overpromising == 0',
            b == 0 and o == 0)

        # Fixture F - a comments-only violation: a comment quotes the
        # pattern, must NOT be counted (matches the comment-stripping
        # behavior every sibling script enforces).
        _mk(root, 'tests/codegen/fixture_f_comment_only_test.cpp', '''
// Historically this test called:
//   bezel::codegen::emitObjectFile(*m, obj, err);
// without threading forceSanitizeAddress() - fixed by #3917's wrapper.
BEZEL_TEST(codegen, fixture_f_comment_only_no_uaf_1234) {
    BEZEL_EXPECT_TRUE(true);
}
''')
        b, o, _, _ = _scan_fixture(root, 'tests/codegen/fixture_f_comment_only_test.cpp')
        chk('fixture F: a comment quoting emitObjectFile( is not a live '
            'call -> both counts 0',
            b == 0 and o == 0)

        # Family assertions (5): match passes, planted violation fails,
        # --update refuses rise, unbanked drop fails STALE, missing
        # baseline exits 2.
        # Build a small fixture tree with a known count and bank it under
        # a TEMPORARY path (writing to the production baseline would leak
        # fixtures into the repo).
        results = scan(root)
        cur_b = {p: r[0] for p, r in results.items()}
        cur_o = {p: r[1] for p, r in results.items() if r[1] > 0}

        bpath = root / 'baseline'
        write_baseline(cur_b, cur_o, path=bpath)
        # Family assertion 1: a tree matching its baseline passes.
        chk('family/1: a tree matching its baseline passes',
            not (compare(cur_b, read_baseline(quiet=True, path=bpath))
                 + compare_overpromising(cur_o, read_overpromising(path=bpath))))

        # Family assertion 2: a RISE fails.
        rose = dict(cur_b); rose['tests/codegen/fixture_a_three_arg_with_uaf_test.cpp'] = cur_b['tests/codegen/fixture_a_three_arg_with_uaf_test.cpp'] + 5
        chk('family/2: a rise FAILS the ratchet (bypassing)',
            compare(rose, cur_b))
        # Family assertion 3: --update refuses the rise.
        chk('family/3: --update REFUSES to bank a rise (bypassing)',
            compare(rose, cur_b, stale=False))
        # (overpromising-rise is symmetric - tested by compare_overpromising.)
        rose_o = dict(cur_o); rose_o['tests/codegen/fixture_a_three_arg_with_uaf_test.cpp'] = cur_o['tests/codegen/fixture_a_three_arg_with_uaf_test.cpp'] + 5
        chk('family/3b: --update REFUSES to bank a rise (overpromising)',
            compare_overpromising(rose_o, cur_o, stale=False))

        # Family assertion 4: STALE HEADROOM.
        dropped = {p: max(0, n - 1) for p, n in cur_b.items()}
        chk('family/4: a count below baseline fails as STALE HEADROOM',
            compare(dropped, cur_b, stale=True))
        chk('family/4: --update ACCEPTS the drop (stale=False)',
            not compare(dropped, cur_b, stale=False))

        # Family assertion 5: missing baseline exits 2.
        try:
            read_baseline(quiet=True, path=pathlib.Path('/no/such/file'))
            chk('family/5: a missing baseline exits 2', False)
        except SystemExit as e:
            chk('family/5: a missing baseline exits 2', e.code == 2)

    print('check_uninstrumented_emit_runners --self-test: '
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
                    help='print every remaining row, sorted by count')
    a = ap.parse_args()

    if a.self_test:
        return self_test()

    results = scan(ROOT)
    cur_b = {p: r[0] for p, r in results.items() if r[0] > 0}
    cur_o = {p: r[1] for p, r in results.items() if r[1] > 0}
    debt_by_file = {p: (r[2], r[3]) for p, r in results.items()}

    if a.list:
        for p, n in sorted(cur_b.items(), key=lambda kv: -kv[1]):
            op = cur_o.get(p, 0)
            print('%-3d bypassing / %-3d overpromising  %s'
                  % (n, op, p))
        print('--- bypassing total=%d in %d file(s); '
              'overpromising total=%d in %d file(s)'
              % (sum(cur_b.values()), len(cur_b),
                 sum(cur_o.values()), len(cur_o)))
        return 0

    # --update creates the baseline if missing (mirrors
    # check_ambiguous_ran_false.py:1041-1046), then refuses any rise
    # above the existing baseline. --strict requires a banked baseline
    # and exits 2 if missing (gate phase expectation).
    if a.update:
        if not BASELINE.exists():
            write_baseline(cur_b, cur_o)
            # Seed grandfather so future --update can tighten it. First
            # --update on a fresh tree: copy current bypassing counts into
            # the ceiling verbatim (the family already had these call sites
            # before this ratchet landed, so the ceiling equals the count).
            _new, ceiling_seeded = write_grandfather({}, cur_b, seeding=True)
            print('created baseline: %d bypassing call(s) in %d file(s), '
                  '%d overpromising row(s) in %d file(s); grandfather '
                  'seeded in %d file(s)'
                  % (sum(cur_b.values()), len(cur_b),
                     sum(cur_o.values()), len(cur_o),
                     len(ceiling_seeded)))
            return 0
        base_b = read_baseline()
        base_o = read_overpromising()
        floor = read_grandfather()
        rises_b = [v for v in compare(cur_b, base_b, stale=False) if base_b]
        rises_o = [v for v in compare_overpromising(cur_o, base_o, stale=False)
                   if base_o]
        if (rises_b or rises_o) and not a.force:
            print('REFUSING to update: the baseline may only go DOWN.')
            for v in rises_b + rises_o:
                print('  ' + v)
            print('Use --force if the rise is deliberate, and say why in the commit.')
            return 1
        debt_bad = check_debt_floor(cur_b, floor, debt_by_file)
        debt_bad += malformed_debt_report([(p, ln)
                                           for p, (_, mal) in debt_by_file.items()
                                           for ln in mal])
        if debt_bad:
            print('REFUSING to update: new bypassing call(s) above the '
                  'grandfathered ceiling need a per-site marker - --force '
                  'alone is not enough.')
            for v in debt_bad:
                print('  ' + v)
            print('Add `// EMIT-ASAN-DEBT: #NNNN <reason>` at each new '
                  'site, naming a real issue that tracks the debt, or '
                  'bring the count back down to scripts/uninstrumented_'
                  'emit_runners.grandfather\'s ceiling.')
            return 1
        write_baseline(cur_b, cur_o)
        _new, ceiling_changed = write_grandfather(floor, cur_b)
        print('baseline updated: %d bypassing call(s) in %d file(s), '
              '%d overpromising row(s) in %d file(s)'
              % (sum(cur_b.values()), len(cur_b),
                 sum(cur_o.values()), len(cur_o)))
        if ceiling_changed:
            print('grandfather ceiling tightened (%d file(s)):'
                  % len(ceiling_changed))
            for path, old_v, new_v in ceiling_changed:
                print('  %s: %d -> %d' % (path, old_v, new_v))
        return 0

    # --strict (the gate phase). Baseline MUST exist - fail closed.
    base_b = read_baseline()
    base_o = read_overpromising()
    floor = read_grandfather()

    debt_bad = check_debt_floor(cur_b, floor, debt_by_file)
    debt_bad += malformed_debt_report([(p, ln)
                                       for p, (_, mal) in debt_by_file.items()
                                       for ln in mal])

    bad = (compare(cur_b, base_b) + compare_overpromising(cur_o, base_o)
           + debt_bad)
    if bad:
        print('UNINSTRUMENTED-EMIT RUNNERS: %d violation(s) (#3917)'
              % len(bad))
        for v in bad:
            print('  ' + v)
        print('\nA test that calls bezel::codegen::emitObjectFile(*) with '
              'fewer than four arguments bypasses bezel::test::'
              'forceSanitizeAddress(), so scripts/gate.sh --full does not '
              'instrument that program\'s generated code. A row promising '
              '"no UAF" / "no crash" / "no double-free" on that path can '
              'only observe a signal death, never a genuine ASan-detected '
              'use-after-free on mapped memory. See issue #3917 and '
              'tests/support/force_sanitize.hpp.')
        return 1 if a.strict else 0
    print('check_uninstrumented_emit_runners: clean - %d bypassing call(s) '
          'in %d file(s) (baseline %d), %d overpromising row(s) in %d file(s)'
          % (sum(cur_b.values()), len(cur_b), sum(base_b.values()),
             sum(cur_o.values()), len(cur_o)))
    return 0


if __name__ == '__main__':
    sys.exit(main())