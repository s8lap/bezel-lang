#!/usr/bin/env python3
"""check_stdlib_citation_sections.py — ratchet the stdlib §-citation CLASS.

WHY (#8067). A `stdlib §N` comment is a pointer: it tells the reader which
section of `docs/Bezel_Standard_Library.md` documents the thing the comment is
about. `scripts/check_src_citations.py` already ratchets that the §-NUMBER
RESOLVES -- but it pools the spec, the stdlib, the rationale and the seven
guides into ONE namespace, so `§7` resolves (to the stdlib's `System.IO`, and
also to the spec's `§7`) and the check can never notice that the pointer
points at the WRONG section. §8061 is exactly that: `src/common/OwnershipFacts.cpp`
cited `stdlib §7` for `Encoding.GetBytes`, and `Encoding` is stdlib §3.2.

⇒ A §-number that resolves somewhere is NOT the same claim as a §-number that
resolves to the section the comment's member actually lives in. The first is
ratcheted today; the second was not ratcheted at all, and `tests/` was not
scanned by either.

⇒ THE CHECK IS A RATCHET, NOT A GATE. #8067's own measurement says why: the
precise rule ("the member is DECLARED in the cited section") flags 81 sites in
`src/` and 207 in `tests/` on the measurement tree, of which ~95 % are
structurally explained false positives (a section that DISCUSSES a member
without DECLARING it; a line that NEGATES the member; a sibling §-number
spelled BARE). A gate would carry that adjudication budget forever; a
ratchet carries it ONCE, as a baseline, and then only ever fires on a NEW
site -- which is the thing that must never happen again.

WHAT IT ASSERTS -- two classes of site, both about a `stdlib §N` citation:

    no-such-section   The cited number names NO section of the stdlib at
                      all. The stdlib stops at §11, so `stdlib §5.2`,
                      `stdlib §22.12`, `stdlib §28.2` name LANGUAGE-SPEC
                      sections wearing a `stdlib` label: the reader is sent
                      to a document that does not contain the rule. This is
                      the #8093 class, and it is MECHANICAL -- there is no
                      judgement in it.

    wrong-section     The line names a stdlib member M, and M is DECLARED
                      (per the stdlib's own declaration index) in a section
                      that is neither the cited section nor an ancestor of
                      it. This is the #8061 / #8066 class.

THE RULES, EXACTLY (they are the rules, because a ratchet nobody can
re-derive is a ratchet nobody can audit):

  R1 SCOPE. `src/` and `tests/`, files ending `.cpp .hpp .h .cc`. EVERY line
     is scanned, not only comment lines: a citation also reaches the reader
     inside a diagnostic message string (`src/sema/ExprTypecheck_Calls.cpp`
     writes "no member `IndexOf` on ... (stdlib §22.5 / §4.x)" into a
     diagnostic), and such a citation is exactly as wrong as one in a
     comment. MEASURED: restricting the scan to comment lines loses 2 of the
     39 `no-such-section` sites the #8093 measurement names.

  R2 CITATIONS.
     EXPLICIT  `stdlib` (any case) followed by `§N`, N a dotted number.
     BARE      `§N` with no document word in front of it. A bare citation is
               ATTRIBUTED TO THE STDLIB iff the line carries an explicit
               stdlib citation and the bare citation appears AFTER it --
               because a line that says "stdlib §9.1 / §2.18" puts the
               `stdlib` label on the FIRST citation only, and a checker that
               reads only the labelled one loses the second claim. A bare
               citation that appears BEFORE the first explicit stdlib
               citation, or that is preceded by `spec` / `rationale` /
               `guide`, is never a stdlib citation.
     ATTRIBUTION IS THE POINT, not a nicety: `src/common/BuiltinRegistry.hpp`
     writes "stdlib §1 Conventions -- *Window parameters*; §9.6 `String`;
     §3.2", and the `String` claim is cited at §9.6, correctly, on the same
     line.

  R3 no-such-section. An EXPLICIT stdlib citation whose number is not a
     heading number of `docs/Bezel_Standard_Library.md`. (Only EXPLICIT: a
     bare `§14.2` on a stdlib line is far more often the SPEC's §14.2 --
     see #8093, whose repair for 30 of its 38 sites is to DROP the `stdlib`
     prefix -- and reading it as a stdlib citation would manufacture
     false positives.)

  R4 wrong-section. On a line that carries stdlib citations:
       (a) the line carries exactly ONE EXPLICIT stdlib citation -- with two
           explicit citations on one line the comment may be making two
           claims and the member cannot be attributed to either;
       (b) the line names exactly ONE stdlib-declared member M -- see R5 for
           what "names" means; with two candidate members the line cannot
           attribute the claim either;
       (c) the line does not NEGATE M (R6);
       (d) M is DECLARED under NONE of the line's stdlib citations -- the
           explicit one AND the attributed bare siblings. The union is the
           right set, not the single labelled citation: a line that says
           "stdlib §1 Conventions; §9.6 `String`" cites §9.6 for the
           `String` claim, and §9.6 IS where `String` lives. If M is
           declared in several sections, the citation is accepted when ANY of
           them is the cited section or a descendant of it (the cited section
           is an ancestor of the declaring section).

  R5 WHAT COUNTS AS "NAMING A MEMBER". A stdlib-declared name N is named by
     the line iff:
       (a) it appears inside backticks -- `` `GetBytes` ``;
       (b) it is the member component of a qualified `Type.Member` or
           `Type<T>.Member` -- `Encoding.GetBytes`;
       (c) it is a stdlib TYPE name (a class, interface, struct, record or
           enum) and appears as a bare identifier -- `Random`, `Encoding`,
           `TimeoutException`.
     (c) exists because a bare METHOD or PROPERTY name is an ordinary
     English word: `All`, `Count`, `Value`, `Next`, `Error`, `Item`, `Key`,
     `Write`, `Read`, `Add`, `Remove`, `Insert`, `Clear`, `Contains` are all
     stdlib members AND all ordinary words, and reading them as a citation's
     subject manufactures thousands of false positives. A TYPE name does not
     have that problem.
     ⇒ R5(c) still catches the #8061 site: `stdlib §7   GetBytes  (Encoding;
     ...)` names the type `Encoding`, which the stdlib declares in §3.2, so
     the site is flagged and the fix -- re-point the citation at §3.2 -- is
     the same one either way.

  R6 NEGATION. A line that NEGATES the member is not making the claim the
     check reads: "declares no METHOD", "the only documented `ForEach` is
     elsewhere", "does not declare X". A line is negated when one of the
     phrases `only documented`, `declares no`, `does not declare`,
     `doesn't declare`, `not documented`, `no such`, `neither` appears on it,
     or when one of the words `no`, `not`, `never`, `neither`, `nor`,
     `elsewhere`, `instead`, `rather` appears within the 80 characters
     before the member's mention.

  R7 THE DECLARATION INDEX. Built from `docs/Bezel_Standard_Library.md`
     itself: every `public [modifiers] <kind> <Name>` line inside a numbered
     section declares `<Name>` IN THAT SECTION (the nearest enclosing
     numbered heading -- an unnumbered `#### TimeoutException` inside §6.1
     declares it in §6.1). Nothing outside the stdlib is in the index, so a
     compiler-internal identifier (`TypeRef`, `CallExpr`) is never a
     candidate member -- that is the difference between this rule and the
     naive "the word appears in the section" rule, which #8067 measured at
     167 sites in `src/` with ~99 % false positives.

THE BASELINE IS EXACT, NOT A CEILING -- the rule every sibling in this family
uses (`check_dangling_capture.py:99`, `check_deprecated_jit_oracle.py:49`,
`check_string_view_ownership.py:63`). A count ABOVE the baseline fails: a NEW
site appeared, and that is the one thing this check exists to prevent. A count
BELOW the baseline fails as STALE HEADROOM, so a site that was fixed but not
banked is noticed instead of silently buying room for a future regression.

THE BASELINE ALSO CARRIES THE SITE LIST, not only the count, because a count
alone cannot tell a NEW REAL DEFECT from the noise it sits in: the list is
what makes `--strict` name the new site, and it is the "list to shrink" the
issue asks for. The site KEY is (path, kind, member, cited) and carries NO
line number, because a line-numbered key would fail the gate on every
unrelated commit that moved a comment.

⇒ FAIL CLOSED: no baseline file -> exit 2, "refusing to report green"; a
stdlib document that cannot be read or that yields no section headings ->
exit 2. A ratchet that cannot run must never report green.

Usage:  scripts/check_stdlib_citation_sections.py [--strict] [--self-test]
                                                        [--update] [--list]
"""
import argparse
import io
import os
import pathlib
import re
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
BASELINE = pathlib.Path(__file__).with_name('stdlib_citation_sections.baseline')
STDLIB_REL = os.path.join('docs', 'Bezel_Standard_Library.md')

