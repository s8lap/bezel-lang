#!/usr/bin/env python3
"""check_string_view_ownership.py — ratchet the string_view-owns-nothing hazard.

WHY (#6469). `ast::QualifiedName::parts` is a `std::vector<std::string_view>`
(`src/ast/Node.hpp:42`), and the same shape appears 139 times across `src/`
plus `tests/`. Pushing a `std::string` TEMPORARY into one leaves a
`string_view` pointing at storage that dies at the end of that full-expression.
The defect is invisible to every oracle this project has:

  * it does not crash at the push, and usually not at the next read;
  * it compiles clean under `-Wall -Wextra` (a `string_view` binds happily to a
    `std::string` rvalue — that implicit conversion is the whole trap);
  * `runAot`'s `exitCode == 0` and `assertNoLeak` are both SATISFIED, because
    the program still runs — it just reads a name that is no longer there.

The #6460 fix (commit 978588d0) shipped `qn->parts.push_back(std::string(colName))`
in `typeCall`'s collection-literal factory arm, where `colName` is a local
`std::string`. The temporary died at the end of the statement, so
`TypeRef::headName()` (`src/sema/TypeRef.hpp:80`) read `parts[0]` as the EMPTY
string: the result TypeRef had an array-shaped type whose head no member or
indexer lookup could match, so `ys.Count` and `ys[0]` lowered to NOTHING at
exit 0 while the identical program under an explicit `var ys: Array<int>`
annotation worked. Only a stdout oracle separates the two, and only
instrumenting the read sites found it. The author rewrote the arm to push a
static string literal through a six-name allowlist.

So this ratchet exists for the CLASS, because the instance is invisible. It is
the cheap guard #6469 asked for (its Proposal, option 2).

WHAT IT ASSERTS

    dangling_views

    An insert (`push_back` / `emplace_back` / `push_front` / `insert`) into a
    container whose declared element type is `string_view`, where the inserted
    expression yields a VIEW into storage that cannot outlive the full
    expression. Two argument shapes count:

      TEMP_STRING   `std::string(...)` / `std::string{...}` as the argument —
                    the construction IS the temporary. This is the #6469 shape
                    and the whole of `grep "push_back(std::string("`.
      VIEW_OF_TEMP  `.c_str()` / `.data()` / `.substr(...)` — a view into a
                    `std::string` that is itself a temporary or a local the
                    checker cannot prove outlives the container.

    The container must be one this script can NAME: a `vector<string_view>`
    declared in a scanned root (`src/`, `tests/`), either a local or a class
    member. `QualifiedName::parts` is named by its declaration in `Node.hpp`.

SAFE, and why (these are NOT counted):

  * `ctx.internString(...)` — SemaContext's interning table owns the bytes for
    the whole compilation (`src/sema/SemaContext.hpp:936`), which is the
    pattern #6469 names as correct. `ModuleLoader.cpp:737` and `:6768` are the
    two non-literal `parts.push_back` sites in the tree and both go through it.
  * `std::move(x)` — moves an element of a container whose element type is
    OWNING; a `vector<pair<...>>` push is not a `string_view` push.
  * a string literal, a `std::string_view` lvalue the caller owns, or a ternary
    whose arms are both literals (`ExprTypecheck.cpp:991`, and the two `taskName`
    / `elemIsClass` forms) — static storage, valid for the process.

THE BASELINE IS EXACT, NOT A CEILING — the same rule every ratchet in this
family uses (check_deprecated_jit_oracle.py:243-247). A count ABOVE baseline
fails; a count BELOW fails as STALE HEADROOM so a silent drop is noticed.

SCOPE NOTE — WHY THIS IS A TEXT SCAN AND NOT A TYPE CHECK

CORRECTION (review of #6469). An earlier revision of this docstring claimed
that a compiler-level check was unavailable: "clang-tidy is not in the
toolchain; -Wdangling-gsl does not fire on a container element". BOTH halves
were false on this box, and the error mattered — it put a 460-line regex
ratchet on the record as the ONLY instrument, which a later reader would have
taken as license not to look for the cheaper one. The measurements:

  * `clang-tidy` IS in the toolchain (`/usr/bin/clang-tidy`); the gate already
    runs it as an advisory phase (`scripts/gate_full_cycle.sh:1718`), and this
    repo's `.clang-tidy` enables `bugprone-*` without disabling
    `bugprone-dangling-handle`. On the #6469 shape it reports:
        warning: std::basic_string_view outlives its value
                 [bugprone-dangling-handle]
  * the relevant compiler flag is `-Wdangling-capture`, NOT
    `-Wdangling-gsl` (which is about a different, unrelated borrow), and it is
    DEFAULT-ON in clang 21.1.8 — no `-W` opt-in and no `-Wall` needed.
    MEASURED, both directions:
        pre-fix shape, `clang++ -std=c++20 -fsyntax-only`  -> 1 warning
        safe owning-vector shape (`vector<std::string>`)   -> 0 warnings
    So it DISCRIMINATES PRECISELY — with none of the per-scope container
    machinery below, because it reads the actual type rather than regexing a
    name. Under the project's own compile command (from
    `build/compile_commands.json`) the pre-fix line
    `tests/sema/method_symbol_param_modifier_3308_test.cpp:96` warns, and the
    fixed line does not. The tree is currently at 0 such warnings.

  * the one real caveat, and why this ratchet still earns its keep:
    `BEZEL_WERROR` is `OFF` by default (`CMakeLists.txt:111`), so
    `-Wdangling-capture` does not FAIL a build today — it is visible in the log
    and in clang-tidy, but nothing gates on it. This script is what turns the
    hazard into a hard gate failure today. Keep it as defense-in-depth; do not
    record it as the only option. CORRECTED (R32): an earlier revision of this
    comment claimed that ratcheting the compiler warning would cover the
    identifier-argument gap below "for free". That is FALSE, and it is now
    measured. -Wdangling-capture fires on a dangling TEMPORARY argument --
    `parts.push_back(std::string("x"))` -- and is SILENT on a dangling
    IDENTIFIER argument -- `parts.push_back(localStdStringVar)`. The same is
    true of -Wdangling-gsl and of clang-tidy's bugprone-dangling-handle: each
    reports 0 on a file containing only that shape. So no instrument covers
    that gap today — not this script, and not the compiler. #6475 tracks
    gating the compiler warning over the shapes it DOES see; the
    identifier-argument gap is tracked separately and is still open.

So this is a TEXT ratchet over the named containers, and its honest limits are:

  * it does NOT resolve the type of a plain identifier argument. A site like
    `parts.push_back(localStdStringVar)` — where `localStdStringVar` is a local
    `std::string` whose scope ends before the vector dies — is the SAME defect
    and this script does not see it. That is a real gap, stated rather than
    hidden; the two sites it DOES find are the `std::string(...)` constructions,
    which is the shape the issue's grep reports.
  * it strips string literals and `//` comments before matching, so a
    diagnostic message that quotes `std::string(` cannot trip it, and a marker
    placed in a comment cannot exempt a site. Markers therefore do not apply
    here: every hit is either fixed or the container must stop being a
    `string_view` one.
  * it does NOT strip `/* ... */` BLOCK comments. A block comment that quotes
    the shape is a false positive (MEASURED: planting one in a fixture makes
    `find_sites` report a hit that is not code). Zero block comments quoting
    the needle exist under `src/`+`tests/` today, so the baseline is correct;
    this is a live FP path for a future one, not a current defect. Only `//`
    and string-literal stripping are claimed above, and both are verified by
    the self-test's diagnostic-string fixture.

Usage:  scripts/check_string_view_ownership.py [--strict] [--self-test]
                                                [--update] [--list]
"""
import argparse
import os
import pathlib
import re
import sys
import tempfile

