#!/usr/bin/env python3
"""check_ambiguous_ran_false.py - ratchet rows that pin a compile-time
REJECTION using only `runAot`'s `.ran` flag (or `hasErrors()` / a bare
severity check), with no check on diagnostic message TEXT and no check that
the diagnostic carries a real, nonzero source LOCATION.

WHY (issue #3892). `runAot`'s `JitResult::ran` is false for FOUR outcomes,
not one (tests/support/aot_support.hpp):
  1. a genuine `CompileOutcome::Rejected`;
  2. `CompileOutcome::Crashed` - the compiler itself died;
  3. the program compiled and linked fine, then died from a RUNTIME signal;
  4. a compile/run TIMEOUT, or a forced-ASan report.
All four are reported as ordinary `Severity::Error` diagnostics at
`SourceLocation{}` (0:0). A test body that pins "the compiler rejects this"
with nothing but `BEZEL_EXPECT_FALSE(r.ran)` (or `hasErrors()`, or a raw
`severity == Error` check) cannot tell outcome 1 apart from 2-4 - it ALSO
passes when the compiled program segfaults, hangs, or trips ASan.

MEASURED, REPRODUCED (not merely described - see the commit body for the
exact probe): a program that compiles cleanly and then stack-overflows at
runtime reports `r.ran=0`, one diagnostic reading "runAot: program terminated
by signal 11", and a bare `BEZEL_EXPECT_FALSE(r.ran)` against it reports
`[ OK ]`.

THIS SCRIPT'S COUNTING CRITERIA (stated explicitly per issue #3892's own
request - a mechanical count over ~35 files of hand-traced "two-hop helper
chains" cannot reproduce the reviewer's manual 60 exactly, and does not need
to; only the CRITERIA need to be stated so the baseline means something):

  A ROW is one (file, BEZEL_TEST body, variable) triple such that:
    - `variable` is bound in that body from a rejection-observing harness
      call (runAot / runAotOut / runAotChecked / runAotCheckedLenient /
      runAotCheckedDisposal), and
    - the body contains at least one "rejection-claim" assertion for that
      variable: `BEZEL_EXPECT_FALSE(variable.ran)`, or
      `BEZEL_EXPECT_TRUE(variable(.diagnostics)?.hasErrors())`, and
    - the body contains NO "protection": neither
        (a) a direct `.message.find(` scan, nor
        (b) a call, with `variable` as an argument, to a helper whose name
            signals a message/location check (a fixed marker-substring list
            covering both this codebase's existing idiom names -
            hasErrorContaining / hasErrorWithRealLocation / hasMsgAtRealLocation
            / hasErrorLocatedAt - and the two this issue promotes -
            failedAtCompileTimeNotRuntime / failedAtCompileTimeWithRealLocation
            / diagnosticMessageContains), nor
        (c) an in-file helper taking a JitResult-shaped parameter that
            ULTIMATELY reads `.message` - either directly in its own body,
            or by DELEGATING (transitively) to a helper that does (fix
            #6538). Same "classify a local predicate by its body, not its
            name" technique scripts/check_vacuous_tests.py uses, extended
            from one hop to the whole visible call graph so a wrapper that
            forwards to a shared assertion is recognised as protective.
            The SKIP-literal bail-out exclusion is carried across each
            delegation edge by its ARGUMENT, so a helper forwarding
            `"SKIP"` stays denied; see `_protective_helpers`,
            nor
        (d) a location check anywhere in the body: both a `.line` and a
            `.column` member compared against 0 (the
            tests/sema/instantiation_hash_container_diag_3796_test.cpp idiom
            this issue names as an equally-sound alternative to a text
            check).

  ONE row per (file, test, variable), not one per macro invocation - a body
  asserting both `!ran` and `hasErrors()` for the same variable is one claim
  about that variable, not two.

  Scope is every `tests/**/*_test.cpp` file (tests/support/ INCLUDED,
  deliberately: tests/support/aot_infra_loud_3007_test.cpp's
  `legacy_negative_assertion_shape` is a knowingly-preserved demonstration of
  exactly this unsound shape for issue #3007's unrelated purpose, and a
  mechanical detector has no way to distinguish "deliberate demonstration"
  from "accidental bug" without a suppression marker this run does not add -
  see the commit body).

WHAT THIS DOES NOT CLAIM. This is a heuristic over source TEXT, not a
type-checked trace of every helper-chain hop (issue #3892's own count needed
manual inspection of two-hop chains like
`sawPureOperatorReject(diagText(r), ...)`). A name-based allowlist plus a
body-classified-helper pass catches the common shapes; a helper defined in a
header this script does not parse, under a name matching none of the markers,
would be missed. That is why the count here differs from the issue's manually
measured 60 - and why FIXING rows (the sweep) is explicitly out of THIS
lane's scope (see #3892 and its filed follow-up).

⚠ DELEGATION-CLOSURE SCOPE (fix #6538). `_protective_helpers` resolves a
helper chain through every helper it can SEE: those defined in the file
under scan, plus those defined in `tests/support/*.hpp` (the same scope
C5 already collects). A forwarder in, say, `tests/codegen/codegen_helpers.hpp`
is still not resolvable, exactly as it was before - the closure widened
WHICH BODY is read, not WHICH FILES. It is also name-keyed, so two
same-named helpers in different translation units are treated as one (the
pre-existing C5 approximation). Both limits under-detect (a missed row is
one fewer defect caught by this ratchet); neither can manufacture
protection, because a name enters the closure only by reading `.message`
itself or by forwarding to a name that already did.

⚠ PREPROCESSOR CONDITIONAL HANDLING (C4 COMPLETION ITEM). The scanner
does not understand preprocessor conditionals in general: only `#if 0`
and `#if false` blocks (the explicit "always false" cases) are blanked
out via _strip_disabled_preprocessor() before the rest of the scan runs
(scan_file). Other shapes - `#if FOO_MACRO`, `#ifdef FOO`, `#if 1`,
`#if true`, the `#else`/`#elif` branches of an `#if 1`/`#if 0` pair,
nested conditionals beyond simple depth counting - are NOT handled. A
row inside any such unhandled conditional IS still counted by this
ratchet, even though the compiler will never see it. Issue #3935
(2026-09-06) names this as a pre-existing, orthogonal limitation -
the C4 completion item records it here rather than fixing it silently.

⚠ SUPPORT-HEADER HELPERS (C5 COMPLETION ITEM). The `_text_checking_
helpers()` classifier only sees the file under scan: a helper defined
in another file - the canonical case is a shared utility in
`tests/support/*.hpp` that issue #3892 itself recommends using -
is INVISIBLE to it. Before C5, a test whose only protection was a
call to such a shared helper was counted; C5 fixes this by ALSO
collecting helpers from `tests/support/*.hpp` once per `measure()`
call and unioning them into the per-file helper set (see
`_support_helpers()`). The same classification rule applies:
positive-exclusion for the SKIP-literal bail-out idiom, default-
protective otherwise. Helpers that do NOT match NAME_MARKERS AND do
NOT live under `tests/support/` (e.g. a per-area shared header like
`tests/codegen/codegen_helpers.hpp`) are still NOT picked up -
deliberate scope, mirroring the LOCALDECL-ONLY/DEBT family's
`tests/`-wide marker search.

THE BASELINE IS EXACT, NOT A CEILING - same rule every ratchet in this file's
family uses (scripts/check_deprecated_jit_oracle.py, scripts/check_localdecl_
only_walkers.py, ...). A count BELOW baseline fails as STALE HEADROOM: a sweep
PR that fixes rows but forgets `--update` silently buys room for the next
regression.

⚠ THE ASYMMETRY IS PARTLY DELIBERATE, PARTLY A CLOSED HOLE (issue #6546).
The skip-marker exclusion is POSITIVE: a guard is denied protective status
only when its needle can be CONFIRMED to be the bail-out marker, and
anything this module cannot confirm defaults to PROTECTIVE. That fail-
toward-under-flagging posture is DELIBERATE and unchanged - an inflated
baseline never reaches zero and people stop trusting it, whereas a missed
row is one fewer defect caught by THIS ratchet, not a false alarm.

Three spellings used to defeat the confirmation, all classifying a bail-out
guard as PROTECTIVE, and all now closed: the needle behind a NAMED CONSTANT
(`.find(kSkip)`), the needle split across ADJACENT LITERALS (`"SK" "IP"`,
which the compiler concatenates), and the marker at the CALL SITE of a
needle-forwarding helper (`skipped(r, "SKIP")`, invisible to an edge rule
that only reads the edge inside the helper). See `_skip_constants`,
`_fold_literals`, `_resolve_literals` and `_protected`.

Those three closures are deliberately NOT symmetric, and the asymmetry is a
decision rather than an oversight:

  * `_text_checking_helpers` - the DIRECT rule as an IMPORTED FUNCTION - is
    still the FROZEN pre-#6546 rule and compares the argument text
    VERBATIMLY. Two sibling ratchets import it
    (`check_toolchain_skip_surface.py`, which uses its COMPLEMENT as its
    SKIP-only set, and `check_timeout_satisfiable_negatives.py`), and
    hardening it in place would move the first one's banked
    `absorbing_sites` count, which may only FALL. `_text_checking_helpers`
    therefore keeps its hole, deliberately and with the reason recorded
    here and at its own def.
  * `_protective_helpers` - the layer ABOVE it, and the only classifier
    THIS ratchet's own row count uses - is fully hardened, with the
    named-constant table threaded into both its leaf test and its edge
    test.
  * `_protected` gained an optional fourth argument defaulting to empty,
    so `check_timeout_satisfiable_negatives.py`'s three-argument call is
    unchanged.

Both remaining gaps under-detect (a missed row), which is the safe
direction, and neither can manufacture protection: a name enters the
closure only by reading `.message` itself or by forwarding to a name that
already did, and `_skip_constants` collects ONLY constants whose value IS
the skip marker, so adding an entry can only ever DENY protection.

Usage:  scripts/check_ambiguous_ran_false.py [--strict] [--self-test]
                                             [--update [--force]] [--list]
"""
import argparse
import pathlib
import re
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
BASELINE = ROOT / 'scripts' / 'ambiguous_ran_false.baseline'
GRANDFATHER = ROOT / 'scripts' / 'ambiguous_ran_false.grandfather'

# A result binding from a harness call whose `.ran == false` is ambiguous
# between reject / crash / runtime-signal / timeout / ASan (issue #3892).
# Allows an optional `bezel::test::` qualification (file_io_644_test.cpp's
# probe file used the qualified form before fix #3892's `using` declaration).
BIND = re.compile(
    r'\b(?:auto|JitResult|bezel::test::JitResult)\s+(\w+)\s*=\s*'
    r'(?:bezel::test::)?'
    r'(?:runAot|runAotOut|runAotChecked|runAotCheckedLenient|'
    r'runAotCheckedDisposal)\s*\(')

BEZEL_TEST = re.compile(r'BEZEL_TEST\s*\(\s*(\w+)\s*,\s*(\w+)\s*\)', re.S)

# An in-file predicate over a JitResult: `bool NAME(<...JitResult...>) {`.
HELPER = re.compile(r'\b(?:static\s+|inline\s+)*bool\s+(\w+)\s*\(([^;{)]*)\)\s*\{')

# Marker substrings for a CALLEE NAME that signals "this checks diagnostic
# message text or location" - covers both this codebase's pre-existing idiom
# names and the ones issue #3892 promotes into tests/support/
# aot_result_asserts.hpp. Matched as a substring of the identifier
# immediately before an opening paren.
NAME_MARKERS = (
    'ErrorContaining', 'RealLocation', 'LocatedAt', 'NotRuntime',
    'hasMsgAt', 'diagnosticMessageContains',
)


# The literal the toolchain-unavailable bail-out idiom hardcodes. Found under
# BOTH `wasSkipped` and `aotWasSkipped` spellings (248 combined in-file
# copies) - always exactly this fixed literal, e.g.:
#   bool wasSkipped(const JitResult& r) {
#       if (r.ran) return false;
#       for (const auto& d : r.diagnostics.diagnostics())
#           if (d.message.find("SKIP") != std::string::npos) return true;
#       return false;
#   }
# This reads `.message` and would otherwise be misclassified as a text check
# that PROTECTS a `BEZEL_EXPECT_FALSE(r.ran)` row - but it is orthogonal to
# the reject-vs-crash/signal/timeout/ASan discriminator issue #3892 is about;
# it only tells a caller "the toolchain to run this program at all was
# unavailable", not "this failed because the compiler rejected it". A
# `BEZEL_TEST` guarded ONLY by `if (wasSkipped(r)) return;` before
# `BEZEL_EXPECT_FALSE(r.ran)` is measurably as unprotected as a bare row.
SKIP_LITERAL = '"SKIP"'

# The CONTENT of that literal - what a needle denotes once the surrounding
# quotes, and any adjacent-literal concatenation, have been resolved.
_SKIP_CONTENT = 'SKIP'

# ── issue #6546: resolving a needle that is not spelled `"SKIP"` verbatim ──
#
# The SKIP-literal exclusion is a POSITIVE confirmation, so a needle this
# file cannot positively confirm to be the skip marker is read as a REAL
# needle. #6546 names three ordinary C++ spellings that used to defeat it,
# each classifying a bail-out guard as PROTECTIVE (all three fail toward
# under-flagging):
#
#   1. the needle behind a NAMED CONSTANT (`static const char* kSkip =
#      "SKIP";` then `.find(kSkip)`) - nothing to see in the argument text;
#   2. ADJACENT-LITERAL CONCATENATION (`"SK" "IP"`, which the compiler
#      joins into one literal) - two literals, neither of them `"SKIP"`;
#   3. the SKIP literal at the CALL SITE, not in the helper body
#      (`skipped(r, "SKIP")` where `skipped` forwards a needle parameter).
#
# `_fold_literals` / `_resolve_literals` close (1) and (2) for BOTH rules;
# the call-site check in `_protected` closes (3).

# A run of one or more ADJACENT double-quoted literals. `_STRING_LITERAL`
# finds each piece; `_fold_literals` joins a run the way the C++ compiler
# does, so `"SK" "IP"` folds to the single content `SKIP`.
_ADJACENT_LITERAL_RUN = re.compile(r'(?:"(?:[^"\\]|\\.)*"\s*)+')

# A named string constant initialised from an adjacent-literal run:
#   static const char* kSkip = "SKIP";
#   static constexpr char kSkip[] = "SKIP";
#   const std::string_view kSkip = "SKIP";
#   static auto kSkip = "SKIP";
# Only constants whose initialiser folds to the skip marker are collected
# (see `_skip_constants`), so this regex is deliberately permissive about
# the declared TYPE and strict about the value.
_NAMED_STRING_CONST = re.compile(
    r'\b(?:static\s+|constexpr\s+|inline\s+|extern\s+|const\s+)*'
    r'(?:const\s+)?(?:char|auto|std::string|std::string_view|string_view)'
    r'\s*\**\s*(\w+)\s*(?:\[[^\]]*\])?\s*=\s*'
    r'((?:"(?:[^"\\]|\\.)*"\s*)+)')


_HEXDIGITS = '0123456789abcdefABCDEF'


def _unescape(literal_with_quotes):
    """The CONTENT of one string literal: quotes stripped and the C++ escape
    forms resolved, so `needle` is compared by VALUE and not by spelling.

    Handles \\\\ \\" \\' \\n \\r \\t \\a \\b \\f \\v \\0 and the numeric forms
    \\xHH (greedy, exactly as C++ phase 5 - the escape absorbs every
    following hex digit, so `"\\x53KIP"` is `SKIP` but `"\\x53AB"` is ONE
    character) and \\NNN octal (up to three digits). Only enough to decide
    whether a needle IS the skip marker; an escape form not listed here is
    left verbatim, which can only fail to CONFIRM - the under-flagging
    direction this module documents everywhere.

    Without the numeric forms, `"\\x53KIP"` and `"\\123KIP"` are yet two more
    spellings of the same bail-out marker that the confirmation cannot see,
    which is the re-entry #6546 exists to prevent: the point is that the
    VALUE is SKIP, not that it is spelled a particular way."""
    s = literal_with_quotes[1:-1]
    out = []
    i, n = 0, len(s)
    simple = {'\\': '\\', '"': '"', "'": "'", 'n': '\n', 'r': '\r',
              't': '\t', 'a': '\a', 'b': '\b', 'f': '\f', 'v': '\v',
              '?': '?'}
    while i < n:
        c = s[i]
        if c != '\\' or i + 1 >= n:
            out.append(c)
            i += 1
            continue
        e = s[i + 1]
        if e in simple:
            out.append(simple[e])
            i += 2
            continue
        if e == 'x':
            j = i + 2
            while j < n and s[j] in _HEXDIGITS:
                j += 1
            if j > i + 2:
                out.append(chr(int(s[i + 2:j], 16) & 0xFF))
                i = j
                continue
        elif e in '01234567':
            j = i + 1
            while j < n and j < i + 4 and s[j] in '01234567':
                j += 1
            out.append(chr(int(s[i + 1:j], 8) & 0xFF))
            i = j
            continue
        # An unrecognised (or truncated) escape: pass both characters
        # through verbatim, so the value simply fails to match later.
        out.append(c)
        out.append(e)
        i += 2
    return ''.join(out)


