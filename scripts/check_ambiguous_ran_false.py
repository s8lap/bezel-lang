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
        (c) an in-file helper taking a JitResult-shaped parameter whose OWN
            body reads `.message` (the same "classify a local predicate by
            its body, not its name" technique scripts/check_vacuous_tests.py
            uses, for a helper this script has not been told the name of),
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

THE BASELINE IS EXACT, NOT A CEILING - same rule every ratchet in this file's
family uses (scripts/check_deprecated_jit_oracle.py, scripts/check_localdecl_
only_walkers.py, ...). A count BELOW baseline fails as STALE HEADROOM: a sweep
PR that fixes rows but forgets `--update` silently buys room for the next
regression.

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


def _is_digit_separator(text, i):
    """True if the `'` at `text[i]` is a C++14 digit separator (`1'000`,
    `0xFF'FF`) rather than the start of a char literal that merely abuts an
    identifier character on both sides (`return'a'`, `x'y'`) - fix #3920.

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
    """
    n = len(text)
    if not (i > 0 and i + 1 < n and _WORDCHAR.match(text[i - 1]) and
            _WORDCHAR.match(text[i + 1])):
        return False
    j = i - 1
    while j > 0 and _WORDCHAR.match(text[j - 1]):
        j -= 1
    return text[j].isdigit()


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


def _text_checking_helpers(text):
    """Set of in-file helper names taking a JitResult-shaped parameter whose
    OWN body reads `.message` - classified by BODY, not name (the
    check_vacuous_tests.py technique), so an oddly-named local helper is
    still recognised as protective.

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
    under-flagging, never over-flagging."""
    out = set()
    for m in HELPER.finditer(text):
        params = m.group(2)
        if 'JitResult' not in params:
            continue
        body = _balanced_body(text, m.end() - 1)
        if '.message' not in body:
            continue
        finds = _message_find_call_args(body)
        if finds and all(a == SKIP_LITERAL for a in finds):
            continue  # positively confirmed skip-only - deny
        out.add(m.group(1))
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


def _protected(body, var, text_helpers):
    if re.search(r'\.message\s*\.\s*find\s*\(', body):
        return True
    if _name_marker_call(body, var):
        return True
    for name in text_helpers:
        if re.search(r'\b' + re.escape(name) + r'\s*\(\s*' + re.escape(var) +
                      r'\s*[,)]', body):
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


def scan_file(path, raw):
    """-> list of (test_name, var) unprotected rows in this file's raw
    text. `raw` is the file's actual on-disk text (the marker-attach
    logic reads from it directly, since both AMBIGUOUS-RAN-FALSE-OK and
    AMBIGUOUS-RAN-FALSE-DEBT are comments and would be stripped from the
    comment-stripped copy - check_localdecl_only_walkers.py:160-165 makes
    the same point)."""
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
    if not (RAN_FALSE.search(text) or HAS_ERRORS_TRUE.search(text)):
        return []
    text_helpers = _text_checking_helpers(text)
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
            if not _protected(body, var, text_helpers) and not test_exempt:
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
    for p in sorted(tests_dir.rglob('*_test.cpp')):
        try:
            raw = p.read_text(errors='replace')
        except OSError:
            continue
        rows = scan_file(p, raw)
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
        chk('exactly 9 unprotected rows across the fixture tree (6 from '
            'the existing tree + the 3 negative-control C1 fixtures: '
            'malformed marker, drifted marker, marker-no-match)',
            total == 9)

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
        # Re-write baseline at the original count, grandfather at the
        # original per-file ceilings, so the legacy flow matches.
        write_baseline(total, bpath)
        existing_grand = read_grandfather(gpath)
        write_grandfather(existing_grand, _per_file_counts(per_file), gpath)

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