ROOTS = ('src', 'tests')
EXTS = ('.cpp', '.hpp', '.h', '.cc')
SKIP_DIR_PARTS = ('third_party', 'build', '.git')

# `### 3.2 `Encoding`` / `## 7. `System.IO`` -- the stdlib's own section
# numbers. Only a NUMBERED heading names a section; an unnumbered `####`
# (e.g. `#### `TimeoutException``) belongs to its nearest numbered ancestor.
HEADING = re.compile(r'^#{2,4}\s+(\d+(?:\.\d+)*)\.?\s', re.M)

# A stdlib declaration: `public [modifiers] <kind> <Name>`.
DECL = re.compile(
    r'^\s*public\s+(?:static\s+|abstract\s+|sealed\s+|readonly\s+|pure\s+|'
    r'const\s+|partial\s+|virtual\s+|override\s+|async\s+)*'
    r'(class|interface|struct|enum|record|method|property|const|extension|'
    r'choice)\s+([A-Za-z_][A-Za-z0-9_]*)')
TYPE_KINDS = ('class', 'interface', 'struct', 'record', 'enum')

# `stdlib §9.6` / `stdlib §22.12` -- the label decides the document.
DOC_CITE = re.compile(r'(stdlib|spec|rationale|guide)\s*§\s*(\d+(?:\.\d+)*)',
                      re.IGNORECASE)