def _fold_literals(text):
    """The CONTENT of every adjacent-literal RUN in `text`, in order.
    `"SK" "IP"` -> `['SKIP']`; `"a" ", " "b"` -> `['a', ', b']`. This is the
    C++ adjacent-literal concatenation rule (phase 6) reduced to what a text
    scan can do: join the pieces of each run, keep the runs separate."""
    out = []
    for m in _ADJACENT_LITERAL_RUN.finditer(text):
        out.append(''.join(_unescape(p) for p in _STRING_LITERAL.findall(m.group(0))))
    return out


def _resolve_literals(args, consts=()):
    """Every string value `args` can denote: the contents of its adjacent-
    literal runs, PLUS the value of any identifier in `consts` (the named-
    constant shape). Order is source order, which is what the exactness
    argument in `_edge_is_skip_only` needs.

    An identifier that is NOT in `consts` contributes nothing, so an
    argument text this module cannot resolve contributes no value - and a
    caller that requires at least one value therefore falls on the
    protective side, the documented under-flagging direction."""
    out = []
    pos = 0
    for m in _ADJACENT_LITERAL_RUN.finditer(args):
        for ident in re.findall(r'\b\w+\b', args[pos:m.start()]):
            if ident in consts:
                out.append(consts[ident])
        out.extend(_fold_literals(m.group(0)))
        pos = m.end()
    for ident in re.findall(r'\b\w+\b', args[pos:]):
        if ident in consts:
            out.append(consts[ident])
    return out


def _skip_constants(text):
    """{name: value} for every named string constant in `text` whose
    initialiser folds to the skip marker - the shape-1 table.

    Deliberately collects ONLY skip-marker-valued constants. A constant
    holding any other needle contributes no entry, so a call passing it
    resolves to no value and stays protective; and because a skip-marker
    constant is only ever consulted to DENY protection, there is no path
    by which adding an entry could promote a helper."""
    out = {}
    for m in _NAMED_STRING_CONST.finditer(text):
        vals = _fold_literals(m.group(2))
        if len(vals) == 1 and vals[0] == _SKIP_CONTENT:
            out[m.group(1)] = vals[0]
    return out


# A local alias for `.message`: `const std::string& m = d.message;` (or
# `auto`/`std::string_view`, with or without `&`/`const`). A helper may read
# `.message` through such an alias rather than spelling `.message.find(`
# directly - e.g. tests/sema/lambda_param_no_target_type_reject_3756_test.cpp's
# `locatedAt`: `const std::string& m = d.message; ... m.find(tail)`.
ALIAS_DECL = re.compile(
    r'\b(?:const\s+)?(?:std::string|std::string_view|auto)\s*&?\s*(\w+)\s*=\s*'
    r'[\w.]*\.message\b')


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


# A raw-string-literal opener: `R"delim(` where `delim` is 0-16 chars, none
# of them `(`, backslash or whitespace (the C++ grammar's own limit).
_RAW_STRING_START = re.compile(r'R"([^(\\\s]{0,16})\(')

# Per-test suppression marker - C1 completion item. Mirrors the
# LOCALDECL-ONLY marker (scripts/check_localdecl_only_walkers.py:75-100
# and :145), itself ported from #3590 as the family-wide shape: a prose
# marker on its own line that, with a reason after the colon, exempts the
# next row the detector would otherwise flag. Used here to drop a row
# deliberately preserved for some OTHER purpose (the FAIL-WITHOUT
# vehicle in tests/support/aot_infra_loud_3007_test.cpp is the canonical
# case: a knowingly-preserved demonstration of exactly the unsafe shape,
# for #3007's unrelated fix) from the count, so the baseline can reach
# zero in principle.
AMBIGUOUS_OK_MARKER = re.compile(r'//\s*AMBIGUOUS-RAN-FALSE-OK:\s*\S')

# Per-site debt marker - C2 completion item. Sibling to AMBIGUOUS-RAN-
# FALSE-OK: like LOCALDECL-DEBT, it does NOT remove the site from the
# count (the site is still an offender, still baselined, still meant to
# be fixed) but justifies a rise ABOVE the per-file ceiling in
# scripts/ambiguous_ran_false.grandfather. The `#NNNN` must sit
# immediately after the colon; a marker with a reason but no issue
# number is malformed and justifies nothing. `--force` alone is not
# enough - a deliberate rise past the ceiling still needs one of these
# at every excess site, mirroring check_localdecl_only_walkers.py:80-96.
AMBIGUOUS_DEBT_MARKER = re.compile(r'//\s*AMBIGUOUS-RAN-FALSE-DEBT:\s*(\S.*)$')
AMBIGUOUS_DEBT_ISSUE = re.compile(r'^#(\d+)\b')

# A line-scoped BEZEL_TEST regex used by the marker-attach logic to find
# the body the suppression marker attaches to. The body-identification
# machinery below (scan_file) uses re.S so it can match a wrapped
# argument list, but per-line scanning here does not need that, and the
# `\w` class never spans a newline anyway.
BEZEL_TEST_LINE = re.compile(r'BEZEL_TEST\s*\(\s*\w+\s*,\s*(\w+)\s*\)')

# An identifier/digit character - used to find the run of characters
# immediately preceding a `'`, to tell a C++14 digit separator (`1'000`,
# `0xFF'FF`) apart from a genuine char-literal-opening `'` - fix #3915,
# narrowed by fix #3920 (see _is_digit_separator: flanking on both sides is
# necessary but not sufficient - an identifier abutting a char literal,
# e.g. `return'a'`, has the same shape and is valid C++).
_WORDCHAR = re.compile(r'[0-9A-Za-z_]')


# Partial numeric-literal grammar for the C3 completion item. The
# pp-number grammar (C++14+) allows `9x` as a valid pp-number (digit,
# then identifier-nondigit), so a strict pp-number check would NOT
# reject the malformed `9x` prefix. The detector instead validates
# against a partial NUMERIC-LITERAL grammar: a token that begins with
# a digit (or `0x`/`0X` for hex, `0b`/`0B` for binary, `0o`/`0O` for
# octal) and contains only digits and a small set of exponent/sign
# markers. This rejects `9x` (the `x` is not a valid digit or exponent
# letter) without rejecting `0xFF` (`FF` are valid hex digits) or
# `1e+10` (the `e+` exponent is valid). See issue #3935 (2026-09-06).
_HEX_DIGITS = set('0123456789abcdefABCDEF')
_BIN_DIGITS = set('01')
_OCT_DIGITS = set('01234567')
_DEC_EXTRAS = set('eEpP+-')


def _is_numeric_literal_run(run):
    """True if `run` (the contiguous word-char run immediately before a
    `'` candidate) is a syntactically coherent numeric-literal prefix.

    Accepts:
    - decimal: ``1``, ``123``, ``1e+10``, ``1.5``
    - hex:     ``0xFF``, ``0XFF'FF`` (note `'` is not in the run; the
      run is the chars BEFORE `'` only)
    - binary:  ``0b1010``, ``0B1010``
    - octal:   ``0o755``, ``0O755``

    Rejects anything with a non-numeric, non-exponent character anywhere
    in the run, e.g. `9x`, `1a`, `2ppx` (the `x` in `9x'y'` is the C3
    bug: a numeric-literal can NEVER have an identifier-nondigit like
    `x` immediately after a decimal digit). Partial implementation:
    no enforcement of `e`/`E`/`p`/`P` requiring a following sign, no
    enforcement of `.` position rules - the goal is "reject malformed
    prefixes that happen to start with a digit", not "parse every
    numeric literal in the C++ standard".
    """
    if not run:
        return False
    # Hex / binary / octal: 0{xXbBoO} prefix
    if len(run) >= 2 and run[0] == '0' and run[1] in 'xX':
        return all(c in _HEX_DIGITS for c in run[2:])
    if len(run) >= 2 and run[0] == '0' and run[1] in 'bB':
        return all(c in _BIN_DIGITS for c in run[2:])
    if len(run) >= 2 and run[0] == '0' and run[1] in 'oO':
        return all(c in _OCT_DIGITS for c in run[2:])
    # Decimal: digits, e/E (exponent), p/P (hex exponent), +/- (sign)
    for c in run:
        if c.isdigit():
            continue
        if c in _DEC_EXTRAS:
            continue
        return False
    return True


def _is_digit_separator(text, i):
    """True if the `'` at `text[i]` is a C++14 digit separator (`1'000`,
    `0xFF'FF`) rather than the start of a char literal that merely abuts an
    identifier character on both sides (`return'a'`, `x'y'`) - fix #3920,
    tightened by fix-C3 (issue #3935) to also reject malformed digit-
    leading runs like `9x'y'`.

    Fix #3915 round 2 flagged a `'` as a separator whenever BOTH neighbours
    were word characters. That is necessary but not sufficient: an
    identifier ending in a letter immediately followed by a char literal
    has exactly the same shape and is legal C++ (`int f() { return'a'; }`
    compiles). The two are distinguished by what kind of TOKEN sits
    immediately before the `'`: a numeric-literal token can only START
    with a digit (0-9), while a C++ identifier can never start with one
    (only a letter or `_`, per the identifier grammar). Walking the
    contiguous run of word characters before `i` back to its start and
    checking whether that start character is a digit therefore tells a
    digit separator (`1'000` -> run "1" starts with '1'; `0xFF'FF` -> run
    "0xFF" starts with '0') apart from an identifier abutting a char
    literal (`return'a'` -> run "return" starts with 'r'; `x'y'` -> run
    "x" starts with 'x') without needing a full numeric-literal grammar.

    fix-C3 (issue #3935) tightens this further: the FIRST-char-is-digit
    check is necessary but not sufficient - a malformed word-char run
    that happens to START with a digit (e.g. `9x` in `9x'y'`) but is not
    actually a numeric-literal prefix (the `x` cannot appear in any
    numeric literal) was still misclassified, with the subsequent `'`
    then desyncing quote parity and hiding any trailing `//` comment.
    The C3 check additionally validates the run against a partial
    numeric-literal grammar - issue #3935 calls this "low priority" and
    "unreachable in compiling C++", but the gap matters inside dead/
    disabled text (the #if-0/-false block discussed by C4).
    """
    n = len(text)
    if not (i > 0 and i + 1 < n and _WORDCHAR.match(text[i - 1]) and
            _WORDCHAR.match(text[i + 1])):
        return False
    j = i - 1
    while j > 0 and _WORDCHAR.match(text[j - 1]):
        j -= 1
    if not text[j].isdigit():
        return False
    # C3: validate the run against the partial numeric-literal grammar.
    # Reject malformed digit-leading runs like `9x` in `9x'y'`.
    return _is_numeric_literal_run(text[j:i])


# C4 COMPLETION ITEM: preprocessor-conditional handling. The scanner
# does not understand preprocessor conditionals in general, but the two
# explicit "always false" cases (`#if 0` and `#if false`) are bounded
# enough to safely skip - they cannot be re-enabled by macro
# definitions, so blanking their contents out for the ratchet's purpose
# is conservative (the compiler would have skipped them anyway). Other
# shapes (`#if FOO_MACRO`, `#ifdef FOO`, `#if 1`, `#if true`, the
# `#else` / `#elif` branches of an `#if 1`/`#if 0` pair) are NOT
# handled and remain a known, documented limitation.
_IF0_DIRECTIVE = re.compile(r'^\s*#\s*if\s+(?:0|false)\b', re.M)
_IF_DIRECTIVE = re.compile(r'^\s*#\s*if(?:def|ndef)?\b', re.M)
_ENDIF_DIRECTIVE = re.compile(r'^\s*#\s*endif\b', re.M)


def _strip_disabled_preprocessor(text):
    """Blank out the contents of `#if 0` / `#if false` blocks (C4).
    The directives themselves stay in place so the file's overall
    structure is preserved for any tool that reads it; only the BODY
    of the disabled block is blanked (newlines preserved, so offset
    math elsewhere is unaffected). The matching `#endif` (with simple
    `#if` / `#endif` depth counting so a nested conditional is also
    handled) is left in place too. The detection result is conservative:
    the C++ preprocessor would have skipped these blocks anyway, so
    blanking them for the ratchet's purpose matches what the COMPILER
    sees (which is what the ratchet is supposed to mirror).

    NOT handled (see module docstring "PREPROCESSOR CONDITIONAL
    HANDLING"):
    - `#if FOO_MACRO` (any macro test, including `#if true` / `#if 1`)
    - `#ifdef FOO` / `#ifndef FOO`
    - `#else` / `#elif` branches of any conditional
    - `#if 0` blocks containing further preprocessor directives that
      themselves contain nested `#if 0` (depth counting handles the
      straightforward nesting case; pathological shapes do not)

    Issue #3935 (2026-09-06) names this as a pre-existing, orthogonal
    limitation. The C4 completion item handles only the simple
    always-false cases and documents the rest.
    """
    out = list(text)
    n = len(text)
    # Process line by line. For each line that opens an `#if 0` /
    # `#if false` block, blank out from the end of that line to the
    # start of the matching `#endif`.
    pos = 0
    while pos < n:
        nl = text.find('\n', pos)
        line_end = nl if nl >= 0 else n
        line = text[pos:line_end]
        if _IF0_DIRECTIVE.match(line):
            # Find matching #endif with depth counting
            depth = 1
            k = line_end + 1
            while k < n and depth > 0:
                k_nl = text.find('\n', k)
                k_line_end = k_nl if k_nl >= 0 else n
                k_line = text[k:k_line_end]
                if _IF_DIRECTIVE.match(k_line):
                    depth += 1
                elif _ENDIF_DIRECTIVE.match(k_line):
                    depth -= 1
                    if depth == 0:
                        # Blank from line_end (exclusive of newline)
                        # up to k (the matching #endif line's start),
                        # so the BODY of the disabled block is gone
                        # but the `#if` and `#endif` directives stay
                        # visible.
                        for idx in range(line_end + 1, k):
                            if out[idx] != '\n':
                                out[idx] = ' '
                        pos = k_line_end + 1
                        break
                k = k_line_end + 1
            else:
                # Unmatched `#if 0` - bail without blanking anything.
                pos = line_end + 1
            continue
        pos = line_end + 1
    return ''.join(out)


def _strip_comments(text):
    """Return `text` with every `//` and `/* */` comment SPAN replaced by
    spaces (newlines preserved, so no OTHER offset math in this file needs
    to change), so a comment's contents can never be mistaken for live code
    by any regex that runs after this - fix #3915.

    Issue #3915: `scan_file` used to delimit a test body as "everything up
    to the next `BEZEL_TEST(`", so a comment quoting the literal text
    `BEZEL_EXPECT_FALSE(r.ran)` (written to explain why a test deliberately
    avoids that anti-pattern) bled into the PRECEDING test's scan window and
    was misattributed to it. A comment living INSIDE a test's own braces has
    the same problem one level down: it is not live code either way, no
    matter which test's window it falls in.

    A `"..."` string literal and a `'...'` character literal are scanned
    with backslash-escape awareness so a literal containing `//` or `/*`
    (a URL, a path, a diagnostic-message fragment) is never misread as
    starting a comment. A `R"delim(...)delim"` RAW string literal (used
    throughout tests/ to embed whole Bezel programs, which use `//` as
    THEIR OWN comment syntax per spec §1.5) is recognised and copied
    through completely untouched as one opaque unit, precisely so an
    embedded Bezel-level `//` comment is never mistaken for a C++ one and
    stripped out from under a real test body.

    A `'` is NOT always a character-literal opener: C++14 digit separators
    (`1'000`, `0xFF'FF`) reuse the same character. See _is_digit_separator
    for how those are told apart from a genuine char-literal-opening `'`
    (fix #3915, narrowed by fix #3920: flanking by word characters on both
    sides is necessary but not sufficient - `return'a'` has the same shape
    and is valid C++, so the preceding TOKEN must actually be numeric). A
    `'` recognised as a digit separator is passed through as an ordinary
    character rather than toggling char-literal mode - otherwise everything
    after it on the same line (crucially, a same-line `//` comment) would be
    swallowed as "inside a string" and NOT stripped, resurrecting the exact
    #3915 misattribution this function exists to eliminate.
    """
    out = []
    i, n = 0, len(text)
    while i < n:
        m = _RAW_STRING_START.match(text, i)
        if m:
            end_marker = ')' + m.group(1) + '"'
            j = text.find(end_marker, m.end())
            end = len(text) if j == -1 else j + len(end_marker)
            out.append(text[i:end])  # opaque - copied verbatim, never scanned
            i = end
            continue
        c = text[i]
        if c == "'" and _is_digit_separator(text, i):
            # C++14 digit separator (`1'000`, `0xFF'FF`), not a char-literal
            # opener - fix #3915 review round 2, narrowed by fix #3920 (an
            # identifier abutting a char literal, e.g. `return'a'`, has the
            # same both-sides-wordchar shape but is NOT a digit separator -
            # see _is_digit_separator). Pass it through unchanged rather
            # than entering char-literal mode, or the rest of THIS line
            # (including a same-line `//` comment) would wrongly be treated
            # as still being inside a string and left unstripped.
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
                    break  # closed normally, or bail out at an unterminated EOL
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


def _balanced_paren(text, open_paren_idx):
    """Return the text between the '(' at open_paren_idx and its match,
    treating parens INSIDE a `"..."` string literal as inert. Without this a
    diagnostic-message literal that itself contains '(' or ')' (e.g.
    `"...`OfType<" + u + ">()`..."` in
    tests/codegen/oftype_removal_completeness_3440_test.cpp's `removedOfType`)
    can desynchronise a naive character count."""
    depth, in_str, i, n = 0, False, open_paren_idx, len(text)
    while i < n:
        c = text[i]
        if in_str:
            if c == '\\':
                i += 1  # skip the escaped character entirely
            elif c == '"':
                in_str = False
        else:
            if c == '"':
                in_str = True
            elif c == '(':
                depth += 1
            elif c == ')':
                depth -= 1
                if depth == 0:
                    return text[open_paren_idx + 1:i]
        i += 1
    return text[open_paren_idx + 1:]