ROOTS = ('src', 'tests')
EXTS = ('.cpp', '.hpp', '.h', '.cc')
SKIP_DIR_PARTS = ('third_party', 'build', '.git')

BASELINE = pathlib.Path(__file__).with_name(
    'string_view_ownership.baseline')

# A `vector<string_view>` declaration: `std::vector<std::string_view>`,
# `vector<string_view>`, `std::vector<StringView>`. The declarator NAME is
# whatever identifier follows the template argument.
SV_DECL = re.compile(
    r'(?:std::)?vector\s*<\s*(?:std::)?(?:string_view|StringView)\s*>')

# `<recv>.<insert>(` where <recv>'s LAST segment is a candidate container name.
# <recv> is one segment (`v.push_back`) or a member chain (`qn->parts.push_back`,
# `ty->name->parts.push_back`) — the local-variable form has no dot before the
# method, so it must be matched too or a local `vector<string_view>` slips past.
INSERT_CALL = re.compile(
    r'\b((?:[A-Za-z_][A-Za-z0-9_]*(?:\s*->|\s*\.)\s*)*[A-Za-z_][A-Za-z0-9_]*)'
    r'\s*\.\s*(push_back|emplace_back|push_front|insert)\s*\('
)
# The two counted argument shapes. `std::string_view(std::string(...))` is a
# second temporary and is caught by the leading alternative.
TEMP_STRING = re.compile(r'\bstd::string\s*[({]')
VIEW_OF_TEMP = re.compile(r'\.c_str\s*\(\s*\)|\.data\s*\(\s*\)|\.substr\s*\(')
# The explicitly-safe callees (see the module docstring).
SAFE_CALLEE = re.compile(r'\binternString\s*\(')
STD_MOVE = re.compile(r'\bstd::move\s*\(')