# A bare `§N`, whatever precedes it.
BARE_CITE = re.compile('§(\\d+(?:\\.\\d+)*)')
# The document word immediately before a `§` (if any).
DOC_WORD_BEFORE = re.compile(r'([A-Za-z]+)\s*$')

BACKTICKED = re.compile(r'`([A-Za-z_][A-Za-z0-9_]*)`')
# `Type.Member` / `Type<T>.Member`, backticked or not.
QUALIFIED = re.compile(
    r'`?([A-Za-z_][A-Za-z0-9_]*)\s*(?:<[^>]*>)?\.([A-Za-z_][A-Za-z0-9_]*)`?')
IDENT = re.compile(r'[A-Za-z_][A-Za-z0-9_]*')

NEG_PHRASES = re.compile(
    r"(only\s+documented|declares\s+no|does\s+not\s+declare|doesn\'t\s+declare|"
    r"not\s+documented|no\s+such|neither)", re.IGNORECASE)
NEG_WORDS = re.compile(
    r'\b(no|not|never|neither|nor|elsewhere|instead|rather)\b', re.IGNORECASE)
NEG_WINDOW = 80

DOC_WORDS = ('stdlib', 'spec', 'rationale', 'guide')


def ancestors(section):
    """Every prefix of a dotted number: '4.8' -> {'4', '4.8'}."""
    parts = section.split('.')
    return {'.'.join(parts[:k]) for k in range(1, len(parts) + 1)}


class StdlibIndex:
    """The stdlib's section numbers and its declaration index.

    `sections`  -- every numbered heading the stdlib defines.
    `declares`  -- name -> set of sections that declare it.
    `is_type`   -- name -> True when it is declared as a type.
    `visible`   -- name -> the set of sections C such that the name is
                   declared in C or in a DESCENDANT of C (i.e. C is the
                   declaring section or one of its ancestors).
    """

    def __init__(self, sections, declares, is_type):
        self.sections = sections
        self.declares = declares
        self.is_type = is_type
        self.visible = {}
        for name, secs in declares.items():
            vis = set()
            for sec in secs:
                vis |= ancestors(sec)
            self.visible[name] = vis


