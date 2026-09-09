#!/usr/bin/env python3
"""check_null_guard_vacuous.py - find `if (<ptr>) { ...EXPECT... }` rows with
no unconditional non-null assertion on `<ptr>` first.

WHY (issue #3992). A pointer-typed local bound from a lookup/finder call and
then guarded with a bare truthy `if (ptr) { ... }` before any EXPECT runs
inside it executes ZERO assertions when the lookup returns null - a compiler
regression, a fixture that stopped parsing, or simply a fixture bug. gtest
still reports `[ OK ]`: this is the same three-outcomes-collapsed-to-one-
boolean discriminator problem issue #3892
(scripts/check_ambiguous_ran_false.py) already names for `runAot`'s `.ran`,
generalized to any pointer-returning lookup helper, gated or not.

MEASURED, REPRODUCED (fix #3985, commits ed3c050/1b87d8e/a374e15) - TWO
independent live instances, found by a human incidentally looking at the same
files for an unrelated reason:

  1. tests/sema/base_member_lookup_test.cpp::interface_member_resolves -
     `auto* ma = findMemberAccess(r, "M"); if (ma) { ...EXPECT... }` compounded
     with a genuine parse failure (an explicit `public` before an interface
     member, rejected per S11.8): the fixture never parsed, `ma` came back
     null, and the guarded block silently never ran.
  2. tests/sema/member_access_resolution_test.cpp::qualified_field_access_resolves -
     the identical `if (ma) { ...EXPECT... }` shape, but with NO parse defect:
     the fixture declared an unrelated class `D` before the exercised class
     `C`, and the file's own `firstMemberAccess` helper only walks
     `topDecls[0]`'s members, so `ma` was null for a completely different
     reason. Confirmed live: adding the unconditional `ma != nullptr` assert
     WITHOUT reordering made the real gtest binary FAIL immediately
     (`assertion failed: ma != nullptr`) - proving this row had asserted
     nothing since it was written.

Both are fixed on this tree (an unconditional `BEZEL_EXPECT_TRUE(ma !=
nullptr);` now precedes each `if (ma) { ... }`) - this script's self-test
therefore reconstructs each PRE-fix shape as a synthetic fixture (the
established technique in this family - see check_ambiguous_ran_false.py's
`comment_bleeds_to_preceding_test` fixtures) rather than relying on the live
tree to still contain a bug that has already been fixed.

WHAT COUNTS AS A ROW. Per (file, BEZEL_TEST body):
  1. A pointer-typed local bound, as its OWN statement (not a C++
     if-with-initializer condition - see below), from a call expression:
     `auto* NAME = CALL(...);` (optionally `const auto*`), where CALL may be
     a free/qualified function (`findMemberAccess(`, `bezel::sema::foo(`) or
     a chained member/arrow call (`r.ctx->resolvedType(`).
  2. The body's FIRST subsequent `if (NAME) { ... }` or
     `if (NAME != nullptr) { ... }` guard on that same NAME.
  3. That guard's block contains at least one assertion macro
     (`[BEZEL_]EXPECT_*` / `ASSERT_*`) - a guard with no assertion inside has
     nothing to lose, so it is not this issue's shape.
  4. NO unconditional protection on NAME appears between (1) and (2): neither
     `[BEZEL_](EXPECT|ASSERT)_TRUE(NAME[ != nullptr])`, nor
     `[BEZEL_](EXPECT|ASSERT)_FALSE(NAME == nullptr)`, nor `ASSERT_NE(NAME,
     nullptr)`.

WHAT IS DELIBERATELY *NOT* FLAGGED, and why the exemption is DERIVED rather
than special-cased:
  - The C++ if-with-initializer idiom, `if (auto* t = dynamic_cast<T*>(x)) {
    ... }` (real, common in this corpus - e.g.
    tests/sema/choice_sema_classification_35_test.cpp:57/83/135). There is no
    separate statement before the guard for an unconditional check to occupy
    AT ALL, and the idiom's whole point is usually a type-narrowing FILTER
    over many candidates (most of which are legitimately not the wanted
    variant), not "did a lookup succeed" - a structurally different claim
    from #3992's shape. The bind regex requires the match to start a fresh
    statement (preceded by `;`, `{`, or the top of the body slice), which
    excludes a bind nested inside an `if (`/`while (` condition by
    construction - not a name-based special case.
  - A pointer bound and then used with NO guard at all (straight-line
    dereference). A missing guard is a different, unrelated defect (a crash
    on null) - not a vacuous assertion.
  - A guard whose block asserts nothing (see criterion 3).

HOW HELPERS ARE CLASSIFIED / COMMENT SAFETY. Every regex below runs over a
COMMENT-STRIPPED copy of the file (never raw text): a comment quoting the
literal anti-pattern text (to explain why a nearby test avoids it) must not
be misread as live code, and a comment must not desynchronise the C-string /
char-literal / digit-separator handling that follows it on the same line.
This is the exact class of bug fix #3915/#3920 found and fixed in
check_ambiguous_ran_false.py; `_strip_comments` and its helpers below are
copied from that script verbatim (each check_*.py in this family is
self-contained - see check_vacuous_tests.py, which duplicates its own
BEZEL_TEST-splitting logic rather than importing a shared module) rather than
re-deriving a weaker version of the same fix.

THE BASELINE IS EXACT, NOT A CEILING - same rule as every ratchet in this
family (scripts/check_ambiguous_ran_false.py, scripts/check_deprecated_jit_
oracle.py, ...). A count BELOW baseline fails as STALE HEADROOM: a sweep PR
that fixes rows but forgets `--update` silently buys room for the next
regression.

Usage:  scripts/check_null_guard_vacuous.py [--strict] [--self-test]
                                            [--update [--force]] [--list]
"""
import argparse
import pathlib
import re
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
BASELINE = ROOT / 'scripts' / 'null_guard_vacuous.baseline'