def _message_find_call_args(body):
    """Every `<message-derived>.find(...)` call's raw argument TEXT in
    `body` - `.message.find(...)` directly, or `<alias>.find(...)` for an
    ALIAS_DECL-recognised local alias of `.message`. Argument text is
    returned RAW (untouched, whitespace-trimmed only) precisely so the
    caller does not need to understand concatenation/expression syntax -
    only whether it is EXACTLY the `"SKIP"` literal or not."""
    names = {'message'} | {a.group(1) for a in ALIAS_DECL.finditer(body)}
    call_start = re.compile(
        r'\b(?:' + '|'.join(re.escape(n) for n in names) + r')\s*\.\s*find\s*\(')
    args = []
    for m in call_start.finditer(body):
        args.append(_balanced_paren(body, m.end() - 1).strip())
    return args


def _helper_bodies(text):
    """{name: [body, ...]} for every `bool NAME(...)` taking a
    JitResult-shaped parameter defined in `text`. A name can appear more
    than once (an overload set, or the same helper name defined in several
    translation units - the tree has several), so each body is kept and the
    name is treated as protective if ANY visible body is."""
    out = {}
    for m in HELPER.finditer(text):
        if 'JitResult' not in m.group(2):
            continue
        out.setdefault(m.group(1), []).append(
            _balanced_body(text, m.end() - 1))
    return out


def _is_directly_text_checking(body, consts=None):
    """The DIRECT classification rule, factored out of
    _text_checking_helpers so both the classifier and the delegation
    closure below apply the SAME rule to a leaf body.

    A body reads diagnostic message text when it contains a `.message`
    (or a recognised `.message` ALIAS) whose `.find(...)` argument is not
    the fixed `"SKIP"` literal. A body whose every recognised `.find(...)`
    is exactly `"SKIP"` is the toolchain-unavailable bail-out idiom and is
    DENIED protective status (SKIP_LITERAL above).

    `consts` (issue #6546). OMITTED - the default - means the FROZEN
    pre-#6546 rule: the argument text must be VERBATIMLY `"SKIP"`, compared
    as a string. `_text_checking_helpers` calls it that way and must keep
    calling it that way, because two sibling ratchets import that function
    and one of them uses its COMPLEMENT as its SKIP-only set; widening it
    in place would move that ratchet's banked `absorbing_sites` count
    (see `_text_checking_helpers` and #6538).

    A `consts` mapping switches the rule to the RESOLVED one, which
    recognises the two additional #6546 spellings of the skip marker: the
    needle behind a named constant (`.find(kSkip)`) and the needle split
    across adjacent literals (`.find("SK" "IP")`, which the compiler
    concatenates into one). `_protective_helpers` - the layer ABOVE
    `_text_checking_helpers`, and the only classifier this ratchet's own
    row count uses - always passes one, so the direct rule is hardened
    exactly where this ratchet decides. Both spellings can only ever make
    a body DENY protection, never grant it, so the resolution cannot
    promote a helper."""
    if '.message' not in body:
        return False
    finds = _message_find_call_args(body)
    if consts is None:
        if finds and all(a == SKIP_LITERAL for a in finds):
            return False
        return True
    if finds and all(_resolve_literals(a, consts) == [_SKIP_CONTENT]
                     for a in finds):
        return False
    return True


# One `NAME(` call site in a helper body. The CALLEE NAME plus the call's
# raw argument text - the argument text is what lets the SKIP literal be
# tracked ACROSS a delegation edge (see _edge_is_skip_only).
_CALL_SITE = re.compile(r'\b([A-Za-z_]\w*)\s*\(')

# Every double-quoted string literal in a piece of argument text, as the
# literal INCLUDING its quotes (so it can be compared to SKIP_LITERAL).
_STRING_LITERAL = re.compile(r'"(?:[^"\\]|\\.)*"')


def _edge_is_skip_only(args, consts=()):
    """True if a delegation edge's argument text passes ONLY the skip marker
    as string arguments - i.e. the callee is being handed the skip marker and
    nothing else.

    This is what keeps the delegation closure from over-widening. The
    canonical shape is `tests/support/aot_leak_detection_4638_test.cpp`'s

        bool skipped(const JitResult& r) {
            return bezel::test::diagnosticMessageContains(r, "SKIP");
        }

    which DELEGATES to a genuinely text-checking shared assertion and is
    nonetheless a SKIP-ONLY bail-out guard - exactly the shape the SKIP_LITERAL
    exclusion exists to deny. Without argument-sensitive edge classification
    it would be promoted to protective and real absorbing sites would stop
    being counted.

    Mirrors the DIRECT rule's exactness: a needle that merely CONTAINS the
    substring SKIP (`"toolchain SKIP"`) is a real needle and does NOT count
    as a skip-only edge, so the closure still fails toward under-flagging
    rather than over-flagging.

    `consts` (issue #6546) threads the named-constant table through, so an
    edge passing `diagnosticMessageContains(r, kSkip)` is recognised as
    skip-only exactly as one passing the literal is. Adjacent-literal
    concatenation needs no parameter - `_resolve_literals` folds `"SK" "IP"`
    unconditionally - so BOTH sides of #6546's shapes 1 and 2 are closed
    here as well as in the DIRECT rule."""
    values = _resolve_literals(args, consts)
    return bool(values) and all(v == _SKIP_CONTENT for v in values)


def _protective_helpers(local_bodies, support_bodies, consts=None):
    """Set of helper names that ULTIMATELY check diagnostic message text -
    either directly (their own body reads `.message` with a non-SKIP needle)
    or by DELEGATING, transitively, to a helper that does.

    WHY DELEGATION (issue #6538). The direct rule reads a helper's OWN body
    and looks for `.message` there, so it cannot see through a helper that
    forwards to the shared assertion in `tests/support/
    aot_result_asserts.hpp`. `tests/sema/dict_range_index_6500_test.cpp`'s
    `rejectedWith` is the canonical shape:

        bool rejectedWith(const JitResult& r, const char* msg) {
            return failedAtCompileTimeWithRealLocation(r) &&
                   diagnosticMessageContains(r, msg);
        }

    Both halves pin the diagnostic WORDING, but no `.message` read is
    visible in `rejectedWith`'s own body, so it was denied protective
    status and its test was wrongly reported as an unprotected bare-`!ran`
    row - taking the gate-enforced ratchet RED over a row that is in fact
    correct. The question the ratchet really asks is "does this helper
    ULTIMATELY check message text?", and that is answered by FOLLOWING the
    delegation, not by listing known helper names: an allowlist of
    `diagnosticMessageContains` and friends would have to be re-extended
    every time a new wrapper appears, and would miss a wrapper whose own
    name says nothing.

    MEASURED on the tree at this revision, six helpers across five files
    were denied protective status for exactly this reason
    (`rejectedWith`, `rejectedWithNeedle`, `diagnosticAbsent`,
    `rejectedAt`, `hasAnyObjectTrioDiag`, and the #3892 header's own
    `failedAtCompileTimeNotRuntime`), so this is a family-wide defect,
    not one file's.

    The fixpoint is monotone (the protective set only ever grows) so it
    terminates in at most len(helpers) passes. Mutual recursion between two
    helpers cannot manufacture protection: neither is in the seed set, so
    neither is ever added, and both stay denied.

    Both directions are pinned by self-test fixtures:
      - a helper delegating to a text-checking assertion IS protective;
      - a helper delegating to something that does NOT check text, and a
        helper delegating with the SKIP literal, are NOT.

    `consts` (issue #6546) is the file's named-skip-constant table, threaded
    into BOTH the leaf test and the edge test so the three #6546 spellings
    of the bail-out marker are denied here as well as in the DIRECT rule.
    `None` (the default) selects the frozen pre-#6546 behaviour; every
    caller inside this module passes a real table."""
    bodies = {}
    for src in (support_bodies, local_bodies):
        for name, bs in src.items():
            bodies.setdefault(name, []).extend(bs)
    prot = set()
    changed = True
    while changed:
        changed = False
        for name, bs in bodies.items():
            if name in prot:
                continue
            for body in bs:
                if _is_directly_text_checking(body, consts):
                    prot.add(name)
                    changed = True
                    break
                for m in _CALL_SITE.finditer(body):
                    callee = m.group(1)
                    if callee not in prot:
                        continue
                    args = _balanced_paren(body, m.end() - 1)
                    if _edge_is_skip_only(args, consts or ()):
                        continue
                    prot.add(name)
                    changed = True
                    break
                if name in prot:
                    break
    return prot


def _text_checking_helpers(text):
    """Set of in-file helper names taking a JitResult-shaped parameter whose
    OWN body reads `.message` - classified by BODY, not name (the
    check_vacuous_tests.py technique), so an oddly-named local helper is
    still recognised as protective.

    This is the DIRECT (single-hop) rule. `scan_file` deliberately uses
    `_protective_helpers` - the transitive closure built on the same direct
    rule - because a helper that forwards to a shared text-checking
    assertion protects a row just as much as one that reads `.message`
    itself (issue #6538). This DIRECT function is kept as-is because two
    sibling ratchets import it and one
    (`scripts/check_toolchain_skip_surface.py`) uses its COMPLEMENT as its
    "SKIP-only" set; widening it in place would silently change that
    checker's `absorbing_sites` count.

    The exclusion is POSITIVE, not a parse failure: a helper is EXCLUDED
    (denied protective status) only when every `.message`-derived
    `.find(...)` call this function can find in its body can be confirmed to
    use the exact fixed `"SKIP"` literal (the toolchain-unavailable bail-out
    idiom - see SKIP_LITERAL above) - i.e. there is at least one such call,
    and none of them is anything else. Anything this function CANNOT
    positively confirm as skip-only - no recognised `.find(` call at all
    (some other check shape, e.g. `==`/`starts_with`/a regex), a call this
    function's alias/string-literal handling still cannot see through,
    ANYTHING - defaults to PROTECTIVE, matching the plain historical
    `.message in body` behaviour. This asymmetry is deliberate (see the
    commit body / #3892 review round 3): an inflated baseline never reaches
    zero and people stop trusting it, whereas a missed row is merely one
    fewer defect caught THIS ratchet, not a false alarm. Fail toward
    under-flagging, never over-flagging.

    ⚠ THIS FUNCTION IS DELIBERATELY FROZEN AT ITS PRE-#6546 BEHAVIOUR
    (issue #6546). It has the same three under-detection holes the
    delegation closure had - a needle behind a named constant, a needle
    split across adjacent literals, and a marker at a call site rather
    than in the body - and they are NOT closed here. They are closed one
    layer up, in `_protective_helpers` and `_protected`, which is the only
    classifier THIS ratchet's own row count uses. Closing them HERE is
    impossible without moving a banked number in another ratchet:
    `scripts/check_toolchain_skip_surface.py` imports this function and
    uses its COMPLEMENT as its "SKIP-only" set (see that script's
    `_absorbing_sites_in_body`), so denying one more helper as
    non-protective here ADDS a name to that complement and can only RAISE
    `absorbing_sites` - which `--update` refuses and which may only fall.
    The same import is why `_is_directly_text_checking` takes `consts` as
    an OPTIONAL argument defaulting to `None` = this frozen comparison:
    this function calls it with the default, so its result is unchanged
    byte for byte, while `_protective_helpers` passes a real table.

    Recorded here rather than left implicit so the next reader sees a
    decision, not an oversight. See the module docstring's
    "THE ASYMMETRY IS PARTLY DELIBERATE, PARTLY A CLOSED HOLE" note."""
    out = set()
    for m in HELPER.finditer(text):
        params = m.group(2)
        if 'JitResult' not in params:
            continue
        body = _balanced_body(text, m.end() - 1)
        if not _is_directly_text_checking(body):
            continue  # no `.message` read, or positively confirmed skip-only
        out.add(m.group(1))
    return out


def _support_helpers(root):
    """Set of helper names defined under `tests/support/*.hpp` whose
    signature takes a `JitResult`-shaped parameter and whose body reads
    `.message` (C5 COMPLETION ITEM). Same classification rule as
    _text_checking_helpers, applied across the support header files -
    the canonical case is a shared utility in tests/support/ that an
    ordinary test file `#include`s and calls as its sole protection
    against the ambiguous-ran-false shape (issue #3892's recommended
    remedy is to use a shared helper, but the SHARED one was invisible
    to this ratchet before C5: a call site in `tests/<area>/X_test.cpp`
    to a helper defined in `tests/support/some_helpers.hpp` would not
    match NAME_MARKERS (the helper's name might be `expectsCompileError`
    or similar), AND `_text_checking_helpers` only sees the file under
    scan, never the included header).

    The same POSITIVE-exclusion rule applies: a helper is denied
    protective status only when every `.find(...)` call on `.message`
    in its body can be positively confirmed as the SKIP-literal bail-
    out shape. Anything this function CANNOT positively confirm as
    skip-only defaults to PROTECTIVE, matching the per-file rule.

    CAVEAT: this function reads the SUPPORT HEADER as text, not the
    compiled image. A helper that goes through a templated alias or a
    macro layer (e.g. `#define EXPECTS_ERROR(x) checkErr(x, "...")`)
    may not be classified as protective. The narrower `tests/<area>/`
    -shaped checks in `check_vacuous_tests.py` have the same caveat;
    the fix there is to write the helper as plain C++, which is the
    discipline this ratchet enforces by the same mechanism.
    """
    out = set()
    support_dir = root / 'tests' / 'support'
    if not support_dir.is_dir():
        return out
    for p in sorted(support_dir.glob('*.hpp')):
        try:
            text = p.read_text(errors='replace')
        except OSError:
            continue
        out.update(_text_checking_helpers(text))
    return out


def _support_helper_bodies(root):
    """{name: [body, ...]} for every helper defined under `tests/support/
    *.hpp` whose signature takes a `JitResult`-shaped parameter - the BODY
    form of `_support_helpers`, read once per `measure()` call.

    Bodies (not just names) are needed because the delegation closure in
    `_protective_helpers` has to read what each shared helper does in order
    to know whether forwarding to it pins message text: a support header
    may itself define a SKIP-ONLY guard, and a caller of THAT must stay
    denied. Returning names only (as `_support_helpers` does) would make
    every forwarder to a support-header helper look equally protective.

    Same JitResult-signature gate as `_support_helpers`, so a helper whose
    parameter is not a JitResult is still not collected."""
    out = {}
    support_dir = root / 'tests' / 'support'
    if not support_dir.is_dir():
        return out
    for p in sorted(support_dir.glob('*.hpp')):
        try:
            text = p.read_text(errors='replace')
        except OSError:
            continue
        for name, bs in _helper_bodies(text).items():
            out.setdefault(name, []).extend(bs)
    return out


def _support_skip_constants(root):
    """{name: value} for the named skip-marker constants declared under
    `tests/support/*.hpp` (issue #6546) - the header-side companion to
    `_support_helpers`/`_support_helper_bodies`, and the same
    once-per-`measure()` collection discipline.

    A test that #includes a shared header and calls a header-declared
    `skipped(r, kSkip)` would otherwise see `kSkip` as an unresolvable
    identifier at the call site and, per the documented
    fail-toward-under-flagging polarity, count a row that the bail-out
    idiom in the header actually absorbs. Same JitResult-free shape as the
    other two support collectors (this one is keyed on the declaration,
    not on a JitResult parameter)."""
    out = {}
    support_dir = root / 'tests' / 'support'
    if not support_dir.is_dir():
        return out
    for p in sorted(support_dir.glob('*.hpp')):
        try:
            text = p.read_text(errors='replace')
        except OSError:
            continue
        out.update(_skip_constants(_strip_comments(text)))
    return out


LOC_LINE = re.compile(r'\.line\b\s*[><=!]=?\s*0')
LOC_COLUMN = re.compile(r'\.column\b\s*[><=!]=?\s*0')


def _name_marker_call(body, var):
    """True if `body` calls SOME_NAME(..., var, ...) [var in any argument
    position, since a needle/substring often comes first] where SOME_NAME
    contains one of NAME_MARKERS."""
    for mk in re.finditer(r'([A-Za-z_]\w*)\s*\(', body):
        name = mk.group(1)
        if not any(marker in name for marker in NAME_MARKERS):
            continue
        # Find this call's argument list (balanced on parens) and check `var`
        # appears in it as a whole identifier.
        depth = 0
        j = mk.end() - 1
        for k in range(j, len(body)):
            if body[k] == '(':
                depth += 1
            elif body[k] == ')':
                depth -= 1
                if depth == 0:
                    args = body[j + 1:k]
                    if re.search(r'\b' + re.escape(var) + r'\b', args):
                        return True
                    break
    return False


def _protective_call_sites(body, name, var):
    """Raw argument text of every `<name>(<var>, ...)` call in `body`.
    The helper-call arm of `_protected` used to be a bare `re.search`,
    which cannot see the ARGUMENTS; #6546 needs them, because shape 3 puts
    the skip marker at the call site rather than in the helper body.

    `var` must be the FIRST argument, exactly as the old regex required
    (`NAME\\s*\\(\\s*VAR\\s*[,)]`): a needle or substring often comes first
    in C++ (`contains(r, "needle")`), so anything after the first argument
    is deliberately not matched here. The old regex's `[,)]` tail became a
    `,`-or-end-of-arguments tail here, because `_balanced_paren` returns
    the text BETWEEN the parens and so never the closing one.
    """
    out = []
    for m in re.finditer(r'\b' + re.escape(name) + r'\s*\(', body):
        args = _balanced_paren(body, m.end() - 1)
        if re.match(r'\s*' + re.escape(var) + r'\s*(?:,|$)', args):
            out.append(args)
    return out