def strip_noncode(line):
    """Drop `//` comments and string-literal contents from one source line.

    A checker that greps raw text trips on the diagnostic strings this codebase
    is full of (`"push_back(std::string(" is itself the needle #6469 quotes),
    and on its own prose. Matching code only is what keeps the ratchet honest
    about what it counts.
    """
    out = []
    i, n = 0, len(line)
    in_str = False
    while i < n:
        c = line[i]
        if in_str:
            if c == '\\':
                i += 2
                continue
            if c == '"':
                in_str = False
            i += 1
            continue
        if c == '"':
            in_str = True
            i += 1
            continue
        if c == '/' and i + 1 < n and line[i + 1] == '/':
            break
        out.append(c)
        i += 1
    return ''.join(out)


def scanned_files(roots=ROOTS):
    files = []
    for root in roots:
        if not os.path.isdir(root):
            continue
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [d for d in dirnames if d not in SKIP_DIR_PARTS]
            for fn in filenames:
                if fn.endswith(EXTS):
                    files.append(os.path.join(dirpath, fn))
    return files


def brace_depths(lines):
    """Brace depth BEFORE each line, computed on comment/literal-stripped code.

    Depth is what lets a name be resolved per SCOPE rather than per file:
    `src/sema/ExprTypecheck.cpp` declares `missing` twice — a
    `vector<string_view>` at :3212 and a `vector<std::string>` at :3707 — and
    only the first is a view container.
    """
    depths = []
    depth = 0
    for line in lines:
        code = strip_noncode(line)
        depths.append(depth)
        for ch in code:
            if ch == '{':
                depth += 1
            elif ch == '}':
                depth -= 1
    return depths


def local_container_scopes(lines):
    """For each line index, the `vector<string_view>` locals VISIBLE there.

    A declaration at brace depth D is visible from a later line at any depth
    >= D, and the NEAREST such declaration wins (C++ shadowing). Non-view
    `vector<...>` declarations of the same name at the same or nearer depth
    SHADOW the view one out of the visible set, which is what stops
    `missing.push_back(std::string(lf->name))` at :3717 — a
    `vector<std::string>` — from being counted.

    Returns {lineno_index: set(names)}.
    """
    # depth -> name -> is_view, innermost declaration last so a re-declaration
    # at the same depth overwrites the earlier one.
    decls = {}   # depth -> {name: is_view}
    order = []   # (lineno_index, name, is_view)
    for i, line in enumerate(lines):
        stripped = strip_noncode(line)
        for m in re.finditer(r'\bvector\s*<\s*([^;{]*?)\s*>\s*'
                             r'([A-Za-z_][A-Za-z0-9_]*)\s*(?:=|;|\)|\{)',
                             stripped):
            elem, name = m.group(1), m.group(2)
            is_view = bool(re.fullmatch(
                r'(?:std::)?(?:string_view|StringView)', elem.strip()))
            order.append((i, name, is_view))
    depths = brace_depths(lines)
    for i, name, is_view in order:
        decls.setdefault(depths[i], {})[name] = is_view

    visible = {}
    for i in range(len(lines)):
        names = set()
        for depth in sorted(d for d in decls if d <= depths[i]):
            for name, is_view in decls[depth].items():
                if is_view:
                    names.add(name)
                else:
                    names.discard(name)
        visible[i] = names
    return visible