def build_index(stdlib_path):
    """Read the stdlib and build its section + declaration index.

    Raises RuntimeError when the document cannot be read or carries no
    numbered heading -- the caller turns that into "refusing to report
    green", never into a zero count.
    """
    try:
        text = io.open(stdlib_path, encoding='utf-8').read()
    except OSError as exc:
        raise RuntimeError(f'cannot read {stdlib_path}: {exc}')
    heads = [(m.group(1), m.start(), m.end()) for m in HEADING.finditer(text)]
    if not heads:
        raise RuntimeError(f'{stdlib_path}: no numbered section headings found')
    sections = set(sec for sec, _, _ in heads)
    declares = {}
    is_type = {}
    for i, (sec, _, end) in enumerate(heads):
        stop = heads[i + 1][1] if i + 1 < len(heads) else len(text)
        for line in text[end:stop].split('\n'):
            m = DECL.match(line)
            if not m:
                continue
            kind, name = m.group(1), m.group(2)
            declares.setdefault(name, set()).add(sec)
            if kind in TYPE_KINDS:
                is_type[name] = True
    return StdlibIndex(sections, declares, is_type)


def scanned_files(root=ROOT):
    files = []
    for base in ROOTS:
        base_dir = os.path.join(str(root), base)
        if not os.path.isdir(base_dir):
            continue
        for dirpath, dirnames, filenames in os.walk(base_dir):
            dirnames[:] = [d for d in dirnames if d not in SKIP_DIR_PARTS]
            for fn in sorted(filenames):
                if fn.endswith(EXTS):
                    files.append(os.path.join(dirpath, fn))
    return files


def cited_sections(line):
    """(explicit stdlib numbers, attributed bare numbers) for one line."""
    explicit = []
    first = None
    for m in DOC_CITE.finditer(line):
        if m.group(1).lower() != 'stdlib':
            continue
        explicit.append(m.group(2))
        if first is None:
            first = m.start()
    if not explicit:
        return [], []
    attributed = []
    for m in BARE_CITE.finditer(line):
        if m.start() < first:
            continue
        before = line[:m.start()]
        w = DOC_WORD_BEFORE.search(before)
        if w and w.group(1).lower() in DOC_WORDS:
            continue
        attributed.append(m.group(1))
    return explicit, attributed


def named_members(line, index):
    """The stdlib-declared names the line names (R5), in first-seen order."""
    seen = []

    def add(name):
        if name in index.visible and name not in seen:
            seen.append(name)

    for m in BACKTICKED.finditer(line):
        add(m.group(1))
    for m in QUALIFIED.finditer(line):
        add(m.group(2))
    for m in IDENT.finditer(line):
        if index.is_type.get(m.group(0)):
            add(m.group(0))
    return seen


def negated(line, member):
    """True when the line DENIES the member's existence (R6)."""
    if NEG_PHRASES.search(line):
        return True
    for m in re.finditer(r'`?' + re.escape(member) + r'`?', line):
        before = line[max(0, m.start() - NEG_WINDOW):m.start()]
        if NEG_WORDS.search(before):
            return True
    return False


class Site:
    """One flagged citation site."""

    __slots__ = ('path', 'lineno', 'kind', 'member', 'cited', 'text')

    def __init__(self, path, lineno, kind, member, cited, text):
        self.path = path
        self.lineno = lineno
        self.kind = kind
        self.member = member
        self.cited = cited
        self.text = text

    def key(self):
        """The line-number-independent identity of the site."""
        return (self.path, self.kind, self.member, self.cited)

    def format(self):
        return (f'{self.path}:{self.lineno}: {self.kind}: `{self.member}` '
                f'cited at stdlib §{self.cited}')


