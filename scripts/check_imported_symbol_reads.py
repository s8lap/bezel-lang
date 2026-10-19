#!/usr/bin/env python3
"""check_imported_symbol_reads.py - ratchet the imported-symbol classification seam.

WHY (umbrella #3152, audit docs/proposals/papers/imported-symbol-audit-2026-08-13.md).
`ModuleLoader` never assigns `Symbol::decl` for an imported type - there is no
`ast::TypeDecl` to assign, because the importing compilation has no AST for a
dependency's types at all (`Loader::reconstructTypeSymbol` at
ModuleLoader.cpp:750 —
`sema::TypeSymbol* reconstructTypeSymbol(const MetaTypeDecl& td,` —
and `loadMetadataPass1` at :6237). So

    dynamic_cast<const ast::TypeDecl*>(ts->decl)      // null for EVERY import

is not a null-dereference risk - every site guards. It is a SILENT ANSWER
CHANGE: the guard's `false` branch is written as "not a positional record" /
"no `serialized` clause" / "no generic params" / "not borrow-bearing", and each
of those is a substantive claim that happens to be wrong across a module
boundary, with no diagnostic. The audit triaged 165 such sites: 96 UNIFY (the
bugs), 6 NEEDS-AST, 63 ALREADY-CORRECT.

WHAT THIS SCRIPT IS FOR. The audit is a snapshot; this is the ratchet that keeps
it from being re-earned. Five ad-hoc loader side tables already exist
(`fieldTypes_`/`returnTypes_`, `constValues_` #328, `propHasGetter_`,
`propSetterVisibility_` #2415, `propIsAutoStorage_` #2478), each added by its own
bug, each fixing one consumer and none making the next one correct. Adding a
sixth is the option that has already failed five times. A COUNT THAT CAN ONLY GO
DOWN is the cheap structural alternative.

TWO CLASSES ARE DETECTED.

  CLASS 1 - AST-GATED CLASSIFICATION (ratcheted, per file).
    `dynamic_cast<const ast::TypeDecl*>(<expr>->decl)`. Wrong-but-quiet for an
    imported symbol. Per-file counts are baselined; a file may go DOWN freely and
    may never go UP, and a file with no baseline entry may not introduce one.
    Per-file rather than a single total so that landing one family cannot silently
    fund a new read somewhere else.

  CLASS 2 - ALWAYS-NULL CAST (hard error, never baselined above its recorded
    count). A `sema::TypeSymbol*` cast straight to `ast::TypeDecl*`. These are
    unrelated types, so the cast is null on EVERY path, local and imported alike:
    the guarded block is dead code that has never once executed. #3150 is the
    known instance (LlvmBackend_Setup.cpp: #2115's generic-payload substitution
    has therefore never run).

WHEN A FAMILY LANDS. Counts go down; re-baseline with `--update`, which REFUSES to
raise a count. Raising one is a deliberate act that needs `--force`.

⚠ `--force` ALONE IS NOT ENOUGH (#3590). Measured history: the class-3 baseline
rose fifteen days in a row (292 -> 311) after its 2026-08-14 low point, no drain,
every commit a `--force` with a prose explanation nothing re-checks. A rise past
what the tree already carried when this paragraph was added needs a per-site,
CHECKABLE marker, not just a sentence in a commit body:

    // SEAM-DEBT: #NNNN <reason>

placed like `SEAM-ACCESSOR` (own line or the next code line, within 8 lines).
Unlike `SEAM-ACCESSOR`, `SEAM-DEBT` does NOT exempt the read from the count -
the site is still an offender, still baselined, still meant to be drained. It
only justifies a rise ABOVE `scripts/imported_symbol_reads.grandfather`, a
ceiling file recording the per-file count that already existed when #3590
landed - the existing 50 class-1 / 311 class-3 sites need NO marker, because
they are already at or below their own file's ceiling. Every read a file
carries above that ceiling must be covered by a `SEAM-DEBT` marker naming a
real issue, and that coverage is recomputed from the LIVE TREE on every
`--strict` run, not just at the moment `--force` was typed - so a marker
deleted later while the count stays up is caught too, and a malformed marker
(no `#NNNN` right after the colon) is a hard, always-on violation. See
`check_debt_floor` / `malformed_debt_lines` below.

⚠ THE CEILING ONLY EVER FALLS (#3590 review finding 1). A file's ceiling is NOT
frozen forever at #3590's launch value - a ceiling that never moved would
protect nothing during a drain campaign: a lane could take a file from 28 to 20,
and a LATER lane could silently re-add 8 reads with a bare `--update --force`
(no marker, no issue) as long as it stayed under the original 28, which is
exactly the 292 -> 311 regrowth #3590 exists to stop, just restarting from a
lower number. So EVERY successful `--update` also TIGHTENS the ceiling to
`min(existing ceiling, measured count)`, per file - a drain becomes the new
ceiling immediately, and re-adding what was drained is new debt requiring a
fresh `SEAM-DEBT` marker like any other rise. This can only ever LOWER a
ceiling; `--update` never raises one. RAISING a ceiling stays a deliberate,
reviewed hand-edit to `scripts/imported_symbol_reads.grandfather` - never
automatic, never a side effect of `--update`. See `tighten_ceiling` /
`write_grandfather` below.

⚠ CONTROL-RUN BEFORE TRUSTING A GREEN. `--self-test` runs first in the gate phase
and is not optional. `scripts/check_vacuous_tests.py`'s first draft reported the
tree clean because its regex matched zero tests; a green from a matcher that
matched nothing is worth nothing. The fixtures here include the multi-LINE spelling
of the cast for the same reason - it is how most of the real sites are written.

Usage:  scripts/check_imported_symbol_reads.py [--strict] [--self-test]
                                               [--update [--force]] [--list]
"""
import argparse, pathlib, re, sys, tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
BASELINE = ROOT / 'scripts' / 'imported_symbol_reads.baseline'
GRANDFATHER = ROOT / 'scripts' / 'imported_symbol_reads.grandfather'
SCAN_DIRS = ('src',)
SUFFIXES = ('.cpp', '.hpp')

CAST = re.compile(r'dynamic_cast\s*<\s*const\s+ast::TypeDecl\s*\*\s*>\s*\(', re.S)
# CLASS 3 — the MEMBER half of the same seam (#3149). `ModuleLoader` sets
# `MemberSymbol::decl` only where it re-parsed body text for a GENERIC type, so
# for a non-generic imported class EVERY member symbol has `decl == nullptr` and
# `PropertySymbol::decl` is never set at all. These casts therefore answer the
# same silent "no" across a module boundary that class 1 does.
#
# Added 2026-08-14 because three MemberSymbol families landed while the ratchet
# watched only `ast::TypeDecl` — each lane correctly reported "the count will not
# move", which is exactly the state in which a seam regrows unmeasured. The
# TypeDecl half went 152 -> 51 under a ratchet; this half had none.
MEMBER_CAST = re.compile(
    r'dynamic_cast\s*<\s*const\s+ast::'
    r'(?:MethodDecl|PropertyDecl|FieldDecl|ConstructorDecl|EventDecl|ConstDecl'
    r'|OperatorDecl)'
    r'\s*\*\s*>\s*\(', re.S)