BEZEL_TEST = re.compile(r'BEZEL_TEST\s*\(\s*(\w+)\s*,\s*(\w+)\s*\)', re.S)

# A pointer-typed local bound, as ITS OWN statement, from a call expression.
# Anchored so the match must START a fresh statement (preceded by `;`, `{`,
# or the very start of the body slice being scanned) - this is what excludes
# the C++ if-with-initializer idiom `if (auto* t = f(x)) { ... }`, where the
# same textual shape appears but nested inside an `if (`/`while (` condition
# rather than starting a statement of its own.
PTRBIND = re.compile(
    r'(?:(?<=[;{])|\A)\s*(?:const\s+)?auto\s*\*\s*(\w+)\s*=\s*'
    r'[A-Za-z_]\w*(?:\s*(?:::|\.|->)\s*[A-Za-z_]\w*)*\s*\(')

# Assertion macros this codebase (and plain gtest) use.
ASSERT_CALL = re.compile(
    r'\b(?:BEZEL_)?(?:EXPECT|ASSERT)_(?:TRUE|FALSE|EQ|NE)\s*\(')


def _guard_for(var):
    """The FIRST `if (var)` / `if (var != nullptr)` guard on `var` - matched
    up to (and including) the condition's closing paren only. The guard's
    BODY (braced or a brace-less single statement - both occur in this
    corpus, e.g. tests/sema/type_resolution_test.cpp's `unit_type`:
    `if (ref) BEZEL_EXPECT_TRUE(...);` with no braces at all) is extracted
    separately by `_guard_block_span`, since a brace-less if has no `{` for
    a naive regex to anchor on."""
    return re.compile(
        r'\bif\s*\(\s*' + re.escape(var) + r'\s*(?:!=\s*nullptr\s*)?\)')