def container_names_by_file(files):
    """Header-declared `vector<string_view>` names (class members).

    `QualifiedName::parts` (`src/ast/Node.hpp:42`) is declared once in a
    header and written from ~90 sites across a dozen .cpp files, so member
    names must be visible tree-wide. LOCALS are deliberately NOT collected
    here — they are resolved per scope by `local_container_scopes`, because a
    file-wide name set makes `out` (a `vector<string_view>` local in
    `src/module/ModuleLoader.cpp:540`) taint the `std::vector<std::string> out`
    in `tests/support/parse_support.hpp:1192`.
    """
    header_names = set()
    for path in files:
        if not path.endswith(('.hpp', '.h')):
            continue
        with open(path, encoding='utf-8', errors='replace') as fh:
            for line in fh:
                m = SV_DECL.search(line)
                if not m:
                    continue
                tail = line[m.end():]
                for decl in re.finditer(
                        r'\b([A-Za-z_][A-Za-z0-9_]*)\s*(?:=|\{|;|\))', tail):
                    header_names.add(decl.group(1))
    return header_names


def read_lines(path):
    with open(path, encoding='utf-8', errors='replace') as fh:
        return fh.read().split('\n')


def find_sites(files, header_names):
    """Return [(path, lineno, method, argtext)] for every counted site."""
    sites = []
    for path in files:
        lines = read_lines(path)
        visible = local_container_scopes(lines)
        for i, line in enumerate(lines):
            code = strip_noncode(line)
            for m in INSERT_CALL.finditer(code):
                recv, method = m.group(1), m.group(2)
                base = re.split(r'->|\.', recv)[-1].strip()
                if base not in visible[i] and base not in header_names:
                    continue
                arg = code[m.end():]
                if SAFE_CALLEE.search(arg) or STD_MOVE.search(arg):
                    continue
                if TEMP_STRING.search(arg) or VIEW_OF_TEMP.search(arg):
                    sites.append((path, i + 1, method, arg.strip()[:90]))
    return sorted(sites)


def count_now():
    files = scanned_files()
    header_names = container_names_by_file(files)
    return find_sites(files, header_names)