# The argument shape that makes it a SYMBOL-side classification read. `.decl` is
# the member on sema::Symbol; anything else (`&d`, `td.get()`, an ast::Node*) is
# an ordinary AST cast and is none of this script's business.
SYMBOL_DECL = re.compile(r'->\s*decl\s*$|\.\s*decl\s*$')
# Class 2: the argument is a TypeSymbol itself, not its `decl`. `->declaration` is
# how a sema::TypeRef names its TypeSymbol.
TYPE_SYMBOL = re.compile(r'->\s*declaration\s*$|\btypeSymbol\s*$|\bts\s*$|\bsym\s*$')


# ---------------------------------------------------------------------------
# SANCTIONED ACCESSORS (#3152). The audit's deliverable was "a checker that flags
# a new AST-only classification read outside a SANCTIONED HELPER" — the first
# implementation had no such concept, and the omission showed up immediately: the
# F6 lane's whole point was to build ONE accessor (src/sema/GenericParams.hpp),
# and that accessor MUST read the AST, because consulting both sources is what it
# is for. With no allowlist the lane had to reach for `--update --force`, i.e. the
# escape hatch built to be hard to use. Every future family would have done the
# same, which trains people around the ratchet — the exact failure mode the
# comment-stripping fix was also about.
#
# So a sanctioned accessor's reads are counted SEPARATELY, and the check inverts
# for them: a sanctioned accessor with ZERO AST reads is an ERROR, because it
# cannot be consulting both sources. The list is deliberately short and each entry
# names the ONE question it answers; adding to it is a visible, reviewable act.
# GRANULARITY IS PER FILE, so only a file that is NOTHING BUT accessors may be
# listed. `SymbolCollection.cpp` is deliberately NOT here even though it holds
# `storageComponentType`: it is a 6-read collection pass, and exempting the file
# would exempt five unrelated reads along with the one that deserves it. One
# legitimate read inside a big file simply stays in the ratchet — a fixed cost
# that never grows, which is all the ratchet asks.
SANCTIONED = {
    'src/sema/GenericParams.hpp':
        'generic parameter names / arity / variance (F6, #3143)',
}


# ---------------------------------------------------------------------------
# PER-SITE EXEMPTION. `SANCTIONED` is FILE-granular, which cannot express the
# common case: a both-sources accessor lives in a large file alongside reads that
# are still offenders. `SymbolCollection.cpp` is the example — 7 class-3 reads, of
# which only some are accessor arms.
#
# A both-sources accessor MUST read the AST; consulting both sources is the whole
# point of it. So the exemption is written AT THE SITE, carries a reason, and is
# reviewable in the diff that introduces it:
#
#     // SEAM-ACCESSOR: <why this read is the sanctioned one>
#     if (auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl)) ...
#
# The marker applies to its own line and the next non-blank one. A marker with no
# reason after the colon does NOT exempt — an unexplained exemption is the escape
# hatch this tool exists to avoid, and the F6 lane's `--update --force` showed how
# fast a hatch becomes the standard move.
MARKER = re.compile(r'//\s*SEAM-ACCESSOR:\s*\S')


# ---------------------------------------------------------------------------
# SEAM-DEBT (#3590). A SIBLING marker, not a replacement: SEAM-ACCESSOR says "this
# read IS the sanctioned one" and removes it from the count; SEAM-DEBT says "this
# read is a known offender, deliberately left in past its file's grandfathered
# ceiling" and does NOT remove it from the count - it only supplies the
# per-site, checkable justification `--force` used to let stand for free. See
# `check_debt_floor` and the module docstring.
#
#     // SEAM-DEBT: #NNNN <reason>
#
# The `#NNNN` must sit immediately after the colon - a marker with a reason but
# no issue number is malformed (reported by `malformed_debt_lines`, always a
# violation, not silently ignored) because prose is exactly what could not be
# checked before and a marker that cannot be checked cannot justify a rise.
SEAM_DEBT = re.compile(r'//\s*SEAM-DEBT:\s*(\S.*)$')
SEAM_DEBT_ISSUE = re.compile(r'^#(\d+)\b')


def debt_lines(raw):
    """1-indexed line -> issue number (int) for a WELL-FORMED SEAM-DEBT marker.

    Mirrors `exempt_lines`' window exactly (own line, for a same-line marker;
    otherwise the next line that is neither blank nor a comment continuation,
    within 8 lines) so the two markers read the same to a maintainer. A
    malformed marker (see `malformed_debt_lines`) contributes nothing here -
    it must not silently justify anything.
    """
    out, lines = {}, raw.split('\n')
    for i, ln in enumerate(lines):
        m = SEAM_DEBT.search(ln)
        if not m:
            continue
        im = SEAM_DEBT_ISSUE.match(m.group(1).strip())
        if not im:
            continue
        issue = int(im.group(1))
        out[i + 1] = issue
        for j in range(i + 1, min(i + 8, len(lines))):
            s = lines[j].strip()
            if not s or s.startswith('//'):
                continue
            out[j + 1] = issue
            break
    return out


def malformed_debt_lines(raw):
    """1-indexed line numbers carrying a SEAM-DEBT marker with NO `#NNNN`
    right after the colon. Always a violation (see `malformed_debt_report`),
    on every run - not just when a rise is in flight - because a marker that
    looks like documentation but cannot be checked is worse than no marker.
    """
    out = []
    for i, ln in enumerate(raw.split('\n')):
        m = SEAM_DEBT.search(ln)
        if m and not SEAM_DEBT_ISSUE.match(m.group(1).strip()):
            out.append(i + 1)
    return out


def exempt_lines(raw):
    """1-indexed line numbers exempted by a SEAM-ACCESSOR marker.

    The marker exempts its own line and the next line that is neither blank nor a
    continuation of the comment. Skipping continuation lines matters: a marker
    whose reason wraps is the NORMAL case — the first draft of this function
    stopped at the comment's second line and exempted nothing, which is a check
    that silently fails open. Bounded to a few lines so a marker cannot drift
    away from the read it describes and quietly cover a different one.
    """
    out, lines = set(), raw.split('\n')
    for i, ln in enumerate(lines):
        if not MARKER.search(ln):
            continue
        out.add(i + 1)
        for j in range(i + 1, min(i + 8, len(lines))):
            s = lines[j].strip()
            if not s or s.startswith('//'):
                out.add(j + 1)   # blank or continuation — keep looking
                continue
            out.add(j + 1)
            break
    return out