def _guard_block_span(text, after_cond_idx):
    """The guarded BODY starting right after an `if (...)` condition's
    closing paren at `after_cond_idx`: a balanced `{ ... }` block, or (no
    braces) the single statement up to its terminating top-level `;`."""
    i, n = after_cond_idx, len(text)
    while i < n and text[i] in ' \t\r\n':
        i += 1
    if i < n and text[i] == '{':
        return _balanced_body(text, i)
    depth, j = 0, i
    while j < n:
        c = text[j]
        if c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
        elif c == ';' and depth == 0:
            return text[i:j + 1]
        j += 1
    return text[i:]


def _protection_for(var):
    """An unconditional non-null claim on `var`.

    Includes `EXPECT_TRUE(dynamic_cast<T*>(var) != nullptr)` (and the
    `EXPECT_FALSE(... == nullptr)` mirror): `dynamic_cast<T*>(nullptr)` is
    always null, so a non-null claim on the CAST result is an unconditional
    non-null claim on `var` too - a real, common idiom in this corpus
    (e.g. tests/sema/call_return_typing_test.cpp:67, checked BEFORE its
    `if (e) { ... }` guard) that would otherwise be a false positive."""
    dc = r'(?:dynamic_cast\s*<[^()]*>\s*\(\s*' + re.escape(var) + r'\s*\)|' \
         + re.escape(var) + r')'
    return re.compile(
        r'\b(?:BEZEL_)?(?:EXPECT|ASSERT)_TRUE\s*\(\s*' + dc +
        r'\s*(?:!=\s*nullptr\s*)?\)'
        r'|\b(?:BEZEL_)?(?:EXPECT|ASSERT)_FALSE\s*\(\s*' + dc +
        r'\s*==\s*nullptr\s*\)'
        r'|\bASSERT_NE\s*\(\s*' + dc + r'\s*,\s*nullptr\s*\)')


def _balanced_body(text, open_brace_idx):
    """Return the text between the brace at open_brace_idx and its match."""
    depth = 0
    for j in range(open_brace_idx, len(text)):
        if text[j] == '{':
            depth += 1
        elif text[j] == '}':
            depth -= 1
            if depth == 0:
                return text[open_brace_idx + 1:j]
    return text[open_brace_idx + 1:]


# --- comment/string-literal-safe stripping ---------------------------------
# Copied verbatim from scripts/check_ambiguous_ran_false.py (fixes #3915,
# #3920) - see that script's docstring/comments for the exact hazards this
# guards against. Duplicated deliberately: each check_*.py in this family is
# self-contained (no shared module), and re-deriving a weaker version of an
# already-fixed bug is worse than the duplication.

_RAW_STRING_START = re.compile(r'R"([^(\\\s]{0,16})\(')
_WORDCHAR = re.compile(r'[0-9A-Za-z_]')


def _is_digit_separator(text, i):
    n = len(text)
    if not (i > 0 and i + 1 < n and _WORDCHAR.match(text[i - 1]) and
            _WORDCHAR.match(text[i + 1])):
        return False
    j = i - 1
    while j > 0 and _WORDCHAR.match(text[j - 1]):
        j -= 1
    return text[j].isdigit()


def _strip_comments(text):
    out = []
    i, n = 0, len(text)
    while i < n:
        m = _RAW_STRING_START.match(text, i)
        if m:
            end_marker = ')' + m.group(1) + '"'
            j = text.find(end_marker, m.end())
            end = len(text) if j == -1 else j + len(end_marker)
            out.append(text[i:end])
            i = end
            continue
        c = text[i]
        if c == "'" and _is_digit_separator(text, i):
            out.append(c)
            i += 1
            continue
        if c == '"' or c == "'":
            quote = c
            out.append(c)
            i += 1
            while i < n:
                cc = text[i]
                if cc == '\\' and i + 1 < n:
                    out.append(text[i:i + 2])
                    i += 2
                    continue
                out.append(cc)
                i += 1
                if cc == quote or cc == '\n':
                    break
            continue
        if c == '/' and i + 1 < n and text[i + 1] == '/':
            while i < n and text[i] != '\n':
                out.append(' ')
                i += 1
            continue
        if c == '/' and i + 1 < n and text[i + 1] == '*':
            out.append('  ')
            i += 2
            while i < n and not (text[i] == '*' and i + 1 < n and
                                  text[i + 1] == '/'):
                out.append('\n' if text[i] == '\n' else ' ')
                i += 1
            if i < n:
                out.append('  ')
                i += 2
            continue
        out.append(c)
        i += 1
    return ''.join(out)