def self_test():
    """Prove the matcher fires on the #6469 shape and clears on the safe ones.

    A ratchet that matches nothing says nothing (memory:
    feedback-prove-ratchet-sensitive-plant-a-sentinel). The fixtures below are
    the four shapes that must be DISTINGUISHED, each labelled with the reason
    it is in the set — so a future edit that widens or narrows the matcher
    breaks this test rather than silently changing what the gate means.
    """
    cases = [
        # (label, source, expect_counted)
        ('#6469 verbatim: push_back(std::string(colName)) into parts',
         'namespace ast { struct QN { std::vector<std::string_view> parts; }; }\n'
         'void arm(std::string colName) {\n'
         '    auto* qn = new ast::QN;\n'
         '    qn->parts.push_back(std::string(colName));\n'
         '}\n', 1),
        ('emplace_back(std::string(...)) into a local vector<string_view>',
         'void b(std::string n) {\n'
         '    std::vector<std::string_view> v;\n'
         '    v.emplace_back(std::string(n));\n'
         '}\n', 1),
        ('SAFE: internString owns the bytes for the whole compilation',
         'namespace ast { struct QN { std::vector<std::string_view> parts; }; }\n'
         'void arm(std::string n, Ctx& c) {\n'
         '    auto* qn = new ast::QN;\n'
         '    qn->parts.push_back(c.internString(n));\n'
         '}\n', 0),
        ('SAFE: a string literal has static storage',
         'namespace ast { struct QN { std::vector<std::string_view> parts; }; }\n'
         'void arm() {\n'
         '    auto* qn = new ast::QN;\n'
         '    qn->parts.push_back("Array");\n'
         '}\n', 0),
        ('SAFE: the needle inside a diagnostic string must not trip the matcher',
         'void warn(std::string m) {\n'
         '    emit("use push_back(std::string(x)) carefully");\n'
         '}\n', 0),
        ('SAFE: std::move into an owning vector is not a view push',
         'void d() {\n'
         '    std::vector<std::string_view> names;\n'
         '    std::string s = "x";\n'
         '    (void)names;\n'
         '    (void)s;\n'
         '    std::vector<std::string> owning;\n'
         '    owning.push_back(std::move(s));\n'
         '}\n', 0),
        # A member declared in a HEADER is visible tree-wide, which is what
        # makes `parts` findable from a .cpp that never declares it.
        ('HEADER member `parts` is visible from another file',
         {'src/ast/Node.hpp':
              'namespace ast { struct QN { std::vector<std::string_view> parts; }; }\n',
          'src/sema/Factory.cpp':
              '#include "Node.hpp"\n'
              'void arm(std::string colName) {\n'
              '    auto* qn = new ast::QN;\n'
              '    qn->parts.push_back(std::string(colName));\n'
              '}\n'}, 1),
        # Same FILE, two `missing` locals at different brace depths: the
        # `vector<string_view>` one first, then a `vector<std::string>` in a
        # LATER scope that shadows it. This is the real shape in
        # src/sema/ExprTypecheck.cpp (`:3212` view, `:3707` owning).
        ('SAFE: a later same-file vector<std::string> of the same name shadows it',
         'void f(std::string n) {\n'
         '    {\n'
         '        std::vector<std::string_view> missing;\n'
         '        (void)missing;\n'
         '    }\n'
         '    std::vector<std::string> missing;\n'
         '    missing.push_back(std::string(n));\n'
         '}\n', 0),
        # THE FILE-SCOPING DECISION. `out` is a `vector<string_view>` local in
        # one file and a `vector<std::string>` local in another. A global name
        # set counts the second as a hit; per-file resolution does not. Both
        # real trees in this repo have exactly this pair.
        ('SAFE: a LOCAL sv vector in another file does not taint this one',
         {'src/a.cpp':
              'void a() { std::vector<std::string_view> out; (void)out; }\n',
          'src/b.cpp':
              'void b(std::string n) {\n'
              '    std::vector<std::string> out;\n'
              '    out.push_back(std::string(n));\n'
              '}\n'}, 0),
    ]
    ok = True
    with tempfile.TemporaryDirectory() as td:
        for case_index, (label, src, expect) in enumerate(cases):
            # A FRESH directory per case. Sharing one leaks earlier fixtures
            # into later ones, and the multi-file cases would then inherit
            # container names from the single-file cases — a self-test that
            # tests its own leftovers.
            root = pathlib.Path(td) / f'case{case_index}'
            (root / 'src').mkdir(parents=True, exist_ok=True)
            (root / 'tests').mkdir(exist_ok=True)
            files_to_write = ({'src/fixture.cpp': src}
                              if isinstance(src, str) else src)
            for relpath, text in files_to_write.items():
                target = root / relpath
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(text)
            files = scanned_files(roots=(str(root / 'src'), str(root / 'tests')))
            header_names = container_names_by_file(files)
            got = len(find_sites(files, header_names))
            status = 'ok  ' if got == expect else 'FAIL'
            if got != expect:
                ok = False
            print(f'  {status} expect={expect} got={got}  {label}')
    print('check_string_view_ownership --self-test: '
          + ('PASS' if ok else 'FAIL'))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument('--strict', action='store_true',
                    help='gate phase: fail if the count differs from baseline')
    ap.add_argument('--self-test', action='store_true',
                    help='prove the matcher on planted fixtures, then exit')
    ap.add_argument('--update', action='store_true',
                    help='rewrite the baseline from the current count')
    ap.add_argument('--list', action='store_true',
                    help='print every counted site')
    args = ap.parse_args()

    if args.self_test:
        return self_test()

    sites = count_now()
    n = len(sites)

    if args.list:
        for path, lineno, method, arg in sites:
            print(f'{path}:{lineno}: .{method}({arg})')

    if args.update:
        BASELINE.write_text(f'{n}\n', encoding='utf-8')
        print(f'string_view_ownership: baseline set to {n}')
        return 0

    if args.strict:
        # Fail closed on a MISSING baseline: a green from a checker that never
        # ran is the vacuous-oracle trap this project keeps re-learning.
        if not BASELINE.exists():
            print('string_view_ownership: no baseline '
                  f'({BASELINE.name}); refusing to report green',
                  file=sys.stderr)
            return 2
        base = int(BASELINE.read_text(encoding='utf-8').split()[0])
        if n != base:
            print(f'string_view_ownership: {n} dangling_views, baseline {base}',
                  file=sys.stderr)
            for path, lineno, method, arg in sites:
                print(f'  {path}:{lineno}: .{method}({arg})', file=sys.stderr)
            if n < base:
                print('  (count BELOW baseline — STALE HEADROOM; bank the drop '
                      'with --update)', file=sys.stderr)
            return 1
        print(f'string_view_ownership: {n} dangling_views (baseline {base}) '
              '— no view into a temporary')
        return 0

    print(f'string_view_ownership: {n} dangling_views '
          f'(baseline file {BASELINE.name})')
    return 0


if __name__ == '__main__':
    sys.exit(main())