def scan(root=ROOT):
    """Every flagged site under `root`, sorted by (path, lineno, kind)."""
    index = build_index(os.path.join(str(root), STDLIB_REL))
    sites = []
    for path in scanned_files(root):
        rel = os.path.relpath(path, str(root))
        with io.open(path, encoding='utf-8', errors='replace') as fh:
            for lineno, line in enumerate(fh.read().split('\n'), 1):
                if '§' not in line or 'stdlib' not in line.lower():
                    continue
                explicit, attributed = cited_sections(line)
                if not explicit:
                    continue
                # R3 -- an explicit stdlib citation that names no stdlib
                # section at all (the #8093 class).
                for number in explicit:
                    if number not in index.sections:
                        sites.append(Site(rel, lineno, 'no-such-section',
                                          number, number, line.strip()))
                # R4 -- the member is declared somewhere else.
                if len(explicit) != 1:
                    # R4(a): two EXPLICIT stdlib citations on one line and the
                    # comment may be making two claims; the member cannot be
                    # attributed to either.
                    continue
                cited = explicit[0]
                if cited not in index.sections:
                    # R3 already reported it: the number names no stdlib
                    # section, so "the member is not declared there" adds
                    # nothing. One defect, one site.
                    continue
                members = named_members(line, index)
                if len(members) != 1:
                    continue
                member = members[0]
                # The member must be declared under NONE of the line's stdlib
                # citations -- the explicit one AND the attributed bare
                # siblings. The union is the right set: a line that says
                # "stdlib §1 Conventions; §9.6 `String`" cites §9.6 for the
                # `String` claim, and §9.6 IS where `String` lives.
                cits = explicit + attributed
                if any(c in index.visible[member] for c in cits):
                    continue
                if negated(line, member):
                    continue
                sites.append(Site(rel, lineno, 'wrong-section', member,
                                  cited, line.strip()))
    sites.sort(key=lambda s: (s.path, s.lineno, s.kind))
    return sites


def read_baseline(path=BASELINE):
    """(count, {site keys}) from the baseline file; raises on a bad file."""
    text = path.read_text(encoding='utf-8')
    count = None
    keys = set()
    for raw in text.split('\n'):
        line = raw.strip()
        if not line or line.startswith('#'):
            continue
        parts = raw.split('\t')
        if parts[0].strip().startswith('count:'):
            count = int(parts[0].split(':', 1)[1].strip())
            continue
        if parts[0].strip() == 'site:':
            if len(parts) != 5:
                raise ValueError('malformed site line: %r' % raw)
            keys.add((parts[1], parts[2], parts[3], parts[4]))
            continue
        raise ValueError('unrecognised baseline line: %r' % raw)
    if count is None:
        raise ValueError('baseline has no `count:` line')
    return count, keys


def write_baseline(count, keys, path=BASELINE):
    lines = [
        '# stdlib_citation_sections.baseline - the ratchet for issue #8067.',
        '#',
        '# A `stdlib §N` citation must name a section that DECLARES the member',
        '# the comment is about. This file is the set of sites that are known',
        '# to violate that, so the check fires only on a NEW one.',
        '#',
        '# THE BASELINE IS EXACT, NOT A CEILING (the rule every sibling in this',
        '# family uses): the count below must EQUAL the measured count. A rise',
        '# is a new site -- the thing this check exists to prevent. A fall is',
        '# STALE HEADROOM: a site was fixed but not banked, and until it is',
        '# banked the count hides room for a future regression.',
        '#',
        '# Regenerate with:  scripts/check_stdlib_citation_sections.py --update',
        '#',
        '# `site:` lines carry the site KEY -- path, kind, member, cited number',
        '# -- and NO line number, because a line-numbered key would fail the',
        '# gate on every unrelated commit that moved a comment.',
        '#',
        'count: %d' % count,
    ]
    for key in sorted(keys):
        lines.append('site:\t%s\t%s\t%s\t%s' % key)
    path.write_text('\n'.join(lines) + '\n', encoding='utf-8')

FIXTURE_STDLIB = """# Fixture stdlib

## 1. Conventions

## 2. `System`

### 2.1 `Console`

```bezel
public static class Console
  public static method Write(value: string);
end Console;
```

### 2.2 `Math`

```bezel
public static class Math
  public static pure method Abs(x: int): int;
end Math;
```

### 2.7 `Random`

```bezel
public static class Random
  public method Dispose(): void;
end Random;
```

### 3.2 `Encoding`

```bezel
public abstract class Encoding
  public abstract method GetBytes(s: string): owned Array<byte>;
end Encoding;
```

## 5. `System.Collections.Linq`

### 5.1 No-selector aggregation overloads

```bezel
public static class Linq
  public static method All(x: int): bool;
end Linq;
```

## 7. `System.IO`

### 7.4 `Path`

```bezel
public static class Path
  public static method Combine(a: string, b: string): string;
end Path;
```

## 9. Primitive type members

### 9.6 `String`

```bezel
public static class String
  public method Split(sep: string): owned Array<string>;
end String;
```
"""