# ---------------------------------------------------------------------------


def scan_file(path, text):
    """-> list of (test_name, var) unprotected null-guard rows."""
    if 'BEZEL_TEST' not in text or 'if' not in text:
        return []
    text = _strip_comments(text)
    marks = [(m.start(), f'{m.group(1)}.{m.group(2)}', m.end())
             for m in BEZEL_TEST.finditer(text)]
    out = []
    for i, (start, tname, end) in enumerate(marks):
        stop = marks[i + 1][0] if i + 1 < len(marks) else len(text)
        body = text[end:stop]
        seen_vars = set()
        for bm in PTRBIND.finditer(body):
            var = bm.group(1)
            if var in seen_vars:
                continue
            rest = body[bm.end():]
            gm = _guard_for(var).search(rest)
            if not gm:
                continue  # no guard at all on this var - a different defect
            seen_vars.add(var)
            pre_guard = rest[:gm.start()]
            if _protection_for(var).search(pre_guard):
                continue  # unconditionally checked before the guard - fine
            guard_block = _guard_block_span(rest, gm.end())
            if not ASSERT_CALL.search(guard_block):
                continue  # nothing asserted inside - nothing to lose
            out.append((tname, var))
    return out


def measure(root):
    """-> (total, per_file) where per_file: {relpath: [(test, var), ...]}."""
    per_file = {}
    tests_dir = root / 'tests'
    if not tests_dir.is_dir():
        return 0, {}
    for p in sorted(tests_dir.rglob('*_test.cpp')):
        try:
            text = p.read_text(errors='replace')
        except OSError:
            continue
        rows = scan_file(p, text)
        if rows:
            per_file[str(p.relative_to(root))] = rows
    total = sum(len(v) for v in per_file.values())
    return total, per_file


def read_baseline(path=BASELINE):
    if not path.exists():
        sys.stderr.write('missing baseline: %s\n' % path)
        raise SystemExit(2)
    for line in path.read_text().splitlines():
        s = line.strip()
        if not s or s.startswith('#'):
            continue
        if s.startswith('<<<<<<<') or s.startswith('=======') \
                or s.startswith('>>>>>>>'):
            sys.stderr.write(
                'scripts/null_guard_vacuous.baseline has unresolved merge '
                'conflict markers. Resolve by taking EITHER side (the count '
                'is re-measured), then re-run --update on the merged tree.\n')
            raise SystemExit(2)
        if '=' not in s:
            continue
        k, _, v = s.partition('=')
        if k.strip() == 'count':
            try:
                return int(v.strip())
            except ValueError:
                pass
    sys.stderr.write(
        'scripts/null_guard_vacuous.baseline is missing \'count\'.\n')
    raise SystemExit(2)


def write_baseline(count, path=BASELINE):
    path.write_text('\n'.join([
        '# null_guard_vacuous.baseline - ratchet for issue #3992: a row',
        '# pinning `if (<ptr>) { ...EXPECT... }` with no unconditional',
        '# non-null assertion on <ptr> first executes ZERO assertions when',
        '# the lookup returns null, and gtest still reports [ OK ].',
        '#',
        '# Regenerate with scripts/check_null_guard_vacuous.py --update after',
        '# fixing (protecting) a row.  --update refuses to raise a count.',
        '#',
        '# EXACT, not a ceiling: a count below baseline fails as STALE',
        '# HEADROOM (see the script docstring - same rule as every other',
        '# ratchet in this family).',
        'count = %d' % count,
        '']))