def _protected(body, var, text_helpers, consts=()):
    """True if this BEZEL_TEST body pins the rejection for `var`.

    `consts` (issue #6546) is the file's named-skip-constant table, used by
    the skip-only CALL-SITE arm below. It defaults to empty so that
    `scripts/check_timeout_satisfiable_negatives.py`, which calls this with
    three positional arguments, keeps working unchanged - and keeps this
    function's external signature exactly as it was.

    THE CALL-SITE ARM (shape 3 of #6546). A helper classified protective is
    protective because it ULTIMATELY reads message text - a statement about
    the helper, not about any one of its call sites. The bail-out idiom can
    hide behind that: a helper forwarding a needle PARAMETER
    (`skipped(r, needle) { return diagnosticMessageContains(r, needle); }`)
    is genuinely text-checking, so the closure is RIGHT to classify it
    protective, yet the call `if (skipped(r, "SKIP")) return;` is a SKIP-ONLY
    guard pinning nothing. The edge rule cannot catch this on its own: it
    inspects the delegation edge INSIDE the helper, where the call site's
    `"SKIP"` is not visible.

    So the skip marker is checked here, where the arguments are: a
    protective-helper call whose arguments resolve to skip-only does NOT
    protect. The polarity is fail-toward-under-flagging, as everywhere else
    here - an argument text that resolves to NO value (a needle in a local
    variable, an unrecognised spelling) is not skip-only and still
    protects."""
    if re.search(r'\.message\s*\.\s*find\s*\(', body):
        return True
    if _name_marker_call(body, var):
        return True
    for name in text_helpers:
        sites = _protective_call_sites(body, name, var)
        # Protective if there is at least one call site, and either it
        # passes a real needle or its arguments resolve to nothing this
        # module can call skip-only.
        if sites and any(not _edge_is_skip_only(args, consts)
                         for args in sites):
            return True
    if LOC_LINE.search(body) and LOC_COLUMN.search(body):
        return True
    return False


RAN_FALSE = re.compile(r'BEZEL_EXPECT_FALSE\s*\(\s*(\w+)\s*\.\s*ran\s*\)')
HAS_ERRORS_TRUE = re.compile(
    r'BEZEL_EXPECT_TRUE\s*\(\s*(\w+)\s*(?:\.\s*diagnostics)?\s*\.\s*'
    r'hasErrors\s*\(\s*\)\s*\)')


def _ambiguous_ok_test_names(raw):
    """Set of test names whose BEZEL_TEST body is exempted by an
    `// AMBIGUOUS-RAN-FALSE-OK: <reason>` marker (C1). A marker exempts
    the next BEZEL_TEST body whose opener sits within 8 lines below it
    (mirrors scripts/check_localdecl_only_walkers.py:75-100 and :145 -
    same mechanism as every OTHER-ONLY marker in this repo, ported from
    #3590). A reason after the colon is REQUIRED - a marker with no
    reason is malformed and does not exempt (mirrors LOCALDECL-ONLY,
    which uses `\\S` in its own marker regex for the same reason).

    Reads from the RAW text (marker IS a comment, deliberately -
    check_localdecl_only_walkers.py:160-165 makes the same choice for the
    same reason: a prose MENTION of a marker-shape in a docstring should
    not trip the detector). The body-identification machinery below
    (scan_file) reads the comment-stripped text, but the marker is only
    consulted to determine which TEST NAME to skip, not as part of body
    protection; matching by name within a file is unambiguous in practice
    (every test name is unique within its file, and a marker that
    exempted a sibling row would still only exempt THAT row's name).
    """
    out = set()
    lines = raw.split('\n')
    for i, ln in enumerate(lines):
        if not AMBIGUOUS_OK_MARKER.search(ln):
            continue
        # Marker exempts the next BEZEL_TEST body that opens within 8 lines
        # of it (inclusive of the marker line as the start, exclusive of
        # the marker's own line so a marker placed AT a BEZEL_TEST does not
        # also exempt it). Walk forward, stop on the first match.
        for j in range(i + 1, min(i + 8, len(lines))):
            tm = BEZEL_TEST_LINE.search(lines[j])
            if tm:
                out.add(tm.group(1))
                break
    return out


def _ambiguous_debt_markers(raw):
    """[(line, issue_or_None)] for AMBIGUOUS-RAN-FALSE-DEBT markers in the
    raw text. A marker with a reason but no leading `#NNNN` returns
    `(line, None)` - well-formed-ness is reported separately so the gate
    can flag malformed markers as a distinct violation (mirrors
    scripts/check_localdecl_only_walkers.py:290-304, the same
    well-formed-vs-malformed split).

    The marker is read from RAW (not the comment-stripped text) for the
    same reason AMBIGUOUS_OK_MARKER is - it IS a comment. Reads by line
    number; scan_file's body-attachment uses the same line numbers so
    attaching a marker to the next row within 8 lines is straightforward.
    """
    out = []
    for i, ln in enumerate(raw.split('\n')):
        m = AMBIGUOUS_DEBT_MARKER.search(ln)
        if not m:
            continue
        im = AMBIGUOUS_DEBT_ISSUE.match(m.group(1).strip())
        if im:
            out.append((i + 1, int(im.group(1))))
        else:
            out.append((i + 1, None))
    return out


def scan_file(path, raw, support_helpers=None, support_bodies=None,
              support_consts=None):
    """-> list of (test_name, var) unprotected rows in this file's raw
    text. `raw` is the file's actual on-disk text (the marker-attach
    logic reads from it directly, since both AMBIGUOUS-RAN-FALSE-OK and
    AMBIGUOUS-RAN-FALSE-DEBT are comments and would be stripped from the
    comment-stripped copy - check_localdecl_only_walkers.py:160-165 makes
    the same point). `support_helpers` is the set of helper NAMES
    collected from `tests/support/*.hpp` by `_support_helpers()` (C5);
    a call to one of those names in a test body counts as protection
    exactly like an in-file helper would. `support_consts` is the
    named-skip-constant table of those same headers (issue #6546)."""
    if 'BEZEL_TEST' not in raw:
        return []
    # fix #3915: scan a COMMENT-STRIPPED copy, never the raw text - a
    # comment quoting `BEZEL_EXPECT_FALSE(r.ran)` is not live code, whether
    # it sits inside this test's own body or in the gap before/after it
    # (the body is still delimited by "up to the next BEZEL_TEST(", which
    # is why a comment in that gap used to bleed into the PRECEDING test's
    # window - stripping it removes the only thing that made it matchable,
    # in every window it could possibly fall into).
    text = _strip_comments(raw)
    # C4: blank out `#if 0` / `#if false` block contents before the
    # scan. See _strip_disabled_preprocessor docstring + module docstring
    # "PREPROCESSOR CONDITIONAL HANDLING" for what is and is not handled.
    text = _strip_disabled_preprocessor(text)
    if not (RAN_FALSE.search(text) or HAS_ERRORS_TRUE.search(text)):
        return []
    # #6546: the named-skip-constant table for THIS file, threaded into
    # both the delegation closure and the per-row protection test. Built
    # from the comment-stripped, preprocessor-blanked text for the same
    # reason that text is: a constant declared in a comment is not a
    # constant. A caller may pass `support_consts` to add the names
    # declared in the tests/support headers a test #includes.
    consts = _skip_constants(text)
    if support_consts:
        consts = {**support_consts, **consts}
    text_helpers = _protective_helpers(_helper_bodies(text),
                                       support_bodies or {}, consts)
    # C5: union the in-file helper names with the support-header
    # helper names collected once per measure() call (avoiding one
    # glob+read per scan_file invocation; the union is per-call).
    # `_protective_helpers` already folds the support-header bodies in, so
    # their names are covered; this keeps the pass-through for a caller
    # that supplies only the name set.
    if support_helpers:
        text_helpers = text_helpers | support_helpers
    ok_tests = _ambiguous_ok_test_names(raw)  # C1: suppression marker
    marks = [(m.start(), f'{m.group(1)}.{m.group(2)}', m.end())
             for m in BEZEL_TEST.finditer(text)]
    out = []
    for i, (start, tname, end) in enumerate(marks):
        stop = marks[i + 1][0] if i + 1 < len(marks) else len(text)
        body = text[end:stop]
        bound_vars = {m.group(1) for m in BIND.finditer(body)}
        claim_vars = ({m.group(1) for m in RAN_FALSE.finditer(body)} |
                      {m.group(1) for m in HAS_ERRORS_TRUE.finditer(body)})
        # STRICTLY require the claim variable to be bound, in this same
        # body, from a rejection-observing runAot-family call. Issue #3892
        # is specifically about THAT harness's four-outcome `.ran` ambiguity
        # (a program that is compiled AND RUN) - a `.hasErrors()` assertion
        # over an unrelated result type (OwnershipResult / SemaResult from a
        # static-analysis-only check that never executes anything) does not
        # have this ambiguity at all, and an early draft of this script that
        # counted such vars whenever NO local binding was recognised in a
        # body inflated the count from ~1170 by picking up the entire
        # ownership/sema suites. Under-counting an unusually-wrapped runAot
        # binding (a project helper like `auto r = compileAndRun(src)`) is
        # the safer failure mode for a ratchet than over-counting an entire
        # unrelated test family.
        # C1: a row carrying an AMBIGUOUS-RAN-FALSE-OK marker is
        # deliberately preserved for some other purpose (the #3007
        # demo in tests/support/aot_infra_loud_3007_test.cpp is the
        # canonical case) and is not an accidental hole in protection.
        suite, _, test_name = tname.partition('.')
        test_exempt = test_name in ok_tests
        for var in sorted(claim_vars & bound_vars):
            if (not _protected(body, var, text_helpers, consts)
                    and not test_exempt):
                out.append((tname, var))
    return out


def _ambiguous_debt_test_names(raw):
    """Set of test names whose BEZEL_TEST body carries a WELL-FORMED
    AMBIGUOUS-RAN-FALSE-DEBT marker (C2). A debt marker on a line within
    8 lines before a BEZEL_TEST opener attaches to that body and
    justifies one excess row above the per-file ceiling recorded in
    scripts/ambiguous_ran_false.grandfather - same shape as the
    suppression marker (C1 / _ambiguous_ok_test_names above) and the
    LOCALDECL-DEBT mechanism (scripts/check_localdecl_only_walkers.py
    :80-96 and :290-304).

    A debt marker without a `#NNNN` after the colon is MALFORMED and
    justifies nothing - mirrors LOCALDECL-DEBT's well-formed-vs-
    malformed split (check_localdecl_only_walkers.py:290-304). Only
    well-formed markers appear in the returned set, so debt_per_file
    (built from it) counts only what actually justifies a rise.

    Unlike _ambiguous_ok_test_names, a debt marker does NOT remove the
    site from the count - the row is still an offender, still baselined,
    still meant to be fixed; the marker only justifies a rise past the
    per-file ceiling. Per-row attachment (one DEBT marker justifies one
    excess row) is the same per-site model LOCALDECL-DEBT uses - see
    scripts/check_localdecl_only_walkers.py:402-424.
    """
    out = set()
    lines = raw.split('\n')
    for i, ln in enumerate(lines):
        m = AMBIGUOUS_DEBT_MARKER.search(ln)
        if not m:
            continue
        if not AMBIGUOUS_DEBT_ISSUE.match(m.group(1).strip()):
            continue  # malformed - reason with no #NNNN justifies nothing
        for j in range(i + 1, min(i + 8, len(lines))):
            tm = BEZEL_TEST_LINE.search(lines[j])
            if tm:
                out.add(tm.group(1))
                break
    return out


def _per_file_counts(per_file):
    """{path: count} - simple aggregation over measure's per_file
    output. Used by both check_debt_floor (C2) and write_grandfather."""
    return {p: len(rows) for p, rows in per_file.items()}


def measure(root):
    """-> (total, per_file, debt_per_file) where
    per_file: {relpath: [(test, var), ...]} - all unprotected rows
    debt_per_file: {relpath: int} - count of those rows whose
        BEZEL_TEST body carries an AMBIGUOUS-RAN-FALSE-DEBT marker
        (C2). A debt marker attached to a body whose rows are ALL
        protected is not counted here - it justifies nothing. The
        returned counts are still conservative: a body with one
        unprotected row and a debt marker contributes 1 to
        debt_per_file, matching the per-site 1-marker-per-excess-row
        model."""
    per_file = {}
    debt_per_file = {}
    tests_dir = root / 'tests'
    if not tests_dir.is_dir():
        return 0, {}, {}
    # C5: collect helper names from tests/support/*.hpp ONCE per
    # measure() call (not once per file). See _support_helpers
    # docstring for the rationale and the same-classification rule.
    support_helpers = _support_helpers(root)
    support_bodies = _support_helper_bodies(root)
    support_consts = _support_skip_constants(root)
    for p in sorted(tests_dir.rglob('*_test.cpp')):
        try:
            raw = p.read_text(errors='replace')
        except OSError:
            continue
        rows = scan_file(p, raw, support_helpers=support_helpers,
                         support_bodies=support_bodies,
                         support_consts=support_consts)
        rel = str(p.relative_to(root))
        if rows:
            per_file[rel] = rows
        # C2: count debt-marked unprotected rows. Rows whose body
        # carries a debt marker count toward debt_per_file; rows in
        # the same body that already have an OK marker do not appear
        # in `rows` at all (they were suppressed at scan_file time),
        # so we do not double-count them.
        debt_tests = _ambiguous_debt_test_names(raw)
        debt_count = sum(
            1 for tname, _ in rows
            if tname.split('.', 1)[1] in debt_tests)
        if debt_count > 0:
            debt_per_file[rel] = debt_count
    total = sum(len(v) for v in per_file.values())
    return total, per_file, debt_per_file


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
                'scripts/ambiguous_ran_false.baseline has unresolved merge '
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
    sys.stderr.write('scripts/ambiguous_ran_false.baseline is missing '
                     "'count'.\n")
    raise SystemExit(2)


def write_baseline(count, path=BASELINE):
    path.write_text('\n'.join([
        '# ambiguous_ran_false.baseline - ratchet for issue #3892: a row',
        '# pinning a compile-time REJECTION with only `!ran` / `hasErrors()`',
        '# and no message-text or real-location check cannot distinguish a',
        '# genuine rejection from a runtime crash/signal/timeout/ASan report.',
        '#',
        '# Regenerate with scripts/check_ambiguous_ran_false.py --update after',
        '# fixing (protecting) a row.  --update refuses to raise a count.',
        '#',
        '# EXACT, not a ceiling: a count below baseline fails as STALE',
        '# HEADROOM (see the script docstring - same rule as every other',
        '# ratchet in this family).',
        '#',
        '# See the script docstring for the exact counting criteria; this',
        "# number is NOT expected to equal issue #3892's manually-traced 60",
        '# (different, explicitly-stated criteria - see docstring).',
        'count = %d' % count,
        '']))


def check(root, baseline_path=BASELINE, grandfather_path=GRANDFATHER, stale=True):
    base = read_baseline(baseline_path)
    cur, per_file, debt_per_file = measure(root)
    grand = read_grandfather(grandfather_path)
    msgs = []
    if cur > base:
        msgs.append(
            'ROSE: baseline %d, tree has %d (+%d). A new row pins a '
            'compile-time rejection using only `.ran`/`hasErrors()` with no '
            'message-text or real-location check - see script docstring for '
            'the fix (use tests/support/aot_result_asserts.hpp or an '
            'equivalent).' % (base, cur, cur - base))
    elif stale and cur < base:
        msgs.append(
            'baseline claims %d but the tree has %d - STALE HEADROOM (%+d). '
            'The baseline is exact, not a ceiling: re-run --update on the '
            'tree that fixed these rows so the improvement is banked.'
            % (base, cur, cur - base))
    # C2: check that no file has more unprotected rows than its
    # grandfathered ceiling + number of AMBIGUOUS-RAN-FALSE-DEBT markers
    # in that file. --force does NOT substitute for the marker.
    msgs.extend(check_debt_floor(_per_file_counts(per_file), grand,
                                 debt_per_file))
    return (not msgs), msgs, per_file


# ---------------------------------------------------------------------------
# grandfather / debt floor (C2)
# ---------------------------------------------------------------------------

def _parse_per_file_counts(text, source_name):
    """Parse a flat `<int> <path>` file (used by both the mutable baseline
    and the frozen grandfather ceiling in this ratchet family).
    `source_name` is what a merge-conflict error names."""
    out = {}
    for raw in text.splitlines():
        line = raw.strip()
        if line.startswith(('<<<<<<<', '=======', '>>>>>>>')):
            sys.stderr.write(
                '%s has unresolved merge conflict markers. Resolve by '
                'hand; there is no --update for a frozen ceiling file - '
                'it changes only by a deliberate, reviewed edit.\n'
                % source_name)
            raise SystemExit(2)
        if not line or line.startswith('#'):
            continue
        n, path = line.split(None, 1)
        out[path] = int(n)
    return out


def read_grandfather(path=GRANDFATHER):
    """The per-file ceiling for AMBIGUOUS-RAN-FALSE-DEBT - a rise above
    the ceiling needs a per-site DEBT marker; --force alone is not
    enough. Mirrors scripts/check_localdecl_only_walkers.py:465-473
    (same family-wide mechanism, ported from #3590). A missing file
    reads as an ALL-ZERO ceiling (fail closed): every unprotected row
    needs a DEBT marker until the file is created with the initial
    values."""
    if not path.exists():
        return {}
    return _parse_per_file_counts(path.read_text(),
                                  str(path))