def strip_comments(text):
    """Blank out // and /* */ comments, preserving length and newlines.

    A prose MENTION of the cast is not a cast. This file's own docstring and the
    Symbol.hpp accessor comments both quote the pattern verbatim in order to
    explain it, and the audit's own census noted that one of its 159 grep hits
    was a comment. Counting those would make the ratchet fire on documentation,
    which trains readers to route around it — the one outcome that makes a
    ratchet worthless. Offsets are preserved so reported line numbers stay true.
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


def _balanced_arg(text, start):
    """Return the text between the '(' at start-1 and its matching ')'."""
    depth, i = 1, start
    while i < len(text) and depth:
        if text[i] == '(':
            depth += 1
        elif text[i] == ')':
            depth -= 1
        i += 1
    return text[start:i - 1]


def scan_text(raw, path):
    """-> (class1_sites, class2_sites, class3_sites, malformed_debt).

    A site is (path, line, arg, debt) - `debt` is the SEAM-DEBT issue number
    (int) covering that line, or None. `malformed_debt` is [(path, line), ...]
    for every malformed SEAM-DEBT marker in this file, regardless of whether it
    sits over a counted site at all.
    """
    text = strip_comments(raw)
    exempt = exempt_lines(raw)
    debt = debt_lines(raw)
    malformed = [(path, ln) for ln in malformed_debt_lines(raw)]
    c1, c2, c3 = [], [], []
    for m in CAST.finditer(text):
        arg = ' '.join(_balanced_arg(text, m.end()).split())
        line = text.count('\n', 0, m.start()) + 1
        if line in exempt:
            continue
        if SYMBOL_DECL.search(arg):
            c1.append((path, line, arg, debt.get(line)))
        elif TYPE_SYMBOL.search(arg):
            c2.append((path, line, arg, debt.get(line)))
    for m in MEMBER_CAST.finditer(text):
        arg = ' '.join(_balanced_arg(text, m.end()).split())
        line = text.count('\n', 0, m.start()) + 1
        if line in exempt:
            continue
        if SYMBOL_DECL.search(arg):
            c3.append((path, line, arg, debt.get(line)))
    return c1, c2, c3, malformed


def scan(root):
    c1, c2, c3, malformed = [], [], [], []
    for d in SCAN_DIRS:
        for p in sorted((root / d).rglob('*')):
            if p.suffix not in SUFFIXES:
                continue
            a, b, c, m = scan_text(p.read_text(), str(p.relative_to(root)))
            c1 += a
            c2 += b
            c3 += c
            malformed += m
    return c1, c2, c3, malformed


def counts(sites):
    out = {}
    for path, _, _, _ in sites:
        if path in SANCTIONED:
            continue  # counted separately — see SANCTIONED
        out[path] = out.get(path, 0) + 1
    return out


def debt_marked_paths(sites):
    """{path: N} - how many of `sites` (class-1 or class-3 tuples) carry a
    WELL-FORMED SEAM-DEBT marker. Used by `check_debt_floor` to decide whether
    a file's excess over its grandfathered ceiling is justified."""
    out = {}
    for path, _, _, issue in sites:
        if issue is not None:
            out[path] = out.get(path, 0) + 1
    return out


def malformed_debt_report(malformed):
    """-> violation strings for every malformed SEAM-DEBT marker. Unconditional
    - a marker that looks like a justification but cannot be checked is worse
    than no marker, so this runs on every invocation, rise or not."""
    return ['%s:%d: SEAM-DEBT marker has no `#NNNN` right after the colon - a '
            'marker that cannot be checked cannot justify a rise (write e.g. '
            '`// SEAM-DEBT: #3600 <reason>`)' % (path, line)
            for path, line in sorted(set(malformed))]