def check(root, baseline_path=BASELINE, stale=True):
    base = read_baseline(baseline_path)
    cur, per_file = measure(root)
    if cur > base:
        return False, [
            'ROSE: baseline %d, tree has %d (+%d). A new row guards '
            '`if (<ptr>) { ...EXPECT... }` with no unconditional non-null '
            'assertion on <ptr> first - see script docstring for the fix.'
            % (base, cur, cur - base)], per_file
    if stale and cur < base:
        return False, [
            'baseline claims %d but the tree has %d - STALE HEADROOM (%+d). '
            'The baseline is exact, not a ceiling: re-run --update on the '
            'tree that fixed these rows so the improvement is banked.'
            % (base, cur, cur - base)], per_file
    return True, [], per_file


# ---------------------------------------------------------------------------
# self-test
# ---------------------------------------------------------------------------

def _mk(root, rel, text):
    p = root / rel
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(text)
    return p


def self_test():
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

        # --- KNOWN LIVE INSTANCE 1 (this script's ground truth): the exact
        # PRE-fix shape of tests/sema/base_member_lookup_test.cpp's
        # interface_member_resolves (fix #3985, commit ed3c050/1b87d8e) -
        # `auto* ma = findMemberAccess(...); if (ma) { ...EXPECT... }` with
        # no unconditional check first.
        _mk(root, 'tests/sema/known_instance_1_base_member_lookup_test.cpp', '''
BEZEL_TEST(sema_base, interface_member_resolves) {
    auto r = parseSemaAll(
        "module M; "
        "interface IDoer public method Do(): int; end IDoer; "
        "class C : IDoer "
        "  public method Do(): int => 1; "
        "  public method M() _ := this.Do; end M; "
        "end C;");
    auto* ma = findMemberAccess(r, "M");
    if (ma) {
        auto* sym = r.ctx->exprSymbol(*ma);
        BEZEL_EXPECT_TRUE(dynamic_cast<MethodSymbol*>(sym) != nullptr);
    }
}
''')

        # --- KNOWN LIVE INSTANCE 2: the exact PRE-fix shape of
        # tests/sema/member_access_resolution_test.cpp's
        # qualified_field_access_resolves (fix #3985, commit a374e15) - same
        # shape, but the fixture itself parsed fine; `ma` was null because
        # `D` was declared before `C`.
        _mk(root, 'tests/sema/known_instance_2_member_access_resolution_test.cpp', '''
BEZEL_TEST(sema_member, qualified_field_access_resolves) {
    auto r = parseSemaAll(
        "module M; "
        "class D public property Y: int; end D; "
        "class C public property X: int; "
        "  public method M() _ := this.X; end M; "
        "end C;");
    auto* ma = firstMemberAccess(r);
    if (ma) {
        auto* sym = r.ctx->exprSymbol(*ma);
        BEZEL_EXPECT_TRUE(dynamic_cast<FieldSymbol*>(sym) != nullptr);
    }
}
''')

        # --- POST-fix shape: the SAME body, now with an unconditional
        # `BEZEL_EXPECT_TRUE(ma != nullptr);` before the guard - the real fix
        # this project actually shipped. Must NOT be flagged.
        _mk(root, 'tests/sema/fixed_shape_test.cpp', '''
BEZEL_TEST(sema_base, derived_inherits_base_method) {
    auto r = parseSemaAll("module M; class Base end Base; class Derived : Base end Derived;");
    auto* ma = findMemberAccess(r, "M");
    BEZEL_EXPECT_TRUE(ma != nullptr);
    if (ma) {
        auto* sym = r.ctx->exprSymbol(*ma);
        BEZEL_EXPECT_TRUE(dynamic_cast<MethodSymbol*>(sym) != nullptr);
    }
}
''')

        # --- Protected via the bare-truthy spelling `BEZEL_EXPECT_TRUE(ma);`
        # (no explicit `!= nullptr`) - equally protective, must NOT flag.
        _mk(root, 'tests/sema/bare_truthy_protected_test.cpp', '''
BEZEL_TEST(sema_base, bare_truthy_protected) {
    auto r = parseSemaAll("module M;");
    auto* ma = findMemberAccess(r, "M");
    BEZEL_EXPECT_TRUE(ma);
    if (ma) {
        BEZEL_EXPECT_TRUE(true);
    }
}
''')

        # --- Protected via a `dynamic_cast<T*>(var) != nullptr` unconditional
        # claim (real corpus shape - tests/sema/call_return_typing_test.cpp:67,
        # checked BEFORE its `if (e) { ... }` guard). dynamic_cast on a null
        # pointer is always null, so this is an unconditional non-null claim
        # on `e` too. Must NOT flag.
        _mk(root, 'tests/sema/dynamic_cast_protected_test.cpp', '''
BEZEL_TEST(sema_call, dynamic_cast_protected) {
    auto r = parseSemaAll("module M;");
    auto* e = firstCallDiscardValue(r);
    BEZEL_EXPECT_TRUE(dynamic_cast<const bezel::ast::CallExpr*>(e) != nullptr);
    if (e) {
        auto* t = r.ctx->exprType(*e);
        BEZEL_EXPECT_TRUE(t && t->primitive == PrimitiveType::Int);
    }
}
''')

        # --- Brace-less single-statement guard (real corpus shape -
        # tests/sema/type_resolution_test.cpp's `unit_type`:
        # `if (ref) BEZEL_EXPECT_TRUE(...);` with no braces at all). Must
        # still be flagged when unprotected.
        _mk(root, 'tests/sema/braceless_guard_test.cpp', '''
BEZEL_TEST(sema_types, braceless_guard) {
    auto r = parseAndResolve("module M;");
    auto* ref = r.ctx->resolvedType(*firstFieldType(r));
    if (ref) BEZEL_EXPECT_TRUE(ref->kind == TypeRef::Kind::Unit);
}
''')

        # --- No guard at all: a different defect (crash-on-null), out of
        # THIS script's scope. Must NOT flag.
        _mk(root, 'tests/sema/no_guard_at_all_test.cpp', '''
BEZEL_TEST(sema_base, no_guard_at_all) {
    auto r = parseSemaAll("module M;");
    auto* ma = findMemberAccess(r, "M");
    auto* sym = r.ctx->exprSymbol(*ma);
    BEZEL_EXPECT_TRUE(sym != nullptr);
}
''')

        # --- Guard block asserts nothing - nothing to lose. Must NOT flag.
        _mk(root, 'tests/sema/empty_guard_body_test.cpp', '''
BEZEL_TEST(sema_base, empty_guard_body) {
    auto r = parseSemaAll("module M;");
    auto* ma = findMemberAccess(r, "M");
    if (ma) {
        doSomeNonAssertingWork(ma);
    }
}
''')

        # --- The C++ if-with-initializer idiom, real in this corpus
        # (tests/sema/choice_sema_classification_35_test.cpp:57/83/135) - a
        # type-narrowing filter, structurally different from a lookup-
        # succeeded guard. The bind is INSIDE the `if (` condition, not its
        # own statement, so PTRBIND must not match it at all. Must NOT flag.
        _mk(root, 'tests/sema/if_with_initializer_idiom_test.cpp', '''
BEZEL_TEST(sema_base, if_with_initializer_idiom) {
    auto r = parseSemaAll("module M;");
    for (const auto& child : r.ctx->allSymbols()) {
        if (auto* t = dynamic_cast<TypeSymbol*>(child.get())) {
            BEZEL_EXPECT_TRUE(t->name().size() > 0);
        }
    }
}
''')

        # --- COMMENT HAZARD (mirrors #3915/#3920): a comment quoting the
        # exact anti-pattern text sits between two tests. The PRECEDING test
        # binds a var named `ma` from a call but never guards it at all (so
        # it is not this shape either way); the comment must not be
        # misattributed to it, and the FOLLOWING (real, protected) test must
        # not be flagged from the comment bleeding into its own window.
        _mk(root, 'tests/sema/comment_hazard_test.cpp', '''
BEZEL_TEST(sema_base, preceding_case_no_guard) {
    auto r = parseSemaAll("module M;");
    auto* ma = findMemberAccess(r, "Unused");
}
// The test below deliberately avoids the anti-pattern:
//     auto* ma = findMemberAccess(r, "M");
//     if (ma) { ...EXPECT... }
// because a null lookup would otherwise execute zero assertions (#3992).
BEZEL_TEST(sema_base, protected_case_after_the_comment) {
    auto r = parseSemaAll("module M;");
    auto* ma = findMemberAccess(r, "M");
    BEZEL_EXPECT_TRUE(ma != nullptr);
    if (ma) {
        BEZEL_EXPECT_TRUE(true);
    }
}
''')

        # --- Companion control: a comment quoting the pattern nearby must
        # NOT mask a row that is GENUINELY unprotected in real code.
        _mk(root, 'tests/sema/bare_row_near_comment_test.cpp', '''
BEZEL_TEST(sema_base, bare_row_near_comment) {
    auto r = parseSemaAll("module M;");
    auto* ma = findMemberAccess(r, "M");
    // NOTE: the row below genuinely uses the ambiguous anti-pattern - it
    // must still be flagged even though this very comment quotes it.
    if (ma) {
        BEZEL_EXPECT_TRUE(true);
    }
}
''')

        total, per_file = measure(root)
        chk('known live instance 1 (base_member_lookup interface_member_resolves) IS flagged',
            'tests/sema/known_instance_1_base_member_lookup_test.cpp' in per_file)
        chk('known live instance 2 (member_access_resolution qualified_field_access_resolves) IS flagged',
            'tests/sema/known_instance_2_member_access_resolution_test.cpp' in per_file)
        chk('the real shipped fix (unconditional check before the guard) is NOT flagged',
            'tests/sema/fixed_shape_test.cpp' not in per_file)
        chk('a bare-truthy EXPECT_TRUE(ma) unconditional check is also protective',
            'tests/sema/bare_truthy_protected_test.cpp' not in per_file)
        chk('a dynamic_cast<T*>(var) != nullptr unconditional check is also protective',
            'tests/sema/dynamic_cast_protected_test.cpp' not in per_file)
        chk('a brace-less single-statement guard (`if (ref) EXPECT(...);`) IS still flagged',
            'tests/sema/braceless_guard_test.cpp' in per_file)
        chk('a pointer used with no guard at all is out of scope (different defect)',
            'tests/sema/no_guard_at_all_test.cpp' not in per_file)
        chk('a guard block asserting nothing is not flagged (nothing to lose)',
            'tests/sema/empty_guard_body_test.cpp' not in per_file)
        chk('the C++ if-with-initializer idiom (type-narrowing filter) is NOT flagged',
            'tests/sema/if_with_initializer_idiom_test.cpp' not in per_file)
        chk('comment hazard: neither test in the comment-adjacent file is misattributed',
            'tests/sema/comment_hazard_test.cpp' not in per_file)
        chk('a genuinely unprotected row near a comment quoting the pattern IS still flagged',
            'tests/sema/bare_row_near_comment_test.cpp' in per_file)
        chk('exactly 4 unprotected rows across the fixture tree (the 2 known '
            'live instances + the near-comment control + the brace-less-guard '
            'control)', total == 4)

        bpath = root / 'baseline'
        write_baseline(total, bpath)
        good, msgs, _ = check(root, bpath, stale=True)
        chk('a tree matching its baseline passes', good and not msgs)

        # A RISE must fail.
        _mk(root, 'tests/sema/new_bad_test.cpp', '''
BEZEL_TEST(sema_base, new_bad) {
    auto r = parseSemaAll("module M;");
    auto* ma = findMemberAccess(r, "M");
    if (ma) {
        BEZEL_EXPECT_TRUE(true);
    }
}
''')
        good, msgs, _ = check(root, bpath, stale=True)
        chk('a new unprotected row FAILS the ratchet', not good)
        chk('the message names a rise', any('ROSE' in m for m in msgs))
        chk('--update REFUSES to bank a rise',
            not check(root, bpath, stale=False)[0])
        (root / 'tests/sema/new_bad_test.cpp').unlink()

        # STALE HEADROOM: a fix that forgot --update.
        (root / 'tests/sema/known_instance_1_base_member_lookup_test.cpp').write_text('''
BEZEL_TEST(sema_base, interface_member_resolves) {
    auto r = parseSemaAll("module M;");
    auto* ma = findMemberAccess(r, "M");
    BEZEL_EXPECT_TRUE(ma != nullptr);
    if (ma) {
        BEZEL_EXPECT_TRUE(true);
    }
}
''')
        good, msgs, _ = check(root, bpath, stale=True)
        chk('a fix that did not --update fails as STALE HEADROOM',
            not good and any('STALE HEADROOM' in m for m in msgs))
        chk('--update ACCEPTS the drop', check(root, bpath, stale=False)[0])
        total2, _ = measure(root)
        write_baseline(total2, bpath)
        chk('re-baselined tree is green again', check(root, bpath, True)[0])
        chk('the fix actually lowered the count', total2 < total)

        # A missing baseline must fail loudly.
        missing = root / 'no-such-baseline'
        try:
            check(root, missing, stale=True)
            chk('a missing baseline fails closed', False)
        except SystemExit as e:
            chk('a missing baseline fails closed', e.code == 2)

    print('check_null_guard_vacuous --self-test: ' + ('PASS' if ok else 'FAIL'))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--strict', action='store_true',
                    help='gate mode: count must EQUAL the baseline')
    ap.add_argument('--self-test', action='store_true',
                    help='prove the detector still detects')
    ap.add_argument('--update', action='store_true',
                    help='re-baseline after fixing rows (refuses to raise)')
    ap.add_argument('--force', action='store_true',
                    help='with --update, bank a rise anyway (deliberate)')
    ap.add_argument('--list', action='store_true',
                    help='list every unprotected row')
    a = ap.parse_args()

    if a.self_test:
        return self_test()

    if a.list:
        total, per_file = measure(ROOT)
        for f, rows in sorted(per_file.items(), key=lambda kv: -len(kv[1])):
            print('%3d  %s' % (len(rows), f))
            for tname, var in rows:
                print('       %s (%s)' % (tname, var))
        print('--- count=%d' % total)
        return 0

    if a.update:
        total, _ = measure(ROOT)
        if not BASELINE.exists():
            write_baseline(total)
            print('created baseline: count=%d' % total)
            return 0
        if not a.force:
            good, msgs, _ = check(ROOT, stale=False)
            if not good:
                sys.stderr.write(
                    'check_null_guard_vacuous --update REFUSES to raise the '
                    'count:\n')
                for m in msgs:
                    sys.stderr.write('  ' + m + '\n')
                sys.stderr.write(
                    '  A rise means a new unprotected null-guard row was '
                    'added. Add an unconditional non-null assertion before '
                    'the guard instead. If the rise is genuinely intended, '
                    're-run with --force and say why in the commit.\n')
                return 1
        write_baseline(total)
        print('re-baselined: count=%d' % total)
        return 0

    good, msgs, per_file = check(ROOT, stale=True)
    if good:
        total, _ = measure(ROOT)
        print('check_null_guard_vacuous: OK (count=%d)' % total)
        return 0
    for m in msgs:
        sys.stderr.write('check_null_guard_vacuous: ' + m + '\n')
    sys.stderr.write(
        "See issue #3992: a row guarding `if (<ptr>) { ...EXPECT... }` with "
        "no unconditional non-null assertion on <ptr> first executes ZERO "
        "assertions when the lookup returns null, and gtest still reports "
        "[ OK ]. Run --list to see every unprotected row.\n")
    return 1 if a.strict else 0


if __name__ == '__main__':
    sys.exit(main())
