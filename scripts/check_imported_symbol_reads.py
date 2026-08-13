#!/usr/bin/env python3
"""check_imported_symbol_reads.py - ratchet the imported-symbol classification seam.

WHY (umbrella #3152, audit docs/proposals/imported-symbol-audit-2026-08-13.md).
`ModuleLoader` never assigns `Symbol::decl` for an imported type - there is no
`ast::TypeDecl` to assign, because the importing compilation has no AST for a
dependency's types at all (`reconstructTypeSymbol` at ModuleLoader.cpp:325,
`loadMetadataPass1` at :2433). So

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
raise a count. Raising one is a deliberate act that needs `--force` and an
explanation in the commit message.

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
SCAN_DIRS = ('src',)
SUFFIXES = ('.cpp', '.hpp')

CAST = re.compile(r'dynamic_cast\s*<\s*const\s+ast::TypeDecl\s*\*\s*>\s*\(', re.S)
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
    """-> (class1_sites, class2_sites); each site is (path, line, arg)."""
    text = strip_comments(raw)
    c1, c2 = [], []
    for m in CAST.finditer(text):
        arg = ' '.join(_balanced_arg(text, m.end()).split())
        line = text.count('\n', 0, m.start()) + 1
        if SYMBOL_DECL.search(arg):
            c1.append((path, line, arg))
        elif TYPE_SYMBOL.search(arg):
            c2.append((path, line, arg))
    return c1, c2


def scan(root):
    c1, c2 = [], []
    for d in SCAN_DIRS:
        for p in sorted((root / d).rglob('*')):
            if p.suffix not in SUFFIXES:
                continue
            a, b = scan_text(p.read_text(), str(p.relative_to(root)))
            c1 += a
            c2 += b
    return c1, c2


def counts(sites):
    out = {}
    for path, _, _ in sites:
        if path in SANCTIONED:
            continue  # counted separately — see SANCTIONED
        out[path] = out.get(path, 0) + 1
    return out


def sanctioned_report(sites):
    """-> (lines, ok). A sanctioned accessor with no AST read is an error."""
    have = {}
    for path, _, _ in sites:
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


def _parse_baseline(text):
    c1, c2, section = {}, {}, 'class1'
    for raw in text.splitlines():
        line = raw.strip()
        # A merge-conflicted baseline used to reach int() and die with an
        # unhandled `ValueError: invalid literal for int() with base 10: '<<<<<<<'`
        # — a traceback where the answer is one sentence. It happens on EVERY
        # multi-family merge, because two lanes that both re-baselined always
        # conflict here, so this is the common case, not an edge case.
        if line.startswith(('<<<<<<<', '=======', '>>>>>>>')):
            raise BaselineConflict(
                'scripts/imported_symbol_reads.baseline has unresolved merge '
                'conflict markers.\n'
                '  Resolve by taking the CURRENT branch\'s version (the ratchet '
                'is a floor, so the lower\n'
                '  counts are the right ones), then re-run with --update to make '
                'it exact:\n'
                '      git checkout --ours scripts/imported_symbol_reads.baseline\n'
                '      python3 scripts/check_imported_symbol_reads.py --update\n'
                '  Do NOT hand-merge the counts: --update measures the merged '
                'tree, which is the\n'
                '  only source that can be right.')
        if not line or line.startswith('#'):
            if line.startswith('# class2'):
                section = 'class2'
            elif line.startswith('# class1'):
                section = 'class1'
            continue
        n, path = line.split(None, 1)
        (c1 if section == 'class1' else c2)[path] = int(n)
    return c1, c2


def read_baseline():
    if not BASELINE.exists():
        return {}, {}
    return _parse_baseline(BASELINE.read_text())


def write_baseline(c1, c2):
    lines = [
        '# imported_symbol_reads.baseline - the ratchet for umbrella #3152.',
        '# Regenerate with scripts/check_imported_symbol_reads.py --update after a',
        '# family lands. A count may only go DOWN; raising one needs --force and an',
        '# explanation in the commit message. See',
        '# docs/proposals/imported-symbol-audit-2026-08-13.md for the per-site triage.',
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
     'auto* td = dynamic_cast<const ast::TypeDecl*>(ts->decl);\n', 1, 0),
    ('positive_multi_line.cpp',
     'const auto* rtd =\n'
     '    dynamic_cast<const ast::TypeDecl*>(\n'
     '        rt->declaration->decl);\n', 1, 0),
    ('positive_wrapped_call.cpp',
     'if (auto* td = dynamic_cast<const ast::TypeDecl*>(\n'
     '        pt->declaration->decl)) {\n', 1, 0),
    ('class2_always_null.cpp',
     'auto* d = dynamic_cast<const ast::TypeDecl*>(rt->declaration);\n', 0, 1),
    ('negative_plain_ast_cast.cpp',
     'auto* td = dynamic_cast<const ast::TypeDecl*>(&d);\n'
     'auto* t2 = dynamic_cast<const ast::TypeDecl*>(child.get());\n', 0, 0),
    ('negative_other_node_type.cpp',
     'auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl);\n', 0, 0),
    # A prose mention is not a cast — line and block form alike.
    ('negative_line_comment.cpp',
     '// dynamic_cast<const ast::TypeDecl*>(ts->decl) is wrong for an import\n',
     0, 0),
    ('negative_block_comment.cpp',
     '/* explains dynamic_cast<const ast::TypeDecl*>(ts->decl) at length */\n',
     0, 0),
    # ...but a real cast on the SAME line as a comment still counts, so the
    # exemption cannot be used to hide one.
    ('positive_code_beside_comment.cpp',
     'auto* td = dynamic_cast<const ast::TypeDecl*>(ts->decl);  // see #3152\n',
     1, 0),
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


def self_test():
    ok = self_test_compare()
    for name, body, want1, want2 in FIXTURES:
        got1, got2 = scan_text(body, name)
        good = (len(got1) == want1 and len(got2) == want2)
        ok = ok and good
        print('  [%s] %s: expected (%d,%d), got (%d,%d)'
              % ('ok  ' if good else 'FAIL', name, want1, want2,
                 len(got1), len(got2)))
    # A detector that cannot see the real tree is also broken.
    c1, _ = scan(ROOT)
    if not c1:
        ok = False
        print('  [FAIL] whole-tree scan found ZERO class-1 reads - the matcher is '
              'matching nothing, which is what a false green looks like')
    else:
        print('  [ok  ] whole-tree scan found %d class-1 read(s)' % len(c1))
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

    c1, c2 = scan(ROOT)
    cur1, cur2 = counts(c1), counts(c2)
    base1, base2 = read_baseline()

    if a.list:
        for path, line, arg in c1:
            print('class1  %s:%d  %s' % (path, line, arg))
        for path, line, arg in c2:
            print('class2  %s:%d  %s' % (path, line, arg))

    if a.update:
        rises = [v for v in compare('class1', cur1, base1, stale=False) if base1] + \
                [v for v in compare('class2', cur2, base2, stale=False) if base2]
        if rises and not a.force:
            print('REFUSING to update: the baseline may only go DOWN.')
            for v in rises:
                print('  ' + v)
            print('Use --force if the rise is deliberate, and say why in the commit.')
            return 1
        write_baseline(cur1, cur2)
        print('baseline updated: %d class-1 read(s) in %d file(s), %d class-2'
              % (len(c1), len(cur1), len(c2)))
        return 0

    sanc_lines, sanc_ok = sanctioned_report(c1)
    bad = compare('class1', cur1, base1) + compare('class2', cur2, base2)
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
        print('docs/proposals/imported-symbol-audit-2026-08-13.md §4 for the four')
        print('accessors that already do this correctly.')
        return 1 if a.strict else 0
    for l in sanc_lines:
        print(l)
    print('check_imported_symbol_reads: clean - %d class-1 read(s) outside the '
          'sanctioned accessors (baseline %d), %d class-2 (#3150)'
          % (total1 - sum(1 for p, _, _ in c1 if p in SANCTIONED), want1, total2))
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except BaselineConflict as e:
        print('IMPORTED-SYMBOL SEAM: %s' % e)
        sys.exit(1)