# (label, source, expected number of flagged sites)
FIXTURES = [
    # R3 -- the #8093 class: a number that names no stdlib section.
    ('#8093: `stdlib §5.2` names no stdlib section -> no-such-section',
     '// fix #1 (stdlib §5.2): a rule that lives in the SPEC, not the stdlib.\n',
     1),
    # R3 -- a bare `§14.2` on a stdlib line is not read as a stdlib citation.
    ('R3: a BARE `§14.2` beside a stdlib citation is not a stdlib citation',
     '// stdlib §9.6; and the spec\'s §14.2 says otherwise.\n',
     0),
    # R4 -- the #8061 class, the exact shape the issue names.
    ('#8061: `stdlib §7` for `Encoding`/`GetBytes` -> wrong-section',
     '//   stdlib §7   GetBytes                        (Encoding; the\n',
     1),
    # R4 -- a correct citation is silent.
    ('R4: `stdlib §3.2` for `Encoding` -> silent',
     '// stdlib §3.2 GetBytes (Encoding; the overload returns a value type)\n',
     0),
    ('R4: `stdlib §9.6` for `String` -> silent',
     '// stdlib §9.6 Split (string)\n',
     0),
    # R2 -- the bare sibling citation is attributed to the stdlib.
    ('R2: `stdlib §1 ...; §9.6 `String`` -> the bare sibling is attributed '
     'and is correct',
     '// stdlib §1 Conventions -- *Window parameters*; §9.6 `String`; §3.2\n',
     0),
    ('R2: a BARE sibling is not a stdlib citation for R3 either',
     '// stdlib §1 Conventions; §22.12 is a spec section.\n',
     0),
    # R4(b) -- two candidate members on one line: no attribution possible.
    ('R4(b): two candidate members on the line -> silent',
     '// stdlib §7 `Encoding` and `Random` both live elsewhere.\n',
     0),
    # R4(a) -- two stdlib citations on one line: no attribution possible.
    ('R4(a): two stdlib citations on the line -> silent',
     '// stdlib §7.4 / stdlib §2.2: two claims on one line.\n',
     0),
    # R6 -- a line that denies the member is not making the claim.
    ('R6: a line that NEGATES the member -> silent',
     '// in stdlib §5.1 (the only documented `ForEach` is the unrelated '
     'static)\n',
     0),
    # R5(c) -- a bare method name is an ordinary English word, not a member.
    ('R5(c): a bare METHOD name is not a named member -> silent',
     '// stdlib §9.6 All derive from Exception.\n',
     0),
    # R5(a) -- a backticked method name IS a member.
    ('R5(a): a backticked method name IS a member -> flagged',
     '// stdlib §2.1 `Split` is the string splitter.\n',
     1),
    # R7 -- a compiler-internal identifier is never a candidate member.
    ('R7: a compiler-internal identifier is not a member -> silent',
     '// stdlib §3.2 TypeRef and CallExpr are compiler internals.\n',
     0),
    # A type name that IS declared in the cited section -> silent.
    ('R4: `stdlib §2.7` for `Random` -> silent',
     '// stdlib §2.7 `Random` has a no-op Dispose.\n',
     0),
]