def tighten_grandfather(existing, cur):
    """Monotone downward only: new[path] = min(existing.get(path, 0),
    cur.get(path, 0)). A file whose tightened ceiling reaches 0 is
    dropped entirely. Same shape as
    scripts/check_localdecl_only_walkers.py:476-485."""
    out = {}
    for path in set(existing) | set(cur):
        new = min(existing.get(path, 0), cur.get(path, 0))
        if new > 0:
            out[path] = new
    return out


def write_grandfather(existing, cur, path=GRANDFATHER):
    """Persist the tightened ceiling. Called on EVERY successful
    `--update`, alongside write_baseline - a drain must become the
    new ceiling immediately, or the mechanism protects nothing during
    a drain campaign. Returns [(path, old, new), ...] for the
    tightening report."""
    new = tighten_grandfather(existing, cur)
    lines = [
        '# ambiguous_ran_false.grandfather - per-file ceiling for #3892/#3895.',
        '#',
        '# Each row is one file maximum number of bare-!ran/hasErrors()',
        '# rows allowed without per-site `// AMBIGUOUS-RAN-FALSE-DEBT:',
        '# #NNNN <reason>` markers. The ceiling only ever falls - every',
        '# successful `--update` tightens each file to min(existing,',
        '# measured), so a drain becomes the new ceiling on the very',
        '# next --update. A file whose tightened ceiling reaches 0 is',
        '# dropped entirely. Raising a ceiling stays a deliberate,',
        '# reviewed hand-edit to this file.',
        '#',
        '# See check_debt_floor() in scripts/check_ambiguous_ran_false.py',
        '# (mirrors scripts/localdecl_only_walkers.grandfather, ported',
        '# from #3590 as the family-wide mechanism).',
        '',
    ]
    lines += ['%-4d %s' % (n, p) for p, n in sorted(new.items())]
    path.write_text('\n'.join(lines) + '\n')
    changed = [(p, existing.get(p, 0), new.get(p, 0))
               for p in sorted(set(existing) | set(new))
               if new.get(p, 0) < existing.get(p, 0)]
    return new, changed