def check_debt_floor(name, cur, floor, sites):
    """-> violation strings. A rise above the GRANDFATHERED ceiling needs a
    per-site SEAM-DEBT marker; `--force` alone is not enough (#3590).

    `floor` is `scripts/imported_symbol_reads.grandfather` - the per-file count
    that already existed when #3590 landed, frozen and never rewritten by
    `--update`. A file at or below its own ceiling needs zero markers (the
    grandfathering); every read above it needs `excess` many SEAM-DEBT-marked
    sites in `sites` (the live scan, not the committed baseline), so a marker
    removed later while the count stays up is caught on the very next run, not
    just at the moment of the rise.
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
                '%s: %s has %d read(s), %d above its grandfathered ceiling of '
                '%d (scripts/imported_symbol_reads.grandfather) - %d of the %d '
                'excess read(s) carry a `// SEAM-DEBT: #NNNN <reason>` marker, '
                '%d do not. --force does not substitute for the marker.'
                % (name, path, have, excess, ceiling, got, excess, excess - got))
    return bad


def sanctioned_report(sites):
    """-> (lines, ok). A sanctioned accessor with no AST read is an error."""
    have = {}
    for path, _, _, _ in sites:
        if path in SANCTIONED:
            have[path] = have.get(path, 0) + 1
    lines, ok = [], True
    for path, what in sorted(SANCTIONED.items()):
        n = have.get(path, 0)
        if not pathlib.Path(ROOT / path).exists():
            continue  # not in this tree (e.g. reviewing an older ref)
        if n == 0:
            ok = False
            lines.append('  SANCTIONED %s has ZERO AST reads — it cannot be '
                         'consulting both sources (%s)' % (path, what))
        else:
            lines.append('  sanctioned %-38s %d read(s)  [%s]' % (path, n, what))
    return lines, ok


class BaselineConflict(Exception):
    pass


def _parse_counts_file(text, source_name):
    """Parse a `# class1` / `# class2` / `# class3`-sectioned `<int> <path>`
    file. Shared by the mutable baseline and the frozen grandfather ceiling
    (#3590) - the two files have the same shape, so one parser serves both;
    `source_name` is what a merge-conflict error names, so the message points
    at whichever file actually has the conflict markers.
    """
    c1, c2, c3, section = {}, {}, {}, 'class1'
    for raw in text.splitlines():
        line = raw.strip()
        # A merge-conflicted file used to reach int() and die with an
        # unhandled `ValueError: invalid literal for int() with base 10: '<<<<<<<'`
        # — a traceback where the answer is one sentence. It happens on EVERY
        # multi-family merge, because two lanes that both re-baselined always
        # conflict here, so this is the common case, not an edge case.
        if line.startswith(('<<<<<<<', '=======', '>>>>>>>')):
            raise BaselineConflict(
                '%s has unresolved merge conflict markers.\n'
                '  Do NOT hand-merge the counts.\n'
                '  For the mutable baseline (scripts/imported_symbol_reads.baseline):\n'
                '  take the CURRENT branch\'s version (the ratchet is a floor, so the\n'
                '  lower counts are right), then regenerate exactly:\n'
                '      git checkout --ours scripts/imported_symbol_reads.baseline\n'
                '      python3 scripts/check_imported_symbol_reads.py --update\n'
                '  For the frozen grandfather ceiling (scripts/imported_symbol_reads.\n'
                '  grandfather): there is no --update for this file — it changes only\n'
                '  by a deliberate, reviewed edit, so resolve it by hand and re-check\n'
                '  which change was actually intended.' % source_name)
        if not line or line.startswith('#'):
            if line.startswith('# class2'):
                section = 'class2'
            elif line.startswith('# class3'):
                section = 'class3'
            elif line.startswith('# class1'):
                section = 'class1'
            continue
        n, path = line.split(None, 1)
        {'class1': c1, 'class2': c2, 'class3': c3}[section][path] = int(n)
    return c1, c2, c3


def read_baseline():
    if not BASELINE.exists():
        return {}, {}, {}
    return _parse_counts_file(BASELINE.read_text(),
                               'scripts/imported_symbol_reads.baseline')


def read_grandfather():
    """The per-file ceiling (#3590): class-1/class-3 counts already present in
    the tree when the SEAM-DEBT mechanism landed, TIGHTENED DOWNWARD by every
    successful `--update` since (see `tighten_ceiling` / `write_grandfather`).
    `--update` may LOWER a ceiling; it may never RAISE one - raising is a
    deliberate, reviewed hand-edit to this file, never automatic. A missing
    file reads as an ALL-ZERO ceiling (fail closed: absence of the record must
    not silently grant unlimited headroom), not as "no floor enforced"."""
    if not GRANDFATHER.exists():
        return {}, {}, {}
    return _parse_counts_file(GRANDFATHER.read_text(),
                               'scripts/imported_symbol_reads.grandfather')


def tighten_ceiling(existing, cur):
    """{path: N} - the new grandfather ceiling after a successful `--update`
    (#3590 review finding 1). MONOTONE DOWNWARD ONLY:

        new[path] = min(existing.get(path, 0), cur.get(path, 0))

    A drain becomes the new ceiling IMMEDIATELY - re-adding what was drained is
    new debt requiring a fresh `SEAM-DEBT` marker, which is what closes the
    exact regrowth channel #3590 exists to stop (this time starting from
    whatever level the tree is currently drained to, not #3590's launch count).
    A path whose tightened ceiling reaches 0 is DROPPED entirely: nothing left
    to grandfather, so a future site there is new debt like any file that never
    had headroom. `min()` guarantees this can never RAISE a ceiling - a marked
    rise above the existing ceiling leaves `cur > existing`, so `min` picks
    `existing` unchanged; only a measured count AT OR BELOW the existing
    ceiling can move it, and only downward.
    """
    out = {}
    for path in set(existing) | set(cur):
        new = min(existing.get(path, 0), cur.get(path, 0))
        if new > 0:
            out[path] = new
    return out


def write_grandfather(existing1, existing3, cur1, cur3):
    """Persist the tightened ceiling (#3590 review finding 1) and report what
    changed as [(cls, path, old, new), ...]. Called on EVERY successful
    `--update`, right alongside `write_baseline` - a drain must become the new
    ceiling immediately, not only on some later deliberate hand-edit, or the
    mechanism protects nothing during the exact drain campaign it exists to
    protect (Run 9). This function itself can only lower a value already on
    disk (see `tighten_ceiling`); it never raises one.
    """
    new1 = tighten_ceiling(existing1, cur1)
    new3 = tighten_ceiling(existing3, cur3)
    lines = [
        '# imported_symbol_reads.grandfather - the ceiling for #3590.',
        '#',
        '# THE CEILING ONLY EVER FALLS. `--update` MAY LOWER a per-file ceiling',
        '# (to min(existing, measured) - a drain becomes the new ceiling on the',
        '# very next --update) and MAY NEVER RAISE one; raising a ceiling stays a',
        '# deliberate, reviewed hand-edit to this file - never automatic, never a',
        '# side effect of --update. A path whose tightened ceiling reaches 0 is',
        '# dropped entirely: nothing left to grandfather, so a future site there',
        '# is new debt like any file that never had headroom.',
        '#',
        '# WHY THE CEILING MOVES: a ceiling frozen forever at its #3590-launch',
        '# value would protect nothing during a drain campaign - a lane could take',
        '# a file from 28 to 20, and a LATER lane could silently re-add 8 reads',
        '# with a bare `--update --force` (no marker, no issue) as long as it',
        '# stayed under the original 28. That is exactly the 292 -> 311 regrowth',
        '# #3590 exists to stop, just restarting from a lower number. See',
        '# tighten_ceiling() / check_debt_floor() in check_imported_symbol_reads.py.',
        '#',
        '# class1: dynamic_cast<const ast::TypeDecl*>(<sym>->decl) - wrong-but-quiet',
        '#         for an imported symbol.',
    ]
    lines += ['%-4d %s' % (n, p) for p, n in sorted(new1.items())]
    lines += [
        '',
        '# class2: deliberately has NO grandfathered ceiling. The always-null cast',
        '#         (#3150) is dead code on every path, not a genuine imported-symbol',
        '#         seam, so there is no legitimate "debt" reading to grandfather -',
        '#         class-2 is zero-tolerance instead: class2_zero_tolerance() (#3590)',
        '#         refuses ANY class-2 site unconditionally, --force included.',
        '',
        '# class3: dynamic_cast<const ast::{Method,Property,Field,Constructor,',
        '#         Event,Const}Decl*>(<sym>->decl) - the MEMBER half of the seam',
        '#         (#3149).',
    ]
    lines += ['%-4d %s' % (n, p) for p, n in sorted(new3.items())]
    GRANDFATHER.write_text('\n'.join(lines) + '\n')
    changed = []
    for cls, existing, new in (('class1', existing1, new1), ('class3', existing3, new3)):
        for path in sorted(set(existing) | set(new)):
            old_v, new_v = existing.get(path, 0), new.get(path, 0)
            if new_v < old_v:
                changed.append((cls, path, old_v, new_v))
    return new1, new3, changed


def class2_zero_tolerance(cur2):
    """-> violation strings. Class-2 (#3150 - the always-null `TypeSymbol` ->
    `TypeDecl` cast) is a HARD error, unconditionally: it is dead code on
    EVERY path, local or imported, so there is no legitimate reading under
    which a rise is acceptable debt - unlike class-1/class-3's genuine
    quiet-wrong-for-imports seam, class-2 has nothing to grandfather and no
    marker can justify it. `--force` does not apply here at all (#3590 review
    finding 2): this check is unconditional, in both `--update` and `--strict`.
    """
    return ['class2: %s has %d always-null cast(s) (#3150) - class-2 is '
            'ZERO-TOLERANCE, dead code on every path; --force does not apply '
            'to it. Fix the dead code (or, if it turns out to be reachable '
            'after all, that is a different, real bug to report), rather than '
            'baselining it.' % (path, n)
            for path, n in sorted(cur2.items()) if n > 0]


def write_baseline(c1, c2, c3):
    lines = [
        '# imported_symbol_reads.baseline - the ratchet for umbrella #3152.',
        '# Regenerate with scripts/check_imported_symbol_reads.py --update after a',
        '# family lands. The gate requires this file to EQUAL the measured count,',
        '# so BOTH a rise and stale headroom fail - a landed family that forgot to',
        '# --update is a violation exactly like a regression, which is what stops a',
        '# later family from quietly handing back what an earlier one ratcheted away.',
        '# Raising a count needs --force, and (#3590) --force alone is not enough',
        '# once the rise pushes a file above its ceiling in',
        '# scripts/imported_symbol_reads.grandfather (which only ever falls - see',
        '# that file\'s header) - the excess needs a `// SEAM-DEBT: #NNNN <reason>`',
        '# marker at each new site. See',
        '# docs/proposals/papers/imported-symbol-audit-2026-08-13.md for the per-site triage.',
        '',
        '# class1: dynamic_cast<const ast::TypeDecl*>(<sym>->decl) - wrong-but-quiet',
        '#         for an imported symbol.',
    ]
    lines += ['%-4d %s' % (n, p) for p, n in sorted(c1.items())]
    lines += [
        '',
        '# class2: a sema::TypeSymbol* cast straight to ast::TypeDecl* - ALWAYS null,',
        '#         so the guarded block is dead on every path (#3150).',
    ]
    lines += ['%-4d %s' % (n, p) for p, n in sorted(c2.items())]
    lines += [
        '',
        '# class3: dynamic_cast<const ast::{Method,Property,Field,Constructor,',
        '#         Event,Const}Decl*>(<sym>->decl) - the MEMBER half of the seam',
        '#         (#3149). MemberSymbol::decl is null for a non-generic import and',
        '#         PropertySymbol::decl is never set at all, so these answer the same',
        '#         silent "no" across a module boundary.',
    ]
    lines += ['%-4d %s' % (n, p) for p, n in sorted(c3.items())]
    BASELINE.write_text('\n'.join(lines) + '\n')


def compare(name, cur, base, stale=True):
    """-> list of violation strings.

    `stale` selects which of the two invariants to enforce, and the split is not
    cosmetic — conflating them broke `--update` outright (see below).

      stale=True  (the GATE): the baseline must EQUAL the measured count. Both a
                  rise (the seam grew) and stale headroom (the file was
                  regenerated at an older base) are violations.
      stale=False (--update): only a rise or a new file matters. A count that
                  went DOWN is the whole point of re-baselining.

    Why the parameter exists: `--update` reuses this function to decide whether a
    refresh would RAISE anything. When stale headroom became a violation, every
    reduction started reading as a rise, so on a tree where nine files had all
    gone down `--update` printed "REFUSING to update: the baseline may only go
    DOWN" — refusing to lower a count, while saying counts may only be lowered —
    and pointed at `--force`. That is the third instance today of a check
    misbehaving on correct work and steering people to the escape hatch, and this
    time the check was the one added to stop the previous two.
    """
    bad = []
    for path in sorted(cur):
        have, want = cur[path], base.get(path)
        if want is None:
            bad.append('%s: NEW FILE %s introduces %d read(s) - the seam may not '
                       'grow a new site' % (name, path, have))
        elif have > want:
            bad.append('%s: %s has %d read(s), baseline allows %d (+%d)'
                       % (name, path, have, want, have - want))
    # STALE HEADROOM is a violation too (#3152). The baseline is EXACT, not a
    # ceiling: after `--update` every entry equals the measured count, so an entry
    # ABOVE it can only mean the file was regenerated at an older base and is
    # carrying headroom an earlier family already ratcheted away.
    #
    # This is the hole that let family F6 through: its baseline was regenerated
    # before F10 landed and would have restored +5 units across three files it
    # does not even touch. `--update` refuses to RAISE, but that guards only the
    # --update path — a stale file committed on a branch sailed straight past a
    # `have > want` test, which never asks whether `want` itself grew. Requiring
    # equality closes it without needing to know what the previous value was.
    for path in sorted(base):
        if not stale or path in SANCTIONED:
            continue
        have, want = cur.get(path, 0), base[path]
        if want > have:
            bad.append('%s: %s baseline claims %d read(s) but the tree has %d — '
                       'STALE HEADROOM (+%d). The baseline is exact, not a '
                       'ceiling: re-run --update on the merged tree.'
                       % (name, path, want, have, want - have))
    return bad


# ---------------------------------------------------------------------------
# self-test: the detector must go RED on a known positive before it is allowed to
# certify anything. Fixtures cover the multi-line spelling, because that is how
# most real sites are written and it is exactly what a naive regex misses.
# ---------------------------------------------------------------------------
FIXTURES = [
    ('positive_single_line.cpp',
     'auto* td = dynamic_cast<const ast::TypeDecl*>(ts->decl);\n', 1, 0, 0),
    ('positive_multi_line.cpp',
     'const auto* rtd =\n'
     '    dynamic_cast<const ast::TypeDecl*>(\n'
     '        rt->declaration->decl);\n', 1, 0, 0),
    ('positive_wrapped_call.cpp',
     'if (auto* td = dynamic_cast<const ast::TypeDecl*>(\n'
     '        pt->declaration->decl)) {\n', 1, 0, 0),
    ('class2_always_null.cpp',
     'auto* d = dynamic_cast<const ast::TypeDecl*>(rt->declaration);\n', 0, 1, 0),
    ('negative_plain_ast_cast.cpp',
     'auto* td = dynamic_cast<const ast::TypeDecl*>(&d);\n'
     'auto* t2 = dynamic_cast<const ast::TypeDecl*>(child.get());\n', 0, 0, 0),
    ('negative_other_node_type.cpp',
     'auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl);\n', 0, 0, 1),  # now COUNTED
    # A prose mention is not a cast — line and block form alike.
    ('negative_line_comment.cpp',
     '// dynamic_cast<const ast::TypeDecl*>(ts->decl) is wrong for an import\n',
     0, 0, 0),
    ('negative_block_comment.cpp',
     '/* explains dynamic_cast<const ast::TypeDecl*>(ts->decl) at length */\n',
     0, 0, 0),
    # ...but a real cast on the SAME line as a comment still counts, so the
    # exemption cannot be used to hide one.
    ('positive_code_beside_comment.cpp',
     'auto* td = dynamic_cast<const ast::TypeDecl*>(ts->decl);  // see #3152\n',
     1, 0, 0),
    # class 3: a member-kind cast on a symbol's decl (#3149).
    ('positive_member_property.cpp',
     'if (auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl)) {\n', 0, 0, 1),
    ('negative_member_plain_node.cpp',
     'auto* fd = dynamic_cast<const ast::FieldDecl*>(&d);\n', 0, 0, 0),
    # A marker WITH a reason exempts its own line and the next non-blank one.
    ('exempt_marker_above.cpp',
     '// SEAM-ACCESSOR: the both-sources arm; it must read the decl\n'
     'if (auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl)) {\n', 0, 0, 0),
    # A marker whose REASON WRAPS must still reach the read below it — the
    # first draft stopped at the comment's second line and exempted nothing.
    ('exempt_marker_multiline_reason.cpp',
     '// SEAM-ACCESSOR: this is the both-sources accessor and the AST arm\n'
     '// is required, not an offender.\n'
     'if (auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl)) {\n', 0, 0, 0),
    ('exempt_marker_same_line.cpp',
     'auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl);'
     '  // SEAM-ACCESSOR: ditto\n', 0, 0, 0),
    # A marker with NO reason must NOT exempt — an unexplained exemption is the
    # escape hatch this tool exists to avoid.
    ('exempt_marker_no_reason.cpp',
     '// SEAM-ACCESSOR:\n'
     'auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl);\n', 0, 0, 1),
    # ...and it must not leak past the next statement.
    ('exempt_marker_does_not_leak.cpp',
     '// SEAM-ACCESSOR: only the next one\n'
     'auto* a = dynamic_cast<const ast::MethodDecl*>(ms->decl);\n'
     'auto* b = dynamic_cast<const ast::MethodDecl*>(ms2->decl);\n', 0, 0, 1),
    # class 3 now covers the operator half (review nit N3).
    ('positive_operator_decl.cpp',
     'auto* od = dynamic_cast<const ast::OperatorDecl*>(ms->decl);\n', 0, 0, 1),
    # SEAM-DEBT (#3590) is NOT SEAM-ACCESSOR: it documents a deliberately-taken
    # rise, it does not remove the site from the count. A well-formed marker
    # still counts, unlike the exempt_marker_* fixtures above.
    ('debt_marker_still_counts.cpp',
     '// SEAM-DEBT: #3600 tracked, will drain with the next family\n'
     'auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl);\n', 0, 0, 1),
]


COMPARE_CASES = [
    # (label, cur, base, gate_violations, update_violations)
    ('exact',        {'a.cpp': 3}, {'a.cpp': 3}, 0, 0),
    ('rise',         {'a.cpp': 4}, {'a.cpp': 3}, 1, 1),
    ('stale',        {'a.cpp': 2}, {'a.cpp': 3}, 1, 0),
    # gate sees TWO (the new file, plus a.cpp now stale at 0); --update sees
    # only the new file, because staleness is not its business.
    ('new file',     {'b.cpp': 1}, {'a.cpp': 3}, 2, 1),
]


def self_test_compare():
    """The gate and --update must disagree about staleness, and only staleness."""
    ok = True
    for label, cur, base, want_gate, want_upd in COMPARE_CASES:
        g = len(compare('t', cur, base, stale=True))
        u = len(compare('t', cur, base, stale=False))
        good = (g == want_gate and u == want_upd)
        ok = ok and good
        print('  [%s] compare/%-8s gate=%d (want %d)  update=%d (want %d)'
              % ('ok  ' if good else 'FAIL', label, g, want_gate, u, want_upd))
    return ok


# SEAM-DEBT marker parsing (#3590): window + malformed-detection fixtures,
# parallel to the exempt_marker_* FIXTURES above for SEAM-ACCESSOR.
DEBT_FIXTURES = [
    # (name, body, want debt_lines() {line: issue}, want malformed_debt_lines())
    # The marker's own line is recorded too (harmless - a comment-only line
    # never matches a cast), mirroring exempt_lines' own-line-unconditionally
    # behavior exactly.
    ('debt_marker_above.cpp',
     '// SEAM-DEBT: #3600 tracked, will drain in the next family\n'
     'auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl);\n',
     {1: 3600, 2: 3600}, []),
    ('debt_marker_same_line.cpp',
     'auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl);'
     '  // SEAM-DEBT: #3601 tracked\n',
     {1: 3601}, []),
    # A reason that WRAPS must still reach the read below it - same failure
    # mode SEAM-ACCESSOR's exempt_marker_multiline_reason fixture pins.
    ('debt_marker_multiline_reason.cpp',
     '// SEAM-DEBT: #3602 this explanation deliberately wraps across two\n'
     '// full lines of prose before the marked read follows\n'
     'auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl);\n',
     {1: 3602, 3: 3602}, []),
    # No `#NNNN` right after the colon - malformed, justifies NOTHING.
    ('debt_marker_no_issue.cpp',
     '// SEAM-DEBT: needs draining eventually\n'
     'auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl);\n',
     {}, [1]),
    # The issue number must be the FIRST thing after the colon, not merely
    # present somewhere in the reason - "see #3152 for background" is prose
    # about a DIFFERENT issue, not a marker justifying THIS site under #NNNN.
    ('debt_marker_issue_not_leading.cpp',
     '// SEAM-DEBT: relates to the #3152 umbrella, no local issue yet\n'
     'auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl);\n',
     {}, [1]),
    # ...and it must not leak past the next statement.
    ('debt_marker_does_not_leak.cpp',
     '// SEAM-DEBT: #3603 only the next one\n'
     'auto* a = dynamic_cast<const ast::MethodDecl*>(ms->decl);\n'
     'auto* b = dynamic_cast<const ast::MethodDecl*>(ms2->decl);\n',
     {1: 3603, 2: 3603}, []),
]


def self_test_debt():
    ok = True
    for name, body, want_lines, want_malformed in DEBT_FIXTURES:
        got_lines = debt_lines(body)
        got_malformed = malformed_debt_lines(body)
        good = (got_lines == want_lines and got_malformed == want_malformed)
        ok = ok and good
        print('  [%s] debt/%-30s lines=%s (want %s)  malformed=%s (want %s)'
              % ('ok  ' if good else 'FAIL', name, got_lines, want_lines,
                 got_malformed, want_malformed))
    return ok


# check_debt_floor (#3590): proves the NEW refusal actually fires on a known
# positive (excess-with-no-marker, excess-partially-marked, a brand-new file
# with zero ceiling) AND that it does NOT fire when the marker coverage is
# sufficient - a detector that only ever says "violation" is as untrustworthy
# as one that never does.
DEBT_FLOOR_CASES = [
    # (label, cur, floor, sites, want_violations)
    ('at ceiling',      {'a.cpp': 3}, {'a.cpp': 3}, [], 0),
    ('under ceiling',   {'a.cpp': 2}, {'a.cpp': 3}, [], 0),
    ('excess, no marker',
     {'a.cpp': 4}, {'a.cpp': 3}, [], 1),
    ('excess, fully marked',
     {'a.cpp': 4}, {'a.cpp': 3}, [('a.cpp', 10, 'x', 9001)], 0),
    ('excess, partially marked',   # needs 2 markers for the excess, has 1
     {'a.cpp': 5}, {'a.cpp': 3}, [('a.cpp', 10, 'x', 9001)], 1),
    ('new file, no ceiling on record',
     {'b.cpp': 1}, {}, [], 1),
    ('new file, marked',
     {'b.cpp': 1}, {}, [('b.cpp', 5, 'y', 9002)], 0),
    # A marker on an UNRELATED path does not cover this file's excess.
    ('excess, marker on a different file',
     {'a.cpp': 4}, {'a.cpp': 3}, [('other.cpp', 10, 'x', 9001)], 1),
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
GRANDFATHER_TIGHTEN_CASES = [
    # (label, existing_ceiling, measured, want_new_ceiling)
    ('drain lowers the ceiling',
     {'a.cpp': 28}, {'a.cpp': 20}, {'a.cpp': 20}),
    ('unchanged stays unchanged',
     {'a.cpp': 28}, {'a.cpp': 28}, {'a.cpp': 28}),
    # A marked rise ABOVE the ceiling must NOT raise it - min() picks the
    # existing (smaller) value. The rise still needs its SEAM-DEBT marker
    # every run; the ceiling itself does not move to meet it.
    ('marked rise above ceiling does not raise it',
     {'a.cpp': 20}, {'a.cpp': 25}, {'a.cpp': 20}),
    # A brand-new file (no existing ceiling) never gets one for free, no
    # matter how well-marked its debt is - min(0, N) is always 0, dropped.
    ('new file gets no ceiling for free',
     {}, {'b.cpp': 5}, {}),
    # A file fully drained to zero is dropped entirely - nothing left to
    # grandfather, so any future site there is new debt from a clean slate.
    ('fully drained file is dropped',
     {'a.cpp': 28}, {}, {}),
    # A ceiling can NEVER rise, even if the file somehow regressed above its
    # OLD ceiling and got marked; min() still floors it at the existing value.
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
    immediately become the new ceiling, so re-adding what was drained is new
    debt requiring a marker. This reproduces the EXACT failure the review
    flagged - a file drained from 28 to 20, then silently regrown to 25
    (still under the OLD ceiling of 28) - and proves it is accepted for free
    under the old (untightened) ceiling and refused under the new one.
    """
    old_ceiling = {'a.cpp': 28}
    drained = {'a.cpp': 20}
    new_ceiling = tighten_ceiling(old_ceiling, drained)
    step1_ok = (new_ceiling == {'a.cpp': 20})

    regrown = {'a.cpp': 25}   # +5 reads, unmarked, still <= the OLD ceiling
    # FAIL-WITHOUT (the reported bug): under a ceiling that never tightened,
    # this regrowth is silently accepted - the review's whole point.
    violations_without_fix = len(check_debt_floor('t', regrown, old_ceiling, []))
    # PASS-WITH (the fix): under the ceiling this --update would have written,
    # the same regrowth is refused until it carries a SEAM-DEBT marker.
    violations_with_fix = len(check_debt_floor('t', regrown, new_ceiling, []))

    good = (step1_ok and violations_without_fix == 0 and violations_with_fix == 1)
    print('  [%s] drain-protection: tighten(28->20)=%s ceiling; regrow to 25 '
          '(<=28) -> WITHOUT tightening: %d violation(s) (bug, want 0 to prove '
          'it reproduces); WITH tightening: %d violation(s) (fix, want 1)'
          % ('ok  ' if good else 'FAIL', step1_ok, violations_without_fix,
             violations_with_fix))
    return good


def self_test_grandfather_missing_fails_closed():
    """Finding 3 (#3590 review): confirm a MISSING grandfather file fails
    CLOSED, not open. `read_grandfather()` returns `{}` for a missing file,
    and `{}.get(path, 0)` is 0 for every path, so EVERY existing site becomes
    "excess" over a zero ceiling and needs a marker none of them have - the
    whole ratchet goes red, which is safe (if confusing) rather than silently
    disabling the mechanism (fail OPEN, which would be the actual bug)."""
    cur = {'a.cpp': 5}
    floor_if_missing = {}   # what read_grandfather() returns when absent
    violations = check_debt_floor('t', cur, floor_if_missing, [])
    good = (len(violations) == 1)   # the WHOLE count becomes unmarked excess
    print('  [%s] grandfather-missing fails CLOSED: violations=%d (want 1 - '
          'the whole count, not silently accepted)'
          % ('ok  ' if good else 'FAIL', len(violations)))
    return good


# class2_zero_tolerance (#3590 review finding 2): unconditional, no --force
# escape hatch, no ceiling, no marker - any class-2 site at all is a violation.
CLASS2_ZERO_TOLERANCE_CASES = [
    # (label, cur2, want_violations)
    ('empty is clean', {}, 0),
    ('zero entries are clean', {'a.cpp': 0}, 0),
    ('any nonzero count is a violation, however small', {'a.cpp': 1}, 1),
    ('multiple files each violate independently', {'a.cpp': 1, 'b.cpp': 2}, 2),
]


def self_test_class2_zero_tolerance():
    ok = True
    for label, cur2, want in CLASS2_ZERO_TOLERANCE_CASES:
        got = len(class2_zero_tolerance(cur2))
        good = (got == want)
        ok = ok and good
        print('  [%s] class2-zero-tolerance/%-42s violations=%d (want %d)'
              % ('ok  ' if good else 'FAIL', label, got, want))
    return ok


def self_test():
    ok = self_test_compare()
    for name, body, want1, want2, want3 in FIXTURES:
        got1, got2, got3, _malformed = scan_text(body, name)
        good = (len(got1) == want1 and len(got2) == want2 and len(got3) == want3)
        ok = ok and good
        print('  [%s] %s: expected (%d,%d,%d), got (%d,%d,%d)'
              % ('ok  ' if good else 'FAIL', name, want1, want2, want3,
                 len(got1), len(got2), len(got3)))
    ok = self_test_debt() and ok
    ok = self_test_debt_floor() and ok
    ok = self_test_grandfather_tighten() and ok
    ok = self_test_drain_protection() and ok
    ok = self_test_grandfather_missing_fails_closed() and ok
    ok = self_test_class2_zero_tolerance() and ok
    # A detector that cannot see the real tree is also broken.
    c1, _, c3, _malformed = scan(ROOT)
    if not c1 or not c3:
        ok = False
        print('  [FAIL] whole-tree scan found ZERO class-1 reads - the matcher is '
              'matching nothing, which is what a false green looks like')
    else:
        print('  [ok  ] whole-tree scan found %d class-1 (incl. sanctioned) and '
              '%d class-3 read(s)' % (len(c1), len(c3)))
    print('check_imported_symbol_reads --self-test: ' + ('PASS' if ok else 'FAIL'))
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

    c1, c2, c3, malformed = scan(ROOT)
    cur1, cur2, cur3 = counts(c1), counts(c2), counts(c3)
    base1, base2, base3 = read_baseline()
    floor1, _floor2, floor3 = read_grandfather()

    if a.list:
        for path, line, arg, debt in c3:
            print('class3  %s:%d  %s%s' % (path, line, arg,
                  '  [SEAM-DEBT #%d]' % debt if debt else ''))
        for path, line, arg, debt in c1:
            print('class1  %s:%d  %s%s' % (path, line, arg,
                  '  [SEAM-DEBT #%d]' % debt if debt else ''))
        for path, line, arg, debt in c2:
            print('class2  %s:%d  %s%s' % (path, line, arg,
                  '  [SEAM-DEBT #%d]' % debt if debt else ''))
        for path, line in sorted(set(malformed)):
            print('malformed-debt-marker  %s:%d' % (path, line))

    # A rise above the grandfathered ceiling (#3590) needs a per-site SEAM-DEBT
    # marker; this is checked on EVERY run (--strict and --update alike), not
    # only at the moment `--force` is passed, so a marker deleted later while
    # the count stays up is caught on the very next run too.
    debt_bad = (check_debt_floor('class1', cur1, floor1, c1) +
                check_debt_floor('class3', cur3, floor3, c3) +
                malformed_debt_report(malformed))
    # Class-2 is zero-tolerance (#3590 review finding 2): unconditional, not
    # even --force gets past it, checked on every run just like debt_bad.
    class2_bad = class2_zero_tolerance(cur2)

    if a.update:
        if class2_bad:
            print('REFUSING to update: class-2 (#3150) is ZERO-TOLERANCE - '
                  '--force does not apply to it, at all.')
            for v in class2_bad:
                print('  ' + v)
            return 1
        rises = [v for v in compare('class1', cur1, base1, stale=False) if base1] + \
                [v for v in compare('class2', cur2, base2, stale=False) if base2] + \
                [v for v in compare('class3', cur3, base3, stale=False) if base3]
        if rises and not a.force:
            print('REFUSING to update: the baseline may only go DOWN.')
            for v in rises:
                print('  ' + v)
            print('Use --force if the rise is deliberate, and say why in the commit.')
            return 1
        # (#3590) --force gets past the "may only go DOWN" refusal above, but
        # NOT past this one - there is no flag that does. The only way through
        # is a real `// SEAM-DEBT: #NNNN <reason>` marker at each new site.
        if debt_bad:
            print('REFUSING to update: new debt above the grandfathered ceiling '
                  '(#3590) needs a per-site marker - --force alone is not enough.')
            for v in debt_bad:
                print('  ' + v)
            print('Add `// SEAM-DEBT: #NNNN <reason>` at each new site, naming a')
            print('real issue that tracks the debt, or bring the count back down')
            print('to scripts/imported_symbol_reads.grandfather\'s ceiling.')
            return 1
        write_baseline(cur1, cur2, cur3)
        # (#3590 review finding 1) Tighten the ceiling DOWNWARD to match, on
        # EVERY successful update - a drain must become the new ceiling right
        # away, not only on a later deliberate hand-edit, or this run's whole
        # drain buys no protection at all. Can only ever lower a value already
        # on disk; never raises one.
        _new1, _new3, ceiling_changed = write_grandfather(floor1, floor3, cur1, cur3)
        # Report the numbers that were WRITTEN, not the raw scan. `c1` counts the
        # sanctioned-accessor reads too, so printing len(c1) said "52 class-1"
        # beside a file recording 51 - a one-line discrepancy that reads like a
        # bug in the ratchet and costs the next lane an investigation.
        n1 = sum(cur1.values())
        sanc = len(c1) - n1
        print('baseline updated: %d class-1 in %d file(s), %d class-2, %d class-3 in %d file(s)'
              % (n1, len(cur1), len(c2), sum(cur3.values()), len(cur3)))
        if sanc:
            print('  (%d further class-1 read(s) live in sanctioned accessors and are '
                  'deliberately not baselined)' % sanc)
        if ceiling_changed:
            print('grandfather ceiling tightened (%d file(s)):' % len(ceiling_changed))
            for cls, path, old_v, new_v in ceiling_changed:
                print('  %s: %s %d -> %d' % (cls, path, old_v, new_v))
        return 0

    sanc_lines, sanc_ok = sanctioned_report(c1)
    bad = (compare('class1', cur1, base1) + compare('class2', cur2, base2) +
           compare('class3', cur3, base3) + debt_bad + class2_bad)
    if not sanc_ok:
        bad += [l.strip() for l in sanc_lines if 'ZERO' in l]
    total1, total2 = len(c1), len(c2)
    want1 = sum(base1.values())
    if bad:
        print('IMPORTED-SYMBOL SEAM: %d violation(s) (#3152)' % len(bad))
        for v in bad:
            print('  ' + v)
        print('\nA new AST-gated classification read answers WRONG for every imported')
        print('symbol, silently. Read the fact off the sema::TypeSymbol instead - see')
        print('docs/proposals/papers/imported-symbol-audit-2026-08-13.md §4 for the four')
        print('accessors that already do this correctly.')
        return 1 if a.strict else 0
    for l in sanc_lines:
        print(l)
    print('check_imported_symbol_reads: clean - %d class-1 read(s) outside the '
          'sanctioned accessors (baseline %d), %d class-2 (#3150), '
          '%d class-3 member reads (#3149)'
          % (total1 - sum(1 for p, _, _, _ in c1 if p in SANCTIONED), want1, total2,
             len(c3)))
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except BaselineConflict as e:
        print('IMPORTED-SYMBOL SEAM: %s' % e)
        sys.exit(1)