def self_test():
    """Prove the rules fire on the defect classes and stay silent elsewhere."""
    ok = True
    with tempfile.TemporaryDirectory() as td:
        root = pathlib.Path(td)
        (root / 'docs').mkdir()
        (root / 'docs' / 'Bezel_Standard_Library.md').write_text(
            FIXTURE_STDLIB, encoding='utf-8')
        for i, (label, src, expect) in enumerate(FIXTURES):
            # A FRESH tree per fixture: sharing one leaks an earlier fixture's
            # sites into a later one.
            case = root / f'case{i}'
            (case / 'src').mkdir(parents=True)
            (case / 'tests').mkdir()
            (case / 'docs').mkdir()
            (case / 'docs' / 'Bezel_Standard_Library.md').write_text(
                FIXTURE_STDLIB, encoding='utf-8')
            (case / 'src' / 'fixture.cpp').write_text(src, encoding='utf-8')
            try:
                got = len(scan(case))
            except RuntimeError as exc:
                print(f'  FAIL expect={expect} got=ERROR {exc}  {label}')
                ok = False
                continue
            status = 'ok  ' if got == expect else 'FAIL'
            if got != expect:
                ok = False
            print(f'  {status} expect={expect} got={got}  {label}')
    # The baseline rules themselves.
    with tempfile.TemporaryDirectory() as td:
        base = pathlib.Path(td) / 'b.baseline'
        write_baseline(3, {('src/a.cpp', 'wrong-section', 'Encoding', '7')},
                       base)
        count, keys = read_baseline(base)
        if count != 3 or len(keys) != 1:
            print('  FAIL baseline round-trip')
            ok = False
        else:
            print('  ok   baseline round-trip')
        try:
            base.write_text('count: 1\nsite:\tonly\ttwo\n', encoding='utf-8')
            read_baseline(base)
            print('  FAIL a malformed site line must raise')
            ok = False
        except ValueError:
            print('  ok   a malformed site line raises')
    print('check_stdlib_citation_sections --self-test: '
          + ('PASS' if ok else 'FAIL'))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument('--strict', action='store_true',
                    help='gate phase: fail if the measured sites differ from '
                         'the baseline')
    ap.add_argument('--self-test', action='store_true',
                    help='prove the rules on planted fixtures, then exit')
    ap.add_argument('--update', action='store_true',
                    help='rewrite the baseline from the current measurement')
    ap.add_argument('--list', action='store_true',
                    help='print every flagged site')
    args = ap.parse_args()

    if args.self_test:
        return self_test()

    try:
        sites = scan()
    except RuntimeError as exc:
        print(f'stdlib_citation_sections: {exc}; refusing to report green',
              file=sys.stderr)
        return 2

    n = len(sites)
    keys = set(s.key() for s in sites)

    if args.list:
        for site in sites:
            print(f'  {site.format()}')
            print(f'      {site.text[:160]}')

    if args.update:
        write_baseline(n, keys)
        print(f'stdlib_citation_sections: baseline set to {n} sites '
              f'({len(keys)} distinct keys)')
        return 0

    if args.strict:
        if not BASELINE.exists():
            print(f'stdlib_citation_sections: no baseline '
                  f'({BASELINE.name}); refusing to report green',
                  file=sys.stderr)
            return 2
        try:
            base_count, base_keys = read_baseline()
        except (ValueError, OSError) as exc:
            print(f'stdlib_citation_sections: cannot read '
                  f'{BASELINE.name}: {exc}; refusing to report green',
                  file=sys.stderr)
            return 2
        new_keys = keys - base_keys
        gone_keys = base_keys - keys
        if n != base_count or new_keys or gone_keys:
            print(f'stdlib_citation_sections: {n} site(s), baseline '
                  f'{base_count}', file=sys.stderr)
            for site in sites:
                if site.key() in new_keys:
                    print(f'  NEW  {site.format()}', file=sys.stderr)
                    print(f'       {site.text[:160]}', file=sys.stderr)
            if n > base_count:
                print('  (count ABOVE baseline — a NEW site; this is what '
                      'this check exists to prevent)', file=sys.stderr)
            if n < base_count:
                print('  (count BELOW baseline — STALE HEADROOM; bank the drop '
                      'with --update)', file=sys.stderr)
            for key in sorted(gone_keys):
                print(f'  GONE {key[0]} {key[1]} `{key[2]}` §{key[3]} '
                      '(no longer flagged — bank it with --update)',
                      file=sys.stderr)
            return 1
        print(f'stdlib_citation_sections: {n} site(s) (baseline {base_count}) '
              '— every stdlib §-citation resolves to the section that declares '
              'the member it names')
        return 0

    print(f'stdlib_citation_sections: {n} site(s) '
          f'(baseline file {BASELINE.name})')
    return 0


if __name__ == '__main__':
    sys.exit(main())