def check_debt_floor(cur, floor, debt_per_file):
    """-> violation strings. A rise above the GRANDFATHERED ceiling
    needs a per-site AMBIGUOUS-RAN-FALSE-DEBT marker; --force alone
    is not enough. Identical mechanism to
    scripts/check_localdecl_only_walkers.py:402-424, ported from #3590
    as the family-wide shape."""
    bad = []
    for path in sorted(cur):
        have = cur[path]
        ceiling = floor.get(path, 0)
        if have <= ceiling:
            continue
        excess = have - ceiling
        got = debt_per_file.get(path, 0)
        if got < excess:
            bad.append(
                '%s has %d row(s), %d above its grandfathered ceiling '
                'of %d (scripts/ambiguous_ran_false.grandfather) - %d of '
                'the %d excess row(s) carry a `// AMBIGUOUS-RAN-FALSE-DEBT: '
                '#NNNN <reason>` marker, %d do not. --force does not '
                'substitute for the marker.'
                % (path, have, excess, ceiling, got, excess, excess - got))
    return bad


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

        _mk(root, 'tests/codegen/bare_test.cpp', '''
BEZEL_TEST(fixture_suite, bare_ran_false) {
    auto r = runAot("module M;");
    BEZEL_EXPECT_FALSE(r.ran);
}
''')
        _mk(root, 'tests/codegen/haserrors_only_test.cpp', '''
BEZEL_TEST(fixture_suite, haserrors_only) {
    auto r = runAot("module M;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
''')
        _mk(root, 'tests/codegen/text_checked_test.cpp', '''
BEZEL_TEST(fixture_suite, text_checked) {
    auto r = runAot("module M;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "some specific text"));
}
''')
        _mk(root, 'tests/codegen/shared_helper_test.cpp', '''
#include "support/aot_result_asserts.hpp"
BEZEL_TEST(fixture_suite, shared_helper) {
    auto r = runAot("module M;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeNotRuntime(r));
}
''')
        _mk(root, 'tests/codegen/location_checked_test.cpp', '''
namespace {
struct Loc { int line; int column; };
Loc parseLoc(const bezel::test::JitResult& r) { return {1, 2}; }
}
BEZEL_TEST(fixture_suite, location_checked) {
    auto r = runAot("module M;");
    BEZEL_EXPECT_FALSE(r.ran);
    auto loc = parseLoc(r);
    BEZEL_EXPECT_TRUE(loc.line > 0);
    BEZEL_EXPECT_TRUE(loc.column > 0);
}
''')
        _mk(root, 'tests/codegen/local_helper_body_checked_test.cpp', '''
namespace {
bool weirdlyNamed(const bezel::test::JitResult& r, const char* s) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(s) != std::string::npos) return true;
    return false;
}
}
BEZEL_TEST(fixture_suite, local_helper_body_checked) {
    auto r = runAot("module M;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(weirdlyNamed(r, "x"));
}
''')
        _mk(root, 'tests/codegen/direct_message_find_test.cpp', '''
BEZEL_TEST(fixture_suite, direct_message_find) {
    auto r = runAot("module M;");
    BEZEL_EXPECT_FALSE(r.ran);
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("x") != std::string::npos) found = true;
    BEZEL_EXPECT_TRUE(found);
}
''')
        # THE REVIEWER'S FINDING, pinned: a `wasSkipped`-shaped toolchain
        # bail-out guard reads `.message` but checks ONLY the fixed "SKIP"
        # literal - orthogonal to reject-vs-crash discrimination. Before the
        # SKIP_LITERAL fix this was misclassified as a text check and the row
        # went unflagged (the exact shape of
        # tests/codegen/narrowed_value_op_arg_release_3116_test.cpp /
        # narrowed_value_unbox_3090_test.cpp /
        # dict_sorted_key_local_release_3887_3888_test.cpp).
        _mk(root, 'tests/codegen/skip_guard_only_test.cpp', '''
namespace {
bool wasSkipped(const bezel::test::JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}
}
BEZEL_TEST(fixture_suite, skip_guard_only) {
    auto r = runAot("module M;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_FALSE(r.ran);
}
''')
        # Companion: a helper that ALSO checks a real substring alongside the
        # SKIP bail-out is still genuinely protective - the fix must not
        # over-correct into denying every helper whose body ever mentions
        # "SKIP".
        _mk(root, 'tests/codegen/skip_guard_plus_real_check_test.cpp', '''
namespace {
bool wasSkippedOrRejected(const bezel::test::JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("SKIP") != std::string::npos) return true;
        if (d.message.find(sub) != std::string::npos) return true;
    }
    return false;
}
}
BEZEL_TEST(fixture_suite, skip_guard_plus_real_check) {
    auto r = runAot("module M;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(wasSkippedOrRejected(r, "some specific rejection text"));
}
''')
        # ROUND-3 REVIEW FINDING: the classifier's FIND_CALL regex only
        # recognised `.message.find("literal")` or `.message.find(ident)`
        # directly - so it silently failed to parse (and therefore denied
        # protection to) two real shapes, inflating the count by 12 false
        # positives on the real tree. Both get their own fixture here, which
        # is the gap that let the bug ship (self-test exercised neither).
        #
        # Shape 1: `.message` ALIASED to a local before `.find(` is called
        # on the alias - tests/sema/lambda_param_no_target_type_reject_3756_
        # test.cpp's `locatedAt`.
        _mk(root, 'tests/codegen/aliased_message_check_test.cpp', '''
namespace {
bool locatedAt(const bezel::test::JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        const std::string& m = d.message;
        if (m.find(needle) != std::string::npos) return true;
    }
    return false;
}
}
BEZEL_TEST(fixture_suite, aliased_message_check) {
    auto r = runAot("module M;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(locatedAt(r, "some specific rejection text"));
}
''')
        # Shape 2: adjacent-string-literal concatenation as the `.find(...)`
        # argument - tests/codegen/generic_owned_field_string_reject_1107_
        # test.cpp's `has285Reject` (`d.message.find("\\xC2\\xA7""28.5")`) and
        # tests/codegen/oftype_removal_completeness_3440_test.cpp's
        # `removedOfType` (`+`-concatenation over an ALIASED `.message`, and
        # embedded '(' ')' characters inside the literal itself - the
        # _balanced_paren string-literal-awareness this fixture also pins).
        _mk(root, 'tests/codegen/concatenated_literal_check_test.cpp', '''
namespace {
bool has285Reject(const bezel::test::JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("\\xC2\\xA7""28.5") != std::string::npos)
            return true;
    return false;
}
bool removedOfType(const bezel::test::JitResult& r, const std::string& u) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        const std::string& m = d.message;
        if (m.find("`OfType<" + u + ">()` on a sequence") != std::string::npos)
            return true;
    }
    return false;
}
}
BEZEL_TEST(fixture_suite, concatenated_literal_check) {
    auto r = runAot("module M;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(has285Reject(r));
    BEZEL_EXPECT_TRUE(removedOfType(r, "int"));
}
''')
        # A SKIP-only guard reached THROUGH an alias must still be denied -
        # the positive-confirmation logic must not become "anything aliased
        # is automatically protective".
        _mk(root, 'tests/codegen/aliased_skip_only_test.cpp', '''
namespace {
bool wasSkippedAliased(const bezel::test::JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        const std::string& m = d.message;
        if (m.find("SKIP") != std::string::npos) return true;
    }
    return false;
}
}
BEZEL_TEST(fixture_suite, aliased_skip_only) {
    auto r = runAot("module M;");
    if (wasSkippedAliased(r)) return;
    BEZEL_EXPECT_FALSE(r.ran);
}
''')
        # Positive control that this is the real demonstration file's shape
        # (tests/support/aot_infra_loud_3007_test.cpp), included because
        # support/ is IN SCOPE (see docstring).
        _mk(root, 'tests/support/demo_test.cpp', '''
BEZEL_TEST(aot_infra_loud_3007, legacy_negative_assertion_shape) {
    auto r = bezel::test::runAot("module M;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
''')
        # C1 fixture: a row carrying an `// AMBIGUOUS-RAN-FALSE-OK:
        # <reason>` marker on the line before its BEZEL_TEST opener is
        # suppressed from the count - mirrors the LOCALDECL-ONLY marker
        # (check_localdecl_only_walkers.py:75-100). Used to exempt the
        # real demonstration row in tests/support/aot_infra_loud_3007_test.cpp
        # (the #3007 FAIL-WITHOUT vehicle), so the baseline can reach 0
        # in principle. Without the marker this fixture WOULD count (it
        # has bare !ran + hasErrors with no protection); with the marker
        # it must not.
        _mk(root, 'tests/codegen/marked_suppression_test.cpp', '''
// AMBIGUOUS-RAN-FALSE-OK: deliberately preserved as the FAIL-WITHOUT vehicle for issue #3007's fix in tests/support/aot_infra_loud_3007_test.cpp
BEZEL_TEST(fixture_suite, marked_suppression_demo) {
    auto r = runAot("module M;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
''')
        # C1 negative control: a marker with NO reason after the colon is
        # malformed and does NOT exempt - mirrors LOCALDECL-ONLY's `\S`
        # requirement (check_localdecl_only_walkers.py:112-113, "a marker
        # with no reason after the colon does NOT exempt"). Without the
        # reason this row WOULD count; it must still count.
        _mk(root, 'tests/codegen/malformed_marker_test.cpp', '''
// AMBIGUOUS-RAN-FALSE-OK:
BEZEL_TEST(fixture_suite, malformed_marker_no_reason) {
    auto r = runAot("module M;");
    BEZEL_EXPECT_FALSE(r.ran);
}
''')
        # C1 negative control: a marker placed > 8 lines BEFORE the next
        # BEZEL_TEST does NOT attach - mirrors the LOCALDECL-ONLY 8-line
        # window (check_localdecl_only_walkers.py:81-82, "a marker cannot
        # drift away from the cast it describes"). The marker below sits
        # on its own with the body much further down; the row counts.
        _mk(root, 'tests/codegen/drifted_marker_test.cpp', '''
// AMBIGUOUS-RAN-FALSE-OK: too far above the next test - marker window is 8 lines


// padding so the BEZEL_TEST below is well outside the 8-line window
// pad 1
// pad 2
// pad 3
// pad 4
// pad 5
// pad 6
// pad 7
// pad 8
// pad 9
// pad 10
// pad 11
// pad 12
BEZEL_TEST(fixture_suite, drifted_marker_demo) {
    auto r = runAot("module M;");
    BEZEL_EXPECT_FALSE(r.ran);
}
''')
        # C1 control: a marker placed on a line whose next BEZEL_TEST is
        # out of scope (e.g. across files) does not exempt THIS file's
        # row. This is automatic in this per-file scan but worth a
        # negative control so future refactors do not break it: a file
        # with a marker BUT no matching BEZEL_TEST within 8 lines must
        # still see its bare row counted.
        _mk(root, 'tests/codegen/marker_no_match_test.cpp', '''
// AMBIGUOUS-RAN-FALSE-OK: no BEZEL_TEST body opens within 8 lines below
// pad 1
// pad 2
// pad 3
// pad 4
// pad 5
// pad 6
// pad 7
// pad 8
// pad 9
// pad 10
// pad 11
// pad 12
// pad 13
// pad 14
// pad 15
// pad 16
// pad 17
// pad 18
// pad 19
BEZEL_TEST(fixture_suite, marker_no_match) {
    auto r = runAot("module M;");
    BEZEL_EXPECT_FALSE(r.ran);
}
''')
        # A test with EXPECT_TRUE(r.ran) only - not a rejection claim at all.
        _mk(root, 'tests/codegen/positive_only_test.cpp', '''
BEZEL_TEST(fixture_suite, positive_only) {
    auto r = runAot("module M;");
    BEZEL_EXPECT_TRUE(r.ran);
}
''')

        # fix #3915 - THE MEASURED BUG: a comment quoting the literal text
        # `BEZEL_EXPECT_FALSE(r.ran)` sits in the gap between two tests (to
        # explain why the SECOND test deliberately avoids it). The old
        # "everything up to the next BEZEL_TEST(" delimiter put that
        # comment in the PRECEDING test's scan window; the preceding test
        # here binds a var named `r` from `runAot` but never itself
        # protects it (it only asserts the POSITIVE case), so the bug
        # attributed a brand new unprotected `r` row to the WRONG test.
        # Neither test in this file may be flagged.
        _mk(root, 'tests/codegen/comment_bleeds_to_preceding_test_test.cpp', '''
BEZEL_TEST(fixture_suite, positive_case_before_the_comment) {
    auto r = runAot("module M;");
    BEZEL_EXPECT_TRUE(r.ran);
}
// The test below deliberately avoids the ambiguous anti-pattern:
//     BEZEL_EXPECT_FALSE(r.ran)
// because plain `.ran` cannot distinguish a genuine rejection from a
// runtime crash/signal/timeout/ASan report (issue #3892).
BEZEL_TEST(fixture_suite, protected_case_after_the_comment) {
    auto r = runAot("module M;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasErrorContaining(r, "some specific text"));
}
''')

        # fix #3915 - the SAME hazard one level down: a comment quoting the
        # pattern INSIDE a test's own braces (not in the gap between two
        # tests) must not be counted as a live assertion in that test
        # either. This test's only REAL assertion is the positive
        # `EXPECT_TRUE(r.ran)`; the only thing matching the "unprotected
        # rejection claim" shape is inside the comment.
        _mk(root, 'tests/codegen/comment_inside_own_body_test.cpp', '''
BEZEL_TEST(fixture_suite, comment_inside_own_body) {
    auto r = runAot("module M;");
    // Historically this test wrote BEZEL_EXPECT_FALSE(r.ran) here, but
    // that is ambiguous - see issue #3892 - so it asserts the positive
    // case instead.
    BEZEL_EXPECT_TRUE(r.ran);
}
''')

        # Companion control: a comment quoting the pattern must NOT mask a
        # row that is GENUINELY unprotected in real (non-commented) code -
        # the fix must keep detecting real cases, not merely stop
        # over-flagging (LANE_RULES: "a fix that stops detecting real cases
        # is far worse than the bug it fixes").
        _mk(root, 'tests/codegen/bare_row_near_comment_test.cpp', '''
BEZEL_TEST(fixture_suite, bare_row_near_comment) {
    auto r = runAot("module M;");
    // NOTE: the row below genuinely uses the ambiguous anti-pattern - it
    // must still be flagged as a real unprotected row even though this
    // very comment also quotes the pattern literal text.
    BEZEL_EXPECT_FALSE(r.ran);
}
''')

        # fix #3915 review round 2: a C++14 digit separator (`1'000`,
        # `0xFF'FF`) must not be misread as opening a char literal - if it
        # is, everything after it on the SAME line (including a same-line
        # `//` comment) is wrongly treated as still being "inside a
        # string" and is left unstripped, resurrecting the exact
        # misattribution this file exists to eliminate. Real code here
        # only asserts the POSITIVE case; the anti-pattern's literal text
        # appears twice, both times inside a same-line trailing comment
        # immediately after a digit separator (decimal, then hex).
        _mk(root, 'tests/codegen/digit_separator_same_line_comment_test.cpp', """
BEZEL_TEST(fixture_suite, digit_separator_same_line_comment) {
    auto r = runAot("module M;");
    BEZEL_EXPECT_TRUE(r.ran);
    int decimalWithSeparator = 1'000;  // quoting BEZEL_EXPECT_FALSE(r.ran) right after a decimal digit separator - must still be stripped
    int hexWithSeparator = 0xFF'FF;  // quoting BEZEL_EXPECT_FALSE(r.ran) right after a hex digit separator too
}
""")

        # issue #3920: the SAME both-sides-wordchar shape as a digit
        # separator also arises from an identifier immediately abutting a
        # char-literal opener, which is valid C++ (`int f() { return'a'; }`
        # compiles - confirmed with g++, per the issue). Before the fix,
        # `_is_digit_separator` did not exist and the flanking check alone
        # misread this `'` as a digit separator, passing it through instead
        # of opening char-literal mode - so the closing `'` after `a` was
        # then misread as an OPENING quote (its own right-hand neighbour is
        # `;`, not a word character, so it fails the flanking test and falls
        # through to the ordinary quote branch), desyncing parity for the
        # rest of the line and leaving the same-line trailing `//` comment
        # UNSTRIPPED - resurrecting the exact #3915 misattribution. MEASURED:
        # importing the pre-fix module against this exact fixture pair
        # reports count=2 (both falsely flagged); the fixed module reports
        # 0. Real code here only asserts the POSITIVE case.
        _mk(root, 'tests/codegen/char_literal_abuts_identifier_test.cpp', """
BEZEL_TEST(fixture_suite, char_literal_abuts_identifier_return) {
    auto r = runAot("module M;");
    BEZEL_EXPECT_TRUE(r.ran);
    int f() { return'a'; }  // quoting BEZEL_EXPECT_FALSE(r.ran) right after a char literal that abuts an identifier - must still be stripped (issue #3920)
}
""")
        # Sibling shape: a single-letter identifier (not a keyword) abutting
        # a char literal - `x'y'` has the same both-sides-wordchar shape as
        # `return'a'` but with a run of length 1 before the `'`, exercising
        # the _is_digit_separator backward walk's j==i-1 boundary.
        _mk(root, 'tests/codegen/char_literal_abuts_short_identifier_test.cpp', """
BEZEL_TEST(fixture_suite, char_literal_abuts_identifier_short) {
    auto r = runAot("module M;");
    BEZEL_EXPECT_TRUE(r.ran);
    char c = x'y';  // quoting BEZEL_EXPECT_FALSE(r.ran) right after a single-letter identifier abutting a char literal - must still be stripped (issue #3920)
}
""")

        total, per_file, debt_per_file = measure(root)
        chk('bare `!ran` flagged', 'tests/codegen/bare_test.cpp' in per_file)
        chk('`!ran` + hasErrors()-only flagged (equally vulnerable)',
            'tests/codegen/haserrors_only_test.cpp' in per_file)
        chk('a name-marker-matched helper call is NOT flagged',
            'tests/codegen/text_checked_test.cpp' not in per_file)
        chk('the shared failedAtCompileTimeNotRuntime call is NOT flagged '
            '(no bare !ran claim at all)',
            'tests/codegen/shared_helper_test.cpp' not in per_file)
        chk('a loc.line>0 && loc.column>0 body pattern is NOT flagged',
            'tests/codegen/location_checked_test.cpp' not in per_file)
        chk('an oddly-named in-file helper classified by BODY (.message) is '
            'NOT flagged',
            'tests/codegen/local_helper_body_checked_test.cpp' not in per_file)
        chk('a direct inline .message.find( scan is NOT flagged',
            'tests/codegen/direct_message_find_test.cpp' not in per_file)
        chk('a wasSkipped-shaped guard (checks ONLY the fixed "SKIP" '
            'literal) does NOT count as protection (the reviewer finding)',
            'tests/codegen/skip_guard_only_test.cpp' in per_file)
        chk('a helper checking SKIP *plus* a real substring is still '
            'genuinely protective (no over-correction)',
            'tests/codegen/skip_guard_plus_real_check_test.cpp' not in per_file)
        chk('a real check reached through an ALIASED `.message` is '
            'protective (round-3 review finding: aliasing silently denied '
            'protection)',
            'tests/codegen/aliased_message_check_test.cpp' not in per_file)
        chk('a real check using adjacent/concatenated string-literal '
            'arguments (including one with embedded parens) is protective '
            '(round-3 review finding)',
            'tests/codegen/concatenated_literal_check_test.cpp' not in per_file)
        chk('a SKIP-only guard reached through an alias is STILL denied '
            '(positive confirmation, not "any alias is automatically safe")',
            'tests/codegen/aliased_skip_only_test.cpp' in per_file)
        chk('tests/support/ IS in scope (the #3007 demo shape is flagged)',
            'tests/support/demo_test.cpp' in per_file)
        chk('a plain EXPECT_TRUE(r.ran) test is not a rejection claim',
            'tests/codegen/positive_only_test.cpp' not in per_file)
        # fix #3915 fixtures.
        chk('issue #3915 MEASURED BUG: a comment quoting the pattern in the '
            'gap between two tests is NOT attributed to the PRECEDING test',
            'tests/codegen/comment_bleeds_to_preceding_test_test.cpp'
            not in per_file)
        chk('fix #3915: a comment quoting the pattern INSIDE a test\'s own '
            'body is NOT attributed to that test either',
            'tests/codegen/comment_inside_own_body_test.cpp' not in per_file)
        chk('fix #3915: a GENUINE bare row is STILL caught even with a '
            'comment nearby quoting the same literal text - the fix must '
            'not stop detecting real cases',
            'tests/codegen/bare_row_near_comment_test.cpp' in per_file)
        chk('fix #3915 review round 2: a C++14 digit separator (1\'000, '
            '0xFF\'FF) is not misread as opening a char literal, so a '
            'same-line trailing comment quoting the pattern IS stripped '
            'and the test is not flagged',
            'tests/codegen/digit_separator_same_line_comment_test.cpp'
            not in per_file)
        chk('issue #3920: an identifier abutting a char literal '
            '(`return\'a\'`) is not misread as a digit separator, so a '
            'same-line trailing comment quoting the pattern IS stripped '
            'and the test is not flagged',
            'tests/codegen/char_literal_abuts_identifier_test.cpp'
            not in per_file)
        chk('issue #3920 sibling: a single-letter identifier abutting a '
            'char literal (`x\'y\'`) is likewise not misread as a digit '
            'separator',
            'tests/codegen/char_literal_abuts_short_identifier_test.cpp'
            not in per_file)
        # C3 fixture (issue #3935): a malformed digit-leading run
        # `9x'y'` followed by a trailing `// ... BEZEL_EXPECT_FALSE(r.ran)`
        # comment. Pre-C3, the FIRST `'` was misclassified as a digit
        # separator (run "9x" starts with `9`, passes the #3920 check),
        # then the SECOND `'` opened char-literal mode (its right
        # neighbour is `;`, fails the flanking check), unterminated
        # across the line, and the trailing `//` comment was HIDDEN
        # (left unstripped) inside the unterminated char literal. The
        # test then saw the comment text as a live assertion. Post-C3,
        # the partial numeric-literal grammar rejects `9x` (the `x` is
        # not a valid numeric-literal character), so the FIRST `'`
        # correctly enters char-literal mode, the SECOND `'` closes it
        # (char literal is `'y'`), and the trailing `//` comment IS
        # stripped. The fixture is the literal `int z = 9x'y';` line
        # from #3935 verbatim.
        _mk(root, 'tests/codegen/malformed_digit_run_test.cpp', """
// issue #3935 - the literal `int z = 9x'y';` line from the issue, with
// a trailing `// ... BEZEL_EXPECT_FALSE(r.ran)` comment that MUST stay
// stripped (the comment text leaks through as a live row if the
// detector's quote parity desyncs - the exact bug C3 fixes).
int z = 9x'y';  // quoting BEZEL_EXPECT_FALSE(r.ran) trailing comment must be stripped
""")
        chk('C3: a malformed digit-leading run `9x\'y\'` followed by a '
            'trailing comment - the partial numeric-literal grammar '
            'rejects `9x` so the first `\'` enters char-literal mode, the '
            'second `\'` closes it, and the trailing `//` comment IS '
            'stripped (no live row from the comment text)',
            'tests/codegen/malformed_digit_run_test.cpp' not in per_file)
        # C3 control: a VALID numeric-literal prefix with digit
        # separator still works after the C3 tightening - `0xFF'FF` is
        # a real C++14 hex literal, the `'` between `FF` and `FF` is a
        # separator, not a char literal opener. The detector must keep
        # accepting this shape (otherwise C3 over-corrects into denying
        # every legitimate separator).
        _mk(root, 'tests/codegen/valid_digit_separator_test.cpp', """
// Real C++14 hex literal with a digit separator - same-shape as the
// malformed fixture above but the run is `0xFF` (a valid hex prefix
// followed by valid hex digits). The `'` is a real separator, the
// trailing `// BEZEL_EXPECT_FALSE(r.ran)` is a real comment.
int z = 0xFF'FF;  // quoting BEZEL_EXPECT_FALSE(r.ran) trailing comment must be stripped
""")
        chk('C3: a valid numeric-literal prefix with digit separator '
            '(0xFF\'FF) still works after C3 tightening - the comment is '
            'stripped, no live row from the comment text',
            'tests/codegen/valid_digit_separator_test.cpp' not in per_file)
        # C4 fixture: a bare `!ran` row inside `#if 0` (or `#if false`)
        # must NOT count after _strip_disabled_preprocessor blanks it out.
        # The C++ preprocessor would skip the block too, so the ratchet's
        # verdict must mirror what the compiler sees - see module
        # docstring "PREPROCESSOR CONDITIONAL HANDLING".
        _mk(root, 'tests/codegen/inside_if0_test.cpp', '''
BEZEL_TEST(fixture_suite, live_above) {
    auto r = runAot("module M;");
    BEZEL_EXPECT_FALSE(r.ran);
}
#if 0
BEZEL_TEST(fixture_suite, inside_if0) {
    auto r = runAot("module M;");
    BEZEL_EXPECT_FALSE(r.ran);
}
#endif
''')
        chk('C4: a row inside `#if 0` does NOT count (the C++ preprocessor '
            'would skip it; the ratchet mirrors the compiler)',
            'tests/codegen/inside_if0_test.cpp' not in per_file)
        _mk(root, 'tests/codegen/inside_iffalse_test.cpp', '''
BEZEL_TEST(fixture_suite, live_above_iffalse) {
    auto r = runAot("module M;");
    BEZEL_EXPECT_FALSE(r.ran);
}
#if false
BEZEL_TEST(fixture_suite, inside_iffalse) {
    auto r = runAot("module M;");
    BEZEL_EXPECT_FALSE(r.ran);
}
#endif
''')
        chk('C4: a row inside `#if false` does NOT count either '
            '(same mechanism as #if 0)',
            'tests/codegen/inside_iffalse_test.cpp' not in per_file)
        # C4 negative control: a row inside `#if 1` is NOT skipped
        # (the limitation is documented - only #if 0 and #if false are
        # handled). The fixture is included so a future change that
        # widens the C4 handling to #if 1 will trip this assertion
        # and force a deliberate review.
        _mk(root, 'tests/codegen/inside_if1_test.cpp', '''
#if 1
BEZEL_TEST(fixture_suite, inside_if1) {
    auto r = runAot("module M;");
    BEZEL_EXPECT_FALSE(r.ran);
}
#endif
''')
        # Re-measure AFTER the C4 fixtures are added so the post-C4
        # assertions below (the `total == N` summary, the per-file
        # counts of the C4 fixtures, and C2's `total_with_c2` delta)
        # reflect the C4 fixtures. The earlier `measure(root)` at
        # line ~1443 ran BEFORE C4 was added; that fresh capture is
        # what the C1/C2/C3 assertions above correctly reference.
        # The vacuous `not in per_file` checks above are the early-exit
        # form of the same assertion against the post-fixture `per_file`
        # below; the check that C4 actually blanked the disabled block
        # body is the `in per_file` + `len(...) == 1` pair below (a
        # file with only a `#if 0`-protected row would be absent from
        # per_file entirely; a file with a live_above row AND a
        # disabled block contributes exactly one).
        total, per_file, debt_per_file = measure(root)
        chk('C4 NEGATIVE: a row inside `#if 1` IS counted (the C4 '
            'limitation is documented; widening the handling to #if 1 '
            'would need a deliberate review - this assertion is the '
            'tripwire)',
            'tests/codegen/inside_if1_test.cpp' in per_file)
        chk('C4 live rows are still counted: inside_if0_test.cpp has 1 '
            '(live_above, the inside_if0 body is blanked by C4)',
            len(per_file.get('tests/codegen/inside_if0_test.cpp', [])) == 1)
        chk('C4 live rows are still counted: inside_iffalse_test.cpp has 1 '
            '(live_above_iffalse, the inside_iffalse body is blanked)',
            len(per_file.get('tests/codegen/inside_iffalse_test.cpp', [])) == 1)
        # C5 fixtures: a helper defined in tests/support/*.hpp that
        # takes a JitResult and reads .message must protect a calling
        # body, even when the helper's name does NOT match NAME_MARKERS
        # and the file under scan does NOT itself define any local
        # helpers. Before C5, this case was silently counted as
        # unprotected (the recommended fix in issue #3892's own
        # rationale was "use a shared helper", but the shared helper
        # was invisible to this ratchet).
        _mk(root, 'tests/support/c5_shared_helper_test.hpp', '''
#pragma once
#include <string>
namespace c5_test_support {
inline bool hasExpectedRejectionMsg(const bezel::test::JitResult& r,
                                    const char* needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}
}
''')
        _mk(root, 'tests/codegen/uses_support_helper_test.cpp', '''
#include "support/c5_shared_helper_test.hpp"
BEZEL_TEST(fixture_suite, protected_via_support_helper) {
    auto r = runAot("module M;");
    BEZEL_EXPECT_FALSE(r.ran);
    if (!c5_test_support::hasExpectedRejectionMsg(r, "expected text")) return;
}
''')
        chk('C5 POSITIVE: a test whose only protection is a call to a '
            'tests/support/*.hpp-defined helper that takes a JitResult '
            'and reads .message does NOT count (issue #3892\'s '
            '"use a shared helper" recommendation)',
            'tests/codegen/uses_support_helper_test.cpp'
            not in per_file)
        _mk(root, 'tests/support/c5_skip_only_helper_test.hpp', '''
#pragma once
namespace c5_test_support {
inline bool toolchainWasSkippedShape(const bezel::test::JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}
}
''')
        _mk(root, 'tests/codegen/uses_skip_only_helper_test.cpp', '''
#include "support/c5_skip_only_helper_test.hpp"
BEZEL_TEST(fixture_suite, protected_via_skip_only_helper) {
    auto r = runAot("module M;");
    BEZEL_EXPECT_FALSE(r.ran);
    if (c5_test_support::toolchainWasSkippedShape(r)) return;
}
''')
        _mk(root, 'tests/support/c5_nonjit_helper_test.hpp', '''
#pragma once
namespace c5_test_support {
inline bool someOtherCheck(const std::string& s) {
    return s.find("hello") != std::string::npos;
}
}
''')
        _mk(root, 'tests/codegen/uses_nonjit_helper_test.cpp', '''
#include "support/c5_nonjit_helper_test.hpp"
BEZEL_TEST(fixture_suite, protected_via_non_jit_helper) {
    auto r = runAot("module M;");
    BEZEL_EXPECT_FALSE(r.ran);
    if (c5_test_support::someOtherCheck(std::string("hello"))) return;
}
''')
        # fix #6538 fixtures: the DELEGATION closure. A helper whose own
        # body contains no `.message` read but which FORWARDS to a
        # text-checking assertion pins its diagnostic wording just as much
        # as one that reads `.message` itself, and must be protective. The
        # real shape is tests/sema/dict_range_index_6500_test.cpp's
        # `rejectedWith` -> `failedAtCompileTimeWithRealLocation` +
        # `diagnosticMessageContains`, both in
        # tests/support/aot_result_asserts.hpp.
        _mk(root, 'tests/support/c6_text_asserts_test.hpp', '''
#pragma once
#include <string>
namespace c6_test_support {
inline bool c6HasText(const bezel::test::JitResult& r, const char* needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}
}
''')
        _mk(root, 'tests/codegen/c6_delegates_to_support_text_test.cpp', '''
#include "support/c6_text_asserts_test.hpp"
namespace {
bool c6RejectedWith(const bezel::test::JitResult& r, const char* msg) {
    return c6_test_support::c6HasText(r, msg);
}
}
BEZEL_TEST(fixture_suite, c6_delegates_to_support_text) {
    auto r = runAot("module M;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(c6RejectedWith(r, "range index `a..b` is not admitted"));
}
''')
        # Transitivity: the closure must follow a TWO-hop chain, not just
        # one. The real two-hop shape is
        # tests/sema/cyclic_class_const_initializer_4865_test.cpp's
        # `rejectedWith` -> `hasDiag` -> `.message`.
        # ORDERING IS THE POINT (reviewer NOTE 2 on the first version of
        # this fixture). `_protective_helpers` iterates a dict, so bodies
        # arrive in DECLARATION order: a chain written leaf-first is
        # already fully resolved by a SINGLE pass and therefore cannot
        # tell a fixpoint from a one-shot loop. This fixture declares the
        # leaf LAST, so c6Outer is examined before c6Middle before c6Leaf
        # and only an actual fixpoint resolves it. The leaf-FIRST mirror
        # is pinned separately below, so BOTH orderings are covered and a
        # future single-pass simplification has to pass one of them no
        # matter which direction it is written in.
        _mk(root, 'tests/codegen/c6_transitive_delegation_test.cpp', '''
namespace {
bool c6Outer(const bezel::test::JitResult& r, const char* n) {
    return c6Middle(r, n);
}
bool c6Middle(const bezel::test::JitResult& r, const char* n) {
    return c6Leaf(r, n);
}
bool c6Leaf(const bezel::test::JitResult& r, const char* n) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(n) != std::string::npos) return true;
    return false;
}
}
BEZEL_TEST(fixture_suite, c6_transitive_delegation) {
    auto r = runAot("module M;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(c6Outer(r, "some specific rejection text"));
}
''')
        # ...and the leaf-FIRST mirror, so the ordering blind spot is
        # pinned from both sides rather than only in the direction that
        # happens to break a single pass.
        _mk(root, 'tests/codegen/c6_transitive_leaf_first_test.cpp', '''
namespace {
bool c6LfLeaf(const bezel::test::JitResult& r, const char* n) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(n) != std::string::npos) return true;
    return false;
}
bool c6LfMiddle(const bezel::test::JitResult& r, const char* n) {
    return c6LfLeaf(r, n);
}
bool c6LfOuter(const bezel::test::JitResult& r, const char* n) {
    return c6LfMiddle(r, n);
}
}
BEZEL_TEST(fixture_suite, c6_transitive_leaf_first) {
    auto r = runAot("module M;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(c6LfOuter(r, "some specific rejection text"));
}
''')
        # #6538 NEGATIVE CONTROL, load-bearing: a helper that DELEGATES to
        # something which does NOT check message text is still NOT
        # protective. Without this fixture the closure could regress into
        # "anything that calls a helper is protective", which would hide
        # every unprotected row in the tree.
        _mk(root, 'tests/codegen/c6_delegates_to_non_text_test.cpp', '''
namespace {
bool c6RanFlag(const bezel::test::JitResult& r) { return r.ran; }
bool c6ExitCodeIs(const bezel::test::JitResult& r, int c) { return r.exitCode == c; }
bool c6DelegatesToNonText(const bezel::test::JitResult& r) {
    return c6RanFlag(r) || c6ExitCodeIs(r, 139);
}
}
BEZEL_TEST(fixture_suite, c6_delegates_to_non_text) {
    auto r = runAot("module M;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(c6DelegatesToNonText(r));
}
''')
        # #6538 NEGATIVE CONTROL, the sharp one: a helper that delegates to
        # a GENUINELY text-checking shared assertion but passes it the SKIP
        # literal is still a SKIP-ONLY bail-out guard and must stay denied.
        # The real shape is tests/support/aot_leak_detection_4638_test.cpp's
        # `skipped` -> `bezel::test::diagnosticMessageContains(r, "SKIP")`,
        # which guards five `if (skipped(r)) return;` absorbing sites.
        _mk(root, 'tests/codegen/c6_delegates_skip_literal_test.cpp', '''
#include "support/c6_text_asserts_test.hpp"
namespace {
bool c6SkippedShape(const bezel::test::JitResult& r) {
    return c6_test_support::c6HasText(r, "SKIP");
}
}
BEZEL_TEST(fixture_suite, c6_delegates_skip_literal) {
    auto r = runAot("module M;");
    if (c6SkippedShape(r)) return;
    BEZEL_EXPECT_FALSE(r.ran);
}
''')
        # ...and the mirror of that control: a needle that merely CONTAINS
        # the substring SKIP is a REAL needle, not the bail-out marker, so
        # the delegation edge must still count as protection. This pins the
        # exactness of _edge_is_skip_only (same exactness the DIRECT rule
        # uses) so a future loosening does not start denying real checks.
        _mk(root, 'tests/codegen/c6_skip_substring_needle_test.cpp', '''
#include "support/c6_text_asserts_test.hpp"
namespace {
bool c6CheckedToolchainAbsent(const bezel::test::JitResult& r) {
    return c6_test_support::c6HasText(r, "toolchain SKIP");
}
}
BEZEL_TEST(fixture_suite, c6_skip_substring_needle) {
    auto r = runAot("module M;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(c6CheckedToolchainAbsent(r));
}
''')
        # Mutual recursion between two helpers must NOT manufacture
        # protection: neither is in the seed set, so neither is ever added.
        _mk(root, 'tests/codegen/c6_mutual_recursion_test.cpp', '''
namespace {
bool c6Ping(const bezel::test::JitResult& r) { return r.ran || c6Pong(r); }
bool c6Pong(const bezel::test::JitResult& r) { return c6Ping(r) && !r.ran; }
}
BEZEL_TEST(fixture_suite, c6_mutual_recursion) {
    auto r = runAot("module M;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(c6Ping(r));
}
''')
        # ── issue #6546 fixtures: the three SKIP-needle spellings ──
        #
        # Each shape is a POSITIVE control: the guard is a genuine
        # bail-out (it pins only the skip marker, never the rejection
        # wording), so the row it absorbs MUST be counted. Each is written
        # in the shape that produced the hole and nothing else, so a
        # regression in one cannot be masked by the other two.
        #
        # SHAPE 1 - the needle behind a NAMED CONSTANT. Two arms, because
        # the issue names BOTH rules as holding the hole and fixing only
        # one leaves the other as a re-entry: the DIRECT arm puts the
        # constant in a helper's own `.message.find(...)`, the EDGE arm
        # puts it in a delegation argument. They are separate files
        # because `_protective_helpers` reaches the two by different code
        # paths (the leaf test vs the fixpoint's edge test), and a single
        # fixture containing both would go green if either path regressed
        # and the other still denied the helper.
        _mk(root, 'tests/codegen/c6546_named_const_direct_test.cpp', '''
namespace {
static const char* c6546SkipMarker = "SKIP";
bool c6546SkippedDirect(const bezel::test::JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(c6546SkipMarker) != std::string::npos) return true;
    return false;
}
}
BEZEL_TEST(fixture_suite, c6546_named_const_direct) {
    auto r = runAot("module M;");
    if (c6546SkippedDirect(r)) return;
    BEZEL_EXPECT_FALSE(r.ran);
}
''')
        _mk(root, 'tests/codegen/c6546_named_const_edge_test.cpp', '''
#include "support/c6_text_asserts_test.hpp"
namespace {
static const char* c6546SkipNeedle = "SKIP";
bool c6546SkippedEdge(const bezel::test::JitResult& r) {
    return c6_test_support::c6HasText(r, c6546SkipNeedle);
}
}
BEZEL_TEST(fixture_suite, c6546_named_const_edge) {
    auto r = runAot("module M;");
    if (c6546SkippedEdge(r)) return;
    BEZEL_EXPECT_FALSE(r.ran);
}
''')
        # SHAPE 2 - ADJACENT-LITERAL CONCATENATION. Again both rules: the
        # compiler joins `"SK" "IP"` into one literal before it ever means
        # anything, so a text scan has to fold the run to see it. Same
        # two-files rule as shape 1.
        _mk(root, 'tests/codegen/c6546_concat_direct_test.cpp', '''
namespace {
bool c6546SkippedConcatDirect(const bezel::test::JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SK" "IP") != std::string::npos) return true;
    return false;
}
}
BEZEL_TEST(fixture_suite, c6546_concat_direct) {
    auto r = runAot("module M;");
    if (c6546SkippedConcatDirect(r)) return;
    BEZEL_EXPECT_FALSE(r.ran);
}
''')
        _mk(root, 'tests/codegen/c6546_concat_edge_test.cpp', '''
#include "support/c6_text_asserts_test.hpp"
namespace {
bool c6546SkippedConcatEdge(const bezel::test::JitResult& r) {
    return c6_test_support::c6HasText(r, "SK" "IP");
}
}
BEZEL_TEST(fixture_suite, c6546_concat_edge) {
    auto r = runAot("module M;");
    if (c6546SkippedConcatEdge(r)) return;
    BEZEL_EXPECT_FALSE(r.ran);
}
''')
        # SHAPE 3 - the marker at the CALL SITE, not in the helper body.
        # `c6546SkippedCallSite` FORWARDS a needle parameter, so the helper
        # is genuinely text-checking and the closure is right to call it
        # protective; the hole is that the edge rule only ever reads the
        # edge INSIDE the helper and never sees the `"SKIP"` the caller
        # passes. This fixture therefore discriminates the call-site check
        # alone - neither of the shape-1/2 mechanisms can catch it, which
        # is why it is a separate file and a separate assertion.
        _mk(root, 'tests/codegen/c6546_call_site_test.cpp', '''
#include "support/c6_text_asserts_test.hpp"
namespace {
bool c6546SkippedCallSite(const bezel::test::JitResult& r, const char* needle) {
    return c6_test_support::c6HasText(r, needle);
}
}
BEZEL_TEST(fixture_suite, c6546_call_site) {
    auto r = runAot("module M;");
    if (c6546SkippedCallSite(r, "SKIP")) return;
    BEZEL_EXPECT_FALSE(r.ran);
}
''')
        # ...and the load-bearing CONTROL for shape 3: the SAME helper,
        # same file, called with a REAL needle must still protect. Without
        # this arm a fix that simply stopped honouring protective helper
        # calls would go green on the fixture above.
        _mk(root, 'tests/codegen/c6546_call_site_real_needle_test.cpp', '''
#include "support/c6_text_asserts_test.hpp"
namespace {
bool c6546RealNeedleCallSite(const bezel::test::JitResult& r, const char* needle) {
    return c6_test_support::c6HasText(r, needle);
}
}
BEZEL_TEST(fixture_suite, c6546_call_site_real_needle) {
    auto r = runAot("module M;");
    BEZEL_EXPECT_TRUE(c6546RealNeedleCallSite(r, "`a..b` is not admitted"));
    BEZEL_EXPECT_FALSE(r.ran);
}
''')
        # #6546 companion: the marker spelled with a NUMERIC ESCAPE.
        # `"\x53KIP"` and `"\123KIP"` are the same four characters the
        # compiler sees, so a confirmation that compared SPELLINGS rather
        # than values would read them as real needles - the same re-entry as
        # shapes 1 and 2, reached by a fourth spelling.
        #
        # The `"\x053KIP"` file is a SEPARATE, load-bearing arm: C++ phase 5
        # absorbs EVERY following hex digit into one escape, so `"\x053KIP"`
        # is `SKIP` (0x053 = 'S') while a two-digit cap would read it as
        # "\x05" + "3KIP" and miss it. A single `\x53KIP` fixture does NOT
        # discriminate greediness - both readings give the same value there -
        # so this arm exists and is mutation-proved separately.
        _mk(root, 'tests/codegen/c6546_escaped_marker_test.cpp', '''
namespace {
bool c6546SkippedEscaped(const bezel::test::JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("\\x53KIP") != std::string::npos) return true;
    return false;
}
}
BEZEL_TEST(fixture_suite, c6546_escaped_marker) {
    auto r = runAot("module M;");
    if (c6546SkippedEscaped(r)) return;
    BEZEL_EXPECT_FALSE(r.ran);
}
''')
        _mk(root, 'tests/codegen/c6546_escaped_greedy_test.cpp', '''
namespace {
bool c6546SkippedEscapedGreedy(const bezel::test::JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("\\x053KIP") != std::string::npos) return true;
    return false;
}
}
BEZEL_TEST(fixture_suite, c6546_escaped_greedy) {
    auto r = runAot("module M;");
    if (c6546SkippedEscapedGreedy(r)) return;
    BEZEL_EXPECT_FALSE(r.ran);
}
''')
        _mk(root, 'tests/codegen/c6546_escaped_not_marker_test.cpp', '''
namespace {
bool c6546EscapedRealNeedle(const bezel::test::JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("\\x53AB") != std::string::npos) return true;
    return false;
}
}
BEZEL_TEST(fixture_suite, c6546_escaped_not_marker) {
    auto r = runAot("module M;");
    BEZEL_EXPECT_TRUE(c6546EscapedRealNeedle(r));
    BEZEL_EXPECT_FALSE(r.ran);
}
''')
        # Re-measure AFTER the C5 fixtures are added so the post-C5
        # NEGATIVE assertions below (the SKIP-only and non-JitResult
        # helper cases both DO count) can reference a `per_file` that
        # contains the freshly-added fixture files. The earlier
        # `per_file` captured at line ~1443 did NOT see these
        # fixtures yet - `in per_file` would vacuously fail for the
        # same reason the C4 NEGATIVE needed its own re-measure.
        total, per_file, debt_per_file = measure(root)
        chk('C5 NEGATIVE: a tests/support/*.hpp-defined helper that '
            'checks ONLY the SKIP literal is SKIP-only, NOT protective '
            '(same positive-exclusion rule as in-file helpers)',
            'tests/codegen/uses_skip_only_helper_test.cpp' in per_file)
        chk('C5 NEGATIVE: a tests/support/*.hpp-defined helper whose '
            'parameter is NOT a JitResult is NOT collected by '
            '_support_helpers (the JitResult signature check is the '
            'same as _text_checking_helpers\'s), so the row IS counted',
            'tests/codegen/uses_nonjit_helper_test.cpp' in per_file)
        chk('C5 POSITIVE (re-measured): real-helper case is still NOT '
            'counted (the union with support_helpers preserves the '
            'protection across re-measure)',
            'tests/codegen/uses_support_helper_test.cpp' not in per_file)
        # fix #6538 assertions: the DELEGATION closure, both directions.
        chk('#6538 POSITIVE: a helper whose own body has no `.message` read '
            'but DELEGATES to a tests/support/*.hpp text-checking assertion '
            'IS protective (the #6538 shape: rejectedWith -> '
            'failedAtCompileTimeWithRealLocation + diagnosticMessageContains)',
            'tests/codegen/c6_delegates_to_support_text_test.cpp'
            not in per_file)
        chk('#6538: the closure is TRANSITIVE - a two-hop chain with the '
            'leaf declared LAST (c6Outer -> c6Middle -> c6Leaf, the real '
            'rejectedWith -> hasDiag -> .message shape) is protective, '
            'which a SINGLE pass cannot resolve (bodies arrive in '
            'declaration order) - this is the arm that makes the fixture '
            'ordering load-bearing',
            'tests/codegen/c6_transitive_delegation_test.cpp' not in per_file)
        chk('#6538: the leaf-FIRST mirror of the same two-hop chain is '
            'also protective, so BOTH declaration orderings are pinned and '
            'a single-pass simplification cannot pass every fixture',
            'tests/codegen/c6_transitive_leaf_first_test.cpp'
            not in per_file)
        chk('#6538 NEGATIVE (load-bearing): a helper that delegates to '
            'something which does NOT check message text is still NOT '
            'protective - the closure must not regress into "anything '
            'that calls a helper is protective"',
            'tests/codegen/c6_delegates_to_non_text_test.cpp' in per_file)
        chk('#6538 NEGATIVE: a helper that delegates to a genuinely '
            'text-checking assertion but passes it the SKIP literal is '
            'still a SKIP-ONLY bail-out and stays denied (the real '
            '`skipped` shape in tests/support/aot_leak_detection_4638_test.cpp, '
            'which guards five absorbing sites)',
            'tests/codegen/c6_delegates_skip_literal_test.cpp' in per_file)
        chk('#6538: a needle that merely CONTAINS the substring SKIP is a '
            'REAL needle, so the delegation edge still counts as protection '
            '(_edge_is_skip_only is exact, matching the DIRECT rule)',
            'tests/codegen/c6_skip_substring_needle_test.cpp' not in per_file)
        chk('#6538: mutual recursion between two helpers must NOT '
            'manufacture protection - neither is in the seed set, so the '
            'fixpoint adds neither',
            'tests/codegen/c6_mutual_recursion_test.cpp' in per_file)
        # fix #6546 assertions: the three SKIP-needle spellings.
        chk('#6546 shape 1 (DIRECT rule): a helper reading `.message.find(kSkip)` '
            'where `kSkip` is a named constant holding the literal "SKIP" is a '
            'bail-out guard and MUST be denied (its own body carries no '
            'literal to see)',
            'tests/codegen/c6546_named_const_direct_test.cpp' in per_file)
        chk('#6546 shape 1 (EDGE rule): a helper DELEGATING with a named '
            'constant holding the literal "SKIP" is a bail-out guard and '
            'MUST be denied - the two shape-1 fixtures are separate files '
            'because the leaf test and the fixpoint edge test are separate '
            'code paths, so fixing only one leaves the other a re-entry',
            'tests/codegen/c6546_named_const_edge_test.cpp' in per_file)
        chk('#6546 shape 2 (DIRECT rule): a helper reading '
            '`.message.find("SK" "IP")` is a bail-out guard and MUST be '
            'denied (C++ joins adjacent literals before they mean anything)',
            'tests/codegen/c6546_concat_direct_test.cpp' in per_file)
        chk('#6546 shape 2 (EDGE rule): the same concatenation on a '
            'delegation edge MUST be denied, for the same two-path reason',
            'tests/codegen/c6546_concat_edge_test.cpp' in per_file)
        chk('#6546 shape 3 (call site): a needle-FORWARDING helper is '
            'genuinely text-checking and the closure is right to call it '
            'protective - but the call `skipped(r, "SKIP")` is a bail-out, '
            'and the edge rule cannot see a caller\'s literal, so the row '
            'MUST still be counted',
            'tests/codegen/c6546_call_site_test.cpp' in per_file)
        chk('#6546 shape 3 CONTROL (load-bearing): the SAME '
            'needle-forwarding shape called with a REAL needle still '
            'protects - without this arm a fix that merely stopped '
            'honouring protective helper calls would go green on the '
            'assertion above',
            'tests/codegen/c6546_call_site_real_needle_test.cpp'
            not in per_file)
        chk('#6546 companion: a marker spelled with a NUMERIC ESCAPE '
            '(`"\\x53KIP"`) is the same four characters the compiler sees, so '
            'it MUST be denied - a confirmation comparing SPELLINGS rather '
            'than values would read it as a real needle, which is the same '
            're-entry as shapes 1 and 2 by a fourth spelling',
            'tests/codegen/c6546_escaped_marker_test.cpp' in per_file)
        chk('#6546 companion: a hex escape absorbs EVERY following hex '
            'digit (C++ phase 5), so `"\\x053KIP"` is `SKIP` - a two-digit '
            'cap would read it as "\\x05"+"3KIP" and miss it. A single '
            '`"\\x53KIP"` fixture cannot discriminate this (both readings '
            'agree there), so the greedy case is a SEPARATE arm',
            'tests/codegen/c6546_escaped_greedy_test.cpp' in per_file)
        chk('#6546 companion CONTROL (load-bearing): `"\\x53AB"` is ONE '
            'character (C++ phase 5 absorbs every following hex digit), so '
            'it is NOT the marker and must still protect - without this arm '
            'a fix that resolved escapes non-greedily would go green on the '
            'assertion above',
            'tests/codegen/c6546_escaped_not_marker_test.cpp'
            not in per_file)
        # C1 fixtures: the suppression marker mechanism.
        chk('C1: an AMBIGUOUS-RAN-FALSE-OK marker on the line before a '
            'BEZEL_TEST body suppresses that row from the count (the '
            'canonical use is tests/support/aot_infra_loud_3007_test.cpp'
            '::legacy_negative_assertion_shape)',
            'tests/codegen/marked_suppression_test.cpp' not in per_file)
        chk('C1 negative: an AMBIGUOUS-RAN-FALSE-OK marker with NO reason '
            'after the colon is malformed and does NOT exempt (mirrors '
            'LOCALDECL-ONLY\'s \\S requirement)',
            'tests/codegen/malformed_marker_test.cpp' in per_file)
        chk('C1 negative: an AMBIGUOUS-RAN-FALSE-OK marker whose next '
            'BEZEL_TEST is more than 8 lines below does NOT attach '
            '(mirrors the 8-line window)',
            'tests/codegen/drifted_marker_test.cpp' in per_file)
        chk('C1 negative: an AMBIGUOUS-RAN-FALSE-OK marker with no '
            'BEZEL_TEST within 8 lines below (e.g. with too many padded '
            'comment lines) does not exempt anything in this file',
            'tests/codegen/marker_no_match_test.cpp' in per_file)
        chk('exactly 24 unprotected rows across the fixture tree (12 from '
            'the C4-counted tree + 2 net C5 delta + 3 net #6538 delta + 7 '
            'net #6546 delta: every POSITIVE fixture is non-counting by '
            'design - the C5 support-helper case, the #6538 delegation '
            'cases (delegating-to-support-text, two-hop-transitive, and the '
            'SKIP-substring needle), the #6546 real-needle call site and '
            'the #6546 escaped-NOT-marker case are all correctly PROTECTED, '
            'so their rows are removed from the count; every NEGATIVE '
            'control contributes +1 because its protection claim is '
            'rejected by the SAME positive-exclusion rule that applies to '
            'in-file helpers - the C5 SKIP-only helper, the C5 non-JitResult '
            'helper, the #6538 delegate-to-non-text helper, the #6538 '
            'SKIP-literal delegation, the #6538 mutually-recursive pair, '
            'and the seven #6546 skip-marker spellings (named const on each '
            'rule, adjacent-literal concatenation on each rule, the '
            'call-site marker, and the two numerically-escaped markers))',
            total == 24)
        # (Debug prints removed; count was confirmed = 14 via a temp
        # print during the C5 fix - left the count assertion here as
        # the tripwire for any future change that lifts the C5 net.)

        # C2 fixtures: the grandfather / DEBT marker mechanism. The
        # three malformed-marker / drifted-marker / marker-no-match
        # fixtures above already contribute bare rows; C2 covers two
        # additional shapes:
        #
        # (a) a rise above the per-file ceiling WITHOUT a DEBT marker
        #     must fail even with --force (the load-bearing C2 case).
        # (b) a rise above the per-file ceiling WITH one DEBT marker
        #     per excess row must pass.
        #
        # These need a separate file so the grandfather ceiling for
        # each is well-defined by the seeded `gpath` written below.
        # C2-NEGATIVE: a single bare row in a file whose grandfathered
        # ceiling is 0 must fail (no DEBT marker = no justification).
        _mk(root, 'tests/codegen/rise_no_debt_test.cpp', '''
BEZEL_TEST(fixture_suite, rise_above_ceiling_no_debt) {
    auto r = runAot("module M;");
    BEZEL_EXPECT_FALSE(r.ran);
}
''')
        # C2-POSITIVE: same shape but with a DEBT marker on the line
        # before the BEZEL_TEST opener - one DEBT marker justifies one
        # excess row above the ceiling.
        _mk(root, 'tests/codegen/rise_with_debt_test.cpp', '''
// AMBIGUOUS-RAN-FALSE-DEBT: #3892 deliberate rise above grandfather ceiling during C2 ramp-in
BEZEL_TEST(fixture_suite, rise_above_ceiling_with_debt) {
    auto r = runAot("module M;");
    BEZEL_EXPECT_FALSE(r.ran);
}
''')
        # C2-MALFORMED: a DEBT marker with NO leading #NNNN after the
        # colon is malformed and justifies nothing (mirrors
        # LOCALDECL-DEBT's well-formed-vs-malformed split,
        # check_localdecl_only_walkers.py:290-304).
        _mk(root, 'tests/codegen/rise_malformed_debt_test.cpp', '''
// AMBIGUOUS-RAN-FALSE-DEBT: no issue number after the colon - justifies nothing
BEZEL_TEST(fixture_suite, rise_above_ceiling_malformed_debt) {
    auto r = runAot("module M;");
    BEZEL_EXPECT_FALSE(r.ran);
}
''')
        # C2-PARTIAL: a file with TWO excess rows but only ONE DEBT
        # marker - the marker covers only the marked row, the other
        # rise is unjustified and must fail.
        _mk(root, 'tests/codegen/rise_partial_debt_test.cpp', '''
// AMBIGUOUS-RAN-FALSE-DEBT: #3892 only one of the two excess rows is marked
BEZEL_TEST(fixture_suite, rise_partial_debt_first) {
    auto r = runAot("module M;");
    BEZEL_EXPECT_FALSE(r.ran);
}
BEZEL_TEST(fixture_suite, rise_partial_debt_second) {
    auto r = runAot("module M;");
    BEZEL_EXPECT_FALSE(r.ran);
}
''')

        # Re-measure with the four C2 fixtures added; total grows by 5
        # (rise_no_debt=1, rise_with_debt=1, rise_malformed_debt=1,
        # rise_partial_debt=2). The grandfather seed written below is
        # then written from this expanded per-file table; each file's
        # initial ceiling equals its current count (so the seed tree
        # itself passes), and the C2 assertions perturb from there.
        total_with_c2, per_file_with_c2, debt_per_file_with_c2 = measure(root)
        chk('C2 fixtures add 5 unprotected rows (1+1+1+2)',
            total_with_c2 == total + 5)
        chk('C2 fixtures include the C4 fixtures, but the C4 fixtures '
            'also pass C4\'s blanking cleanly (no fixture has a row '
            'inside #if 0 - all C4 live rows are outside, and the C4 '
            'fixture is added BEFORE C2 measurement, so the C2 delta '
            'is exactly 5)',
            'tests/codegen/inside_if0_test.cpp' in per_file_with_c2)
        chk('C2: a well-formed DEBT marker contributes to '
            'debt_per_file for its file',
            debt_per_file_with_c2.get(
                'tests/codegen/rise_with_debt_test.cpp', 0) == 1)
        chk('C2: a malformed DEBT marker (no #NNNN) does NOT '
            'contribute to debt_per_file',
            'tests/codegen/rise_malformed_debt_test.cpp'
            not in debt_per_file_with_c2)
        chk('C2: a partial-DEBT file (1 marker, 2 rows) contributes '
            'only 1 to debt_per_file',
            debt_per_file_with_c2.get(
                'tests/codegen/rise_partial_debt_test.cpp', 0) == 1)

        bpath = root / 'baseline'
        write_baseline(total, bpath)
        # C2: the grandfather file lives alongside the baseline in the
        # temp tree for the self-test. Seed it with the current per-file
        # counts so the baseline / ceiling match (the gate mode sees a
        # balanced tree; C2's rise-above-ceiling tests below perturb it).
        # Hand-write the initial grandfather (tighten() is monotone-
        # downward only and cannot raise from {}).
        gpath = root / 'grandfather'
        initial_grand = _per_file_counts(per_file_with_c2)
        gpath.write_text('\n'.join(
            ['# initial grandfather for self-test'] +
            ['%-4d %s' % (n, p) for p, n in sorted(initial_grand.items())]
        ) + '\n')
        # Re-write baseline to match the C2-expanded count too, so
        # baseline = total_with_c2 (the gate must not see a ROSE from
        # C2's own fixtures).
        write_baseline(total_with_c2, bpath)
        good, msgs, _ = check(root, bpath, gpath, stale=True)
        chk('a tree matching its baseline + grandfather passes',
            good and not msgs)

        # C2 specific: a rise above the per-file ceiling without a
        # DEBT marker must fail - the LOAD-BEARING C2 case.
        # Pick rise_no_debt_test.cpp (ceiling=1, count=1 currently).
        # Drop the ceiling for it to 0 by re-banking just that file.
        _mk(root, 'tests/codegen/rise_no_debt_lower_test.cpp', '''
// AMBIGUOUS-RAN-FALSE-DEBT: #3892 one marker covers the rise
BEZEL_TEST(fixture_suite, with_marker_only) {
    auto r = runAot("module M;");
    BEZEL_EXPECT_FALSE(r.ran);
}
''')
        # Write a grandfather with a tight ceiling (0) for this file -
        # the existing row will then be a rise above the ceiling, and
        # the DEBT marker above justifies it.
        gpath_tight = root / 'grandfather_tight'
        write_grandfather({}, {
            'tests/codegen/rise_no_debt_test.cpp': 0,
            'tests/codegen/rise_no_debt_lower_test.cpp': 0,
        }, gpath_tight)
        # The "rise_no_debt_test.cpp" file has 1 row, ceiling=0, no DEBT
        # marker - this MUST fail.
        good, msgs, _ = check(root, bpath, gpath_tight, stale=True)
        chk('C2: a file with rows above its grandfathered ceiling and '
            'no DEBT marker FAILS (the load-bearing case)',
            not good and any(
                'rise_no_debt_test.cpp' in m and 'above its grandfathered '
                'ceiling' in m
                for m in msgs))
        # The "rise_no_debt_lower_test.cpp" file has 1 row, ceiling=0,
        # ONE DEBT marker - this MUST pass.
        good, msgs, _ = check(root, bpath, gpath_tight, stale=True)
        chk('C2: a file with rows above its grandfathered ceiling and '
            'one well-formed DEBT marker PASSES',
            not any('rise_no_debt_lower_test.cpp' in m for m in msgs))
        # A missing grandfather file reads as an ALL-ZERO ceiling
        # (fail closed) - the entire unprotected count is then a rise,
        # every excess row needs a DEBT marker.
        good, msgs, _ = check(root, bpath, root / 'no-such-grandfather',
                              stale=True)
        chk('C2: a missing grandfather file reads as an ALL-ZERO '
            'ceiling (fail closed)',
            not good and any('above its grandfathered ceiling' in m
                             for m in msgs))

        # CLEANUP - remove all the C2-specific fixtures so the rest
        # of the self-test (rise/run/STALE HEADROOM/--update checks)
        # runs against the original fixture tree without the C2
        # deltas. The C2 fixtures are assertions about C2's behavior;
        # they should not perturb the legacy test sequence.
        for c2_path in ['tests/codegen/rise_no_debt_test.cpp',
                        'tests/codegen/rise_with_debt_test.cpp',
                        'tests/codegen/rise_malformed_debt_test.cpp',
                        'tests/codegen/rise_partial_debt_test.cpp',
                        'tests/codegen/rise_no_debt_lower_test.cpp']:
            p = root / c2_path
            if p.exists():
                p.unlink()
        # Re-measure NOW (post-C2-cleanup) so the cleanup is reflected.
        # The baseline and grandfather are then re-written to the
        # CURRENT count and ceilings (C4 fixtures are still in place
        # since they were added BEFORE C2). The legacy
        # rise/run/STALE-HEADROOM sequence expects a baseline that
        # matches the post-cleanup tree, not the pre-C4 baseline.
        total_post_cleanup, per_file_post_cleanup, _ = measure(root)
        write_baseline(total_post_cleanup, bpath)
        existing_grand = read_grandfather(gpath)
        write_grandfather(existing_grand,
                          _per_file_counts(per_file_post_cleanup), gpath)
        # Update the local `total` so subsequent assertions that compare
        # against it use the post-cleanup count (C4 fixtures ARE in
        # place; only the C2-specific deltas are gone).
        total = total_post_cleanup

        # A RISE must fail.
        _mk(root, 'tests/codegen/new_bad_test.cpp', '''
BEZEL_TEST(fixture_suite, new_bad) {
    auto r = runAot("module M;");
    BEZEL_EXPECT_FALSE(r.ran);
}
''')
        good, msgs, _ = check(root, bpath, gpath, stale=True)
        chk('a new unprotected row FAILS the ratchet', not good)
        chk('the message names a rise', any('ROSE' in m for m in msgs))
        chk('--update REFUSES to bank a rise',
            not check(root, bpath, gpath, stale=False)[0])
        (root / 'tests/codegen/new_bad_test.cpp').unlink()

        # STALE HEADROOM: a fix that forgot --update.
        (root / 'tests/codegen/bare_test.cpp').write_text('''
BEZEL_TEST(fixture_suite, bare_ran_false) {
    auto r = runAot("module M;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeNotRuntime(r));
}
''')
        good, msgs, _ = check(root, bpath, gpath, stale=True)
        chk('a fix that did not --update fails as STALE HEADROOM',
            not good and any('STALE HEADROOM' in m for m in msgs))
        chk('--update ACCEPTS the drop',
            check(root, bpath, gpath, stale=False)[0])
        total2, per_file2, _ = measure(root)
        write_baseline(total2, bpath)
        # C2: --update also tightens the per-file ceiling.
        existing = read_grandfather(gpath)
        cur_counts = _per_file_counts(per_file2)
        write_grandfather(existing, cur_counts, gpath)
        chk('re-baselined tree is green again',
            check(root, bpath, gpath, stale=True)[0])
        chk('the fix actually lowered the count', total2 < total)

        # A missing baseline must fail loudly.
        missing = root / 'no-such-baseline'
        try:
            check(root, missing, gpath, stale=True)
            chk('a missing baseline fails closed', False)
        except SystemExit as e:
            chk('a missing baseline fails closed', e.code == 2)

    print('check_ambiguous_ran_false --self-test: ' + ('PASS' if ok else 'FAIL'))
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
        total, per_file, _ = measure(ROOT)
        for f, rows in sorted(per_file.items(), key=lambda kv: -len(kv[1])):
            print('%3d  %s' % (len(rows), f))
            for tname, var in rows:
                print('       %s (%s)' % (tname, var))
        print('--- count=%d' % total)
        return 0

    if a.update:
        total, per_file, _ = measure(ROOT)
        if not BASELINE.exists():
            write_baseline(total)
            print('created baseline: count=%d' % total)
            return 0
        if not a.force:
            good, msgs, _ = check(ROOT, stale=False)
            if not good:
                sys.stderr.write(
                    'check_ambiguous_ran_false --update REFUSES to raise the '
                    'count:\n')
                for m in msgs:
                    sys.stderr.write('  ' + m + '\n')
                sys.stderr.write(
                    '  A rise means a new unprotected row was added. Protect '
                    'it (tests/support/aot_result_asserts.hpp, or an '
                    'equivalent message-text/location check) instead. If the '
                    'rise is genuinely intended, re-run with --force and say '
                    'why in the commit.\n')
                return 1
        write_baseline(total)
        # C2: tighten the per-file ceiling on every successful --update,
        # alongside the baseline re-bank - same model as
        # check_localdecl_only_walkers.write_grandfather().
        existing = read_grandfather()
        cur_counts = _per_file_counts(per_file)
        new_grand, _changed = write_grandfather(existing, cur_counts)
        print('re-baselined: count=%d (grandfather tightened for %d file(s))'
              % (total, len(_changed)))
        return 0

    good, msgs, per_file = check(ROOT, stale=True)
    if good:
        total, _per_file, _ = measure(ROOT)
        print('check_ambiguous_ran_false: OK (count=%d)' % total)
        return 0
    for m in msgs:
        sys.stderr.write('check_ambiguous_ran_false: ' + m + '\n')
    sys.stderr.write(
        "See issue #3892: a row pinning a compile-time rejection with only "
        "`.ran`/`hasErrors()` cannot distinguish a genuine rejection from a "
        "runtime crash/signal/timeout/ASan report. Run --list to see every "
        "unprotected row.\n")
    return 1 if a.strict else 0


if __name__ == '__main__':
    sys.exit(main())
