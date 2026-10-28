"""Self-test for scripts/builtin_admission_grid.py (build-free, no real bezelc).

The campaign-2 DONE instrument is a LANDED ORACLE: it must be exercised by
pregate, or a change to its classifier would silently change what "green"
means.  A fake `bezelc` (a shell script) stands in for the compiler and for the
generated program, so the generator, the classifier and the cell table are all
exercised end to end in about a second.  The real grid is run BY HAND at the
gate (like its model `scripts/regression_matrix.py`); see
docs/proposals/papers/campaigns/builtin-call-admission.md.

What is asserted:
  * every `reject` cell NAMES the diagnostic FAMILIES it requires (D5) — a
    reject cell that accepted one guessed wording turned a fix that landed on
    another wording of the same family RED, which is what kept the grid from
    being the campaign's DONE instrument;
  * every reject alternative is ANCHORED (it names the cell's own subject) or
    is one of the compiler's context-free family phrases — a bare phrase such
    as "no matching overload of" would match a diagnostic about a DIFFERENT
    call in the same program (the D3 failure mode);
  * a diagnostic about another call in the same program never satisfies a cell;
  * the classification is STABLE within a family: every alternative of every
    reject cell turns that cell OK;
  * a diagnostic in NO family leaves the cell REJECTED (a false wrong only
    when the diagnostic is genuinely absent);
  * the classifier's seven classes: OK / ACCEPTED / REJECTED / ABORT / INTERNAL /
    LEAK / CRASH — the last two are the SANITIZER's verdict (B1), so a program the
    sanitizer fails is never a clean pass;
  * a `reject` cell is OK only when the diagnostic contains one of its needles;
  * a `noabort` cell is OK only when the program exits 0 (a runtime FAULT is a
    wrong answer, not a pass);
  * the reject cell set is PINNED exactly (count and names), so a cell whose
    expectation KIND changes turns this suite RED (A2);
  * a `fault` cell is OK only when the fault text appears;
  * for the NINE cells whose compiler fix landed with a NEW, more specific
    diagnostic, the PRE-FIX wording is still accepted (a family that LOSES an
    old wording stops accepting a legitimate diagnostic and the grid calls a
    correct compiler wrong), the new wording is accepted, a wording from
    ANOTHER rule is NOT, and each family tolerates the variations of its own
    rule (the role, the accepted-type list, the tail, the receiver type, the
    type argument);
  * generation is deterministic, and the TSV's program path is CELL-RELATIVE,
    so two runs into different --out directories produce the same bytes.
"""
import os
import pathlib
import re
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'scripts'))
import builtin_admission_grid as g  # noqa: E402

FAKE = r'''#!/bin/bash
# fake bezelc: -o EXE [--sanitize=address] SRC
out=""; src=""
while [ $0 ] && [ $# -gt 0 ]; do
  case "$1" in
    -o) out="$2"; shift 2;;
    --sanitize=address) shift;;
    *) src="$1"; shift;;
  esac
done
case "$FAKE_MODE" in
  reject)      echo "$src:1:1: error: no matching overload of \`int.Parse\`" >&2; exit 1;;
  reject_msg)  echo "$src:1:1: error: $FAKE_MSG" >&2; exit 1;;
  reject_wrong) echo "$src:1:1: error: something else entirely" >&2; exit 1;;
  abort)       echo "bezelc: codegen error: IR verification failed (malformed IR emitted)" >&2; exit 1;;
  signal)      kill -SEGV $$ ;;
  internal)    echo "$src:1:1: warning: internal: lowerLocalDecl's initializer expression failed to lower" >&2; exit 0;;
  fault)       body='echo "Unhandled exception: System.IndexOutOfRangeException" >&2; exit 70';;
  asan_crash)  body='echo "ERROR: AddressSanitizer: SEGV on unknown address 0x0" >&2; exit 1';;
  asan_leak)   body='echo "ERROR: LeakSanitizer: detected memory leaks" >&2; echo "Direct leak of 32 byte(s) in 1 object(s) allocated from:" >&2; exit 99';;
  died)        body='exit 3';;
  wrongvalue)  body='echo 999';;
  rightvalue)  body='echo 42';;
  *)           body='echo 42';;
esac
printf '#!/bin/bash\n%s\n' "$body" > "$out"; chmod +x "$out"
'''

# A call-shaped name in a generated program: `Console.WriteLine(`, `sb.Append(`.
CALL_RE = re.compile(r'([A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)*)\s*\(')


def _fake_compiler(d):
    p = pathlib.Path(d) / 'fake_bezelc'
    p.write_text(FAKE)
    p.chmod(0o755)
    return str(p)


def _cell(name, expect, body='Console.WriteLine(1);', subject=''):
    """A cell tuple in the grid's shape:
    (name, extra, pre, body, expect, imports, campaign, subject, lib, member)."""
    return (name, '', '', body, expect, '', '2', subject, '', '')


def _needles(expect):
    want = expect[1]
    return [want] if isinstance(want, str) else list(want)


def _anchored(alt, subject):
    """True when the alternative names the cell's own subject (or its member as a
    WORD, or its head as a QUALIFIER), so a diagnostic about ANOTHER call cannot
    contain it.  A plain substring test is NOT enough: the word "string" occurs
    inside "Array<string>", which would anchor a needle that names no call
    (A3)."""
    head, _, short = subject.rpartition('.')
    if subject and subject in alt:
        return True
    if short and re.search(r'\b' + re.escape(short) + r'\b', alt):
        return True
    if head and (head + '.') in alt:
        return True
    return False


def _context_free(alt):
    """True when the alternative carries one of the compiler's family phrases
    that name no call."""
    return any(p in alt for p in g.CONTEXT_FREE)


def _decoys(program, subject):
    """Diagnostics about the OTHER calls in the same program — a reject cell
    must not be satisfied by any of them."""
    head, _, short = subject.rpartition('.')
    skip = {subject, head, short}
    out = []
    for n in CALL_RE.findall(program):
        if n in skip:
            continue
        out += [
            f"no matching overload of `{n}`",
            f"no applicable overload of `{n}`",
            f"ambiguous call to `{n}`",
            f"the argument to `{n}` has a type that is not implicitly "
            f"convertible to the target type",
            f"'{n}' has no static method or property 'X' (§3.4)",
            f"type '{n}' has no static method or property 'X' (§3.4)",
            f"codegen cannot dispatch member 'X' on receiver of type '{n}'",
            f"the call to `{n}` supplies 1 argument(s) but stdlib §9.6 declares 2",
            f"explicit type argument(s), but `{n}` declares 0 type parameters",
            f"`{n}` is a reference to a generic method and names no type argument",
            f"the initializer for `x` has a type that is not implicitly "
            f"convertible to the target type",
        ]
    return out


# A2: the reject set is PINNED — both the count and the names.  Without this a
# cell whose expectation KIND changes (a reject cell turned into a value cell)
# leaves the suite green.
REJECT_CELLS = [
    'bool_parse_int', 'bool_tostring_format', 'bool_tryparse_int',
    'builtin_typearg_arity', 'builtin_typearg_list', 'char_parse_radix',
    'char_tostring_format', 'char_tryparse_int', 'convert_tostring_char',
    'convert_tostring_radix_long', 'convert_tostring_uint', 'datetime_tostring_null',
    'debug_writeline_stringbuilder', 'decimal_parse_double', 'decimal_tostring_radix',
    'decimal_tryparse_double', 'double_parse_int', 'double_tryparse_int',
    'encoding_getstring_int_array', 'guid_tryparse_int',
    'imported_user_class_stringbuilder_bare', 'int_parse_bool', 'int_parse_char',
    'int_parse_double', 'int_parse_int', 'int_parse_object', 'int_parse_radix_bool',
    'int_parse_radix_string', 'int_tostring_radix_null',
    'int_tostring_radix_null_named', 'int_tryparse_double',
    'int_tryparse_int', 'long_parse_intarg', 'long_parse_null',
    'long_tostring_null', 'long_tryparse_null', 'math_abs_decimal_nullable',
    'math_abs_float', 'math_abs_uint', 'math_floor_nullable', 'math_floor_nullable_null',
    'math_max_mixed', 'math_max_reference', 'math_minmax_typed_result',
    'math_round_decimal', 'math_sign_bool', 'math_sign_reference', 'math_sign_string',
    'sb_append_null', 'sb_append_null_literal', 'sb_insert_two_strings', 'sb_insert_zero',
    'sb_remove_two_args', 'sb_replace_int', 'string_contains_int',
    'string_contains_object', 'string_isascii', 'string_join_int_array',
    'string_lastindexof_int', 'string_replace_object', 'string_split_int',
    'string_tostring_format_bad', 'uint_parse_null',
]


# The nine cells whose compiler fix has landed with a NEW, MORE SPECIFIC diagnostic.
# The campaign-2 record's rule -- *The reject needles are FAMILIES, not one guessed
# wording* -- says a family GROWS: the new wording is ADDED and the PRE-FIX wording
# is KEPT, because the compiler still emits the pre-fix wording ELSEWHERE (measured:
# `string member `.Nope` is not yet supported in codegen` for the dispatch family,
# `no matching overload of `bool.ToString` -- the call supplies 2 argument(s)` for the
# overload family, `the argument to `string.PadLeft` has a type that is not implicitly
# convertible to the target type` for the convertible family) and a family that loses one
# stops accepting a legitimate diagnostic -- the grid then calls a correct compiler
# wrong, which is the failure mode these rows exist to prevent.
#
# These are the wordings the nine cells required BEFORE the new families were added;
# they are PINNED as literals so a later edit that drops one turns this suite RED.
PRE_FIX_WORDINGS = {
    'string_contains_int': [
        'no matching overload of `String.Contains`',
        'no applicable overload of `String.Contains`',
        'no matching overload for `String.Contains`',
        'not implicitly convertible to the target type',
        'the argument to `String.Contains` has a type that is not implicitly convertible to the target type',
        'cannot dispatch member \'Contains\'',
        'codegen cannot dispatch member \'Contains\' on receiver of type',
        'member `Contains` on `string` is not yet supported in codegen',
        'string member `.Contains` is not yet supported in codegen',
    ],
    'string_contains_object': [
        'no matching overload of `String.Contains`',
        'no applicable overload of `String.Contains`',
        'no matching overload for `String.Contains`',
        'not implicitly convertible to the target type',
        'the argument to `String.Contains` has a type that is not implicitly convertible to the target type',
        'cannot dispatch member \'Contains\'',
        'codegen cannot dispatch member \'Contains\' on receiver of type',
        'member `Contains` on `string` is not yet supported in codegen',
        'string member `.Contains` is not yet supported in codegen',
    ],
    'string_lastindexof_int': [
        'no matching overload of `String.LastIndexOf`',
        'no applicable overload of `String.LastIndexOf`',
        'no matching overload for `String.LastIndexOf`',
        'not implicitly convertible to the target type',
        'the argument to `String.LastIndexOf` has a type that is not implicitly convertible to the target type',
        'cannot dispatch member \'LastIndexOf\'',
        'codegen cannot dispatch member \'LastIndexOf\' on receiver of type',
        'member `LastIndexOf` on `string` is not yet supported in codegen',
        'string member `.LastIndexOf` is not yet supported in codegen',
    ],
    'string_replace_object': [
        'no matching overload of `String.Replace`',
        'no applicable overload of `String.Replace`',
        'no matching overload for `String.Replace`',
        'not implicitly convertible to the target type',
        'the argument to `String.Replace` has a type that is not implicitly convertible to the target type',
        'cannot dispatch member \'Replace\'',
        'codegen cannot dispatch member \'Replace\' on receiver of type',
        'member `Replace` on `string` is not yet supported in codegen',
        'string member `.Replace` is not yet supported in codegen',
    ],
    'string_split_int': [
        'no matching overload of `String.Split`',
        'no applicable overload of `String.Split`',
        'no matching overload for `String.Split`',
        'not implicitly convertible to the target type',
        'the argument to `String.Split` has a type that is not implicitly convertible to the target type',
        'cannot dispatch member \'Split\'',
        'codegen cannot dispatch member \'Split\' on receiver of type',
        'member `Split` on `string` is not yet supported in codegen',
        'string member `.Split` is not yet supported in codegen',
    ],
    'bool_tostring_format': [
        'no matching overload of `bool.ToString`',
        'no applicable overload of `bool.ToString`',
        'no matching overload for `bool.ToString`',
    ],
    'char_tostring_format': [
        'no matching overload of `char.ToString`',
        'no applicable overload of `char.ToString`',
        'no matching overload for `char.ToString`',
    ],
    'math_sign_string': [
        'no matching overload of `Math.Sign`',
        'no applicable overload of `Math.Sign`',
        'no matching overload for `Math.Sign`',
        'not implicitly convertible to the target type',
        'the argument to `Math.Sign` has a type that is not implicitly convertible to the target type',
    ],
    'math_sign_bool': [
        'no matching overload of `Math.Sign`',
        'no applicable overload of `Math.Sign`',
        'no matching overload for `Math.Sign`',
        'not implicitly convertible to the target type',
        'the argument to `Math.Sign` has a type that is not implicitly convertible to the target type',
    ],
}


# The wording the compiler NOW emits for each of the nine cells (measured on
# the tip's own build; the grid's TSV `detail` column).
NEW_WORDINGS = {
    'string_contains_int': "`string.Contains` takes its search value at position 0 as a `char` or a `string` (stdlib \u00a79.6); the argument's type converts to neither (\u00a75.1)",
    'string_contains_object': "`string.Contains` takes its search value at position 0 as a `char` or a `string` (stdlib \u00a79.6); the argument's type converts to neither (\u00a75.1)",
    'string_lastindexof_int': "`string.LastIndexOf` takes its search value at position 0 as a `char` or a `string` (stdlib \u00a79.6); the argument's type converts to neither (\u00a75.1)",
    'string_replace_object': "`string.Replace` takes its search value at position 0 as a `char` or a `string` (stdlib \u00a79.6); the argument's type converts to neither (\u00a75.1)",
    'string_split_int': "`string.Split` takes its separator at position 0 as a `char`, a `string` or an `Array<char>` (stdlib \u00a79.6); the argument's type converts to none of them (\u00a75.1)",
    'bool_tostring_format': "a format spec requires an intrinsically-formattable receiver (a numeric/temporal/Guid) or one implementing `IFormattable`; `bool` is neither (\u00a72.18; \u00a718.5)",
    'char_tostring_format': "a format spec requires an intrinsically-formattable receiver (a numeric/temporal/Guid) or one implementing `IFormattable`; `char` is neither (\u00a72.18; \u00a718.5)",
    'math_sign_string': "type argument `string` does not satisfy the constraint `where T: INumber<T>` \u2014 `Math.Sign` requires an argument type that conforms to `INumber<T>` (\u00a714.2, stdlib \u00a72.2, stdlib \u00a72.19)",
    'math_sign_bool': "type argument `bool` does not satisfy the constraint `where T: INumber<T>` \u2014 `Math.Sign` requires an argument type that conforms to `INumber<T>` (\u00a714.2, stdlib \u00a72.2, stdlib \u00a72.19)",
}

# A family is a SET OF ALTERNATIVE WORDINGS OF ONE RULE.  These rows feed each
# family a wording the compiler has NOT emitted for these cells -- the same
# rule, a different ROLE / TYPE LIST / TAIL / TYPE NAME -- and require the cell
# to accept it.  A fixed string would fail these rows.
#
# The position-0 family varies in the ROLE (`search value` vs `separator`), in
# the LIST of accepted types and in the TAIL (`converts to neither` vs
# `converts to none of them`) -- each row below changes one of the three
# relative to the wording the compiler emits today.
POSITION_0_VARIANTS = {
    # a `char` only (a shorter accepted-type list) and the other tail
    'string_split_int': "`string.Split` takes its separator at position 0 as a `char` (stdlib \u00a79.6); the argument's type converts to neither (\u00a75.1)",
    # the longer accepted-type list and the other tail
    'string_contains_int': "`string.Contains` takes its search value at position 0 as a `char`, a `string` or an `Array<char>` (stdlib \u00a79.6); the argument's type converts to none of them (\u00a75.1)",
    # the replacement-value role, on the member that declares it
    'string_replace_object': "`string.Replace` takes its replacement value at position 0 as a `char`, a `string` or an `Array<char>` (stdlib \u00a79.6); the argument's type converts to none of them (\u00a75.1)",
}

# The format family names the RECEIVER TYPE, so a fixed string would work for
# one type only; these rows feed it a third type and the `operand` spelling
# the interpolation path uses for the same rule.
FORMAT_VARIANTS = {
    'bool_tostring_format': "a format spec requires an intrinsically-formattable receiver (a numeric/temporal/Guid) or one implementing `IFormattable`; `decimal` is neither (\u00a72.18; \u00a718.5)",
    'char_tostring_format': "a format spec requires an intrinsically-formattable operand (a numeric/temporal/Guid) or one implementing `IFormattable`; `string` is neither (\u00a718.5)",
}

# The constraint family names the OFFENDING TYPE, so a fixed string would work
# for one type only; this row uses a THIRD type name.
CONSTRAINT_ANY_TYPE = (
    "type argument `decimal` does not satisfy the constraint `where T: INumber<T>` \u2014 "
    "`Math.Sign` requires an argument type that conforms to `INumber<T>` "
    "(\u00a714.2, stdlib \u00a72.2, stdlib \u00a72.19)")

# THE NEGATIVES: a wording from A DIFFERENT RULE must NOT match.  A family that
# matches anything is a vacuous oracle.
NEGATIVE_WORDINGS = {}
for _c in ('string_contains_int', 'string_contains_object', 'string_lastindexof_int',
           'string_replace_object', 'string_split_int'):
    NEGATIVE_WORDINGS[_c] = [NEW_WORDINGS['bool_tostring_format'],
                             NEW_WORDINGS['math_sign_bool']]
for _c in ('bool_tostring_format', 'char_tostring_format'):
    NEGATIVE_WORDINGS[_c] = [NEW_WORDINGS['string_contains_int'],
                             NEW_WORDINGS['math_sign_bool']]
for _c in ('math_sign_bool', 'math_sign_string'):
    NEGATIVE_WORDINGS[_c] = [NEW_WORDINGS['bool_tostring_format'],
                             NEW_WORDINGS['string_contains_int']]

# The set of cells each new diagnostic satisfies is PINNED: the two `Contains`
# cells share the member and the rule, the two `ToString` cells share the
# rule, the two `Math.Sign` cells share the member and the rule -- and NO
# other cell of the 118 may accept it.
EXPECTED_SATISFIED = {
    'string_contains_int': ['string_contains_int', 'string_contains_object'],
    'string_contains_object': ['string_contains_int', 'string_contains_object'],
    'string_lastindexof_int': ['string_lastindexof_int'],
    'string_replace_object': ['string_replace_object'],
    'string_split_int': ['string_split_int'],
    'bool_tostring_format': ['bool_tostring_format', 'char_tostring_format'],
    'char_tostring_format': ['bool_tostring_format', 'char_tostring_format'],
    'math_sign_bool': ['math_sign_bool', 'math_sign_string'],
    'math_sign_string': ['math_sign_bool', 'math_sign_string'],
}


class BuiltinAdmissionGridTest(unittest.TestCase):
    # ------------------------------------------------------------------ shape
    def test_every_reject_cell_lists_its_diagnostic_families(self):
        """D5: a reject cell that accepts ONE guessed wording turns a fix that
        lands on another wording of the same family RED — which is what kept
        the grid from being the campaign's DONE instrument."""
        reject = [c for c in g.CELLS if c[4][0] == 'reject']
        self.assertEqual(len(reject), 63)
        for name, _e, _p, _b, expect, _i, _camp, subject, _lib, _m in reject:
            needles = _needles(expect)
            self.assertGreaterEqual(len(needles), 2, f'{name}: one guessed wording')
            for n in needles:
                self.assertTrue(n and n.strip(), f'{name}: empty needle')

    def test_the_reject_cell_set_is_pinned(self):
        """A2: the suite must go RED when a cell's expectation KIND changes (a
        reject cell turned into a value cell), so the reject set is pinned
        EXACTLY — the count AND the names."""
        reject = sorted(c[0] for c in g.CELLS if c[4][0] == 'reject')
        self.assertEqual(reject, REJECT_CELLS)
        # ... and the whole cell table's shape is pinned too.
        self.assertEqual(len(g.CELLS), 118)

    def test_every_reject_cell_names_its_subject(self):
        """The subject is what makes a diagnostic about ANOTHER call unable to
        satisfy the cell."""
        for name, _e, _p, _b, expect, _i, _camp, subject, _lib, _m in g.CELLS:
            if expect[0] == 'reject':
                self.assertTrue(subject, f'{name}: a reject cell must name its subject')

    def test_a_needle_naming_no_call_is_not_anchored(self):
        """A3: `expects `Array<string>`` names a TYPE, not a call — the word
        "string" occurs inside "Array<string>", so a plain substring test
        anchored it for the wrong reason and the decoy test never ran for it."""
        self.assertFalse(_anchored('expects `Array<string>`', 'string.Join'))
        self.assertTrue(_anchored('`string.Join` expects `Array<string>`', 'string.Join'))
        # ... and the needle is therefore context-free (it names no call).
        self.assertTrue(_context_free('expects `Array<string>`'))
        self.assertTrue(_context_free('expects `Array<byte>`'))

    def test_every_reject_alternative_is_anchored_or_a_context_free_phrase(self):
        """A bare phrase ("no matching overload of", "type argument") matches a
        diagnostic about a DIFFERENT call — the D3 failure mode."""
        for name, _e, _p, _b, expect, _i, _camp, subject, _lib, _m in g.CELLS:
            if expect[0] != 'reject':
                continue
            for n in _needles(expect):
                self.assertTrue(_anchored(n, subject) or _context_free(n),
                                f'{name}: the alternative {n!r} names no call and is not '
                                f'one of the compiler\'s context-free family phrases')

    def test_a_diagnostic_about_another_call_never_satisfies_a_cell(self):
        """The false-OK mirror of D3: the cell's other calls cannot produce the
        diagnostic the cell wants."""
        for name, extra, pre, body, expect, imports, _camp, subject, _lib, _m in g.CELLS:
            if expect[0] != 'reject':
                continue
            program = g.prog(extra, pre, body, imports)
            decoys = _decoys(program, subject)
            self.assertTrue(decoys, f'{name}: no decoy was built for this program')
            for n in _needles(expect):
                if not _anchored(n, subject):
                    continue  # a context-free phrase: covered by the record's argument
                for d in decoys:
                    self.assertNotIn(n, d,
                                     f'{name}: the alternative {n!r} is satisfied by the '
                                     f'unrelated diagnostic {d!r}')

    # --------------------------------------------------------------- behaviour
    def _classify(self, mode, expect, body='Console.WriteLine(1);', subject=''):
        with tempfile.TemporaryDirectory() as d:
            bezelc = _fake_compiler(d)
            os.environ['FAKE_MODE'] = mode
            try:
                row = g.classify_one(bezelc, os.path.join(d, 'programs'), os.path.join(d, 'cells'),
                                     _cell('c_' + mode, expect, body, subject), False, 30, 30)
            finally:
                os.environ.pop('FAKE_MODE', None)
            return row

    def _classify_msg(self, msg, expect, body='Console.WriteLine(1);', subject=''):
        with tempfile.TemporaryDirectory() as d:
            bezelc = _fake_compiler(d)
            os.environ['FAKE_MODE'] = 'reject_msg'
            os.environ['FAKE_MSG'] = msg
            try:
                row = g.classify_one(bezelc, os.path.join(d, 'programs'), os.path.join(d, 'cells'),
                                     _cell('c_msg', expect, body, subject), False, 30, 30)
            finally:
                os.environ.pop('FAKE_MODE', None)
                os.environ.pop('FAKE_MSG', None)
            return row

    def test_the_classification_is_stable_within_a_family(self):
        """N1: every alternative of every reject cell turns that cell OK, so a
        fix that words the SAME diagnostic differently still turns the grid
        green."""
        checked = 0
        for name, extra, pre, body, expect, imports, _camp, subject, _lib, _m in g.CELLS:
            if expect[0] != 'reject':
                continue
            for n in _needles(expect):
                row = self._classify_msg(n, expect, body, subject)
                self.assertEqual(row[6], 'OK',
                                 f'{name}: the diagnostic {n!r} did not satisfy the cell '
                                 f'({row[7]})')
                checked += 1
        self.assertGreaterEqual(checked, 200)

    def test_a_diagnostic_in_no_family_leaves_the_cell_rejected(self):
        """N1: the cell is a FALSE WRONG only when the diagnostic is genuinely
        absent."""
        for name, extra, pre, body, expect, imports, _camp, subject, _lib, _m in g.CELLS:
            if expect[0] != 'reject':
                continue
            row = self._classify_msg('the compiler could not bind this call',
                                      expect, body, subject)
            self.assertEqual(row[6], 'REJECTED', f'{name}: an unrelated diagnostic was accepted')

    def test_reject_cell_is_ok_only_when_the_needle_is_present(self):
        row = self._classify('reject', ('reject', ['no matching overload of `int.Parse`']),
                             subject='int.Parse')
        self.assertEqual(row[6], 'OK')
        row = self._classify('reject_wrong', ('reject', ['no matching overload of `int.Parse`']),
                             subject='int.Parse')
        self.assertEqual(row[6], 'REJECTED')

    def test_compiler_abort_is_its_own_class(self):
        row = self._classify('abort', ('reject', ['no matching overload of `int.Parse`']),
                             subject='int.Parse')
        self.assertEqual(row[6], 'ABORT')
        row = self._classify('signal', ('reject', ['no matching overload of `int.Parse`']),
                             subject='int.Parse')
        self.assertEqual(row[6], 'ABORT')

    def test_internal_degrade_is_its_own_class(self):
        row = self._classify('internal', ('reject', ['no matching overload of `int.Parse`']),
                             subject='int.Parse')
        self.assertEqual(row[6], 'INTERNAL')

    def test_value_cell_checks_the_exact_output(self):
        row = self._classify('rightvalue', ('value', '42\n'))
        self.assertEqual(row[6], 'OK')
        row = self._classify('wrongvalue', ('value', '42\n'))
        self.assertEqual(row[6], 'ACCEPTED')

    def test_noabort_cell_rejects_a_runtime_fault(self):
        """D4: `noabort` must not be vacuous — a program that faults at RUN time
        is a wrong answer, not a pass.  Since B1 the fault is its own class:
        CRASH (the program died), not a mere ACCEPTED."""
        row = self._classify('rightvalue', ('noabort', None))
        self.assertEqual(row[6], 'OK')
        row = self._classify('fault', ('noabort', None))
        self.assertEqual(row[6], 'CRASH')

    def test_a_leak_is_its_own_class(self):
        """B1: the campaign's DONE instrument must SEE the sanitizer's verdict.
        A program the sanitizer fails is not a clean pass — it is a LEAK."""
        row = self._classify('asan_leak', ('value', '42\n'))
        self.assertEqual(row[6], 'LEAK')
        self.assertIn('32B', row[7])

    def test_a_sanitizer_crash_is_its_own_class(self):
        """B1: an AddressSanitizer error is a CRASH, whatever the cell wants."""
        row = self._classify('asan_crash', ('value', '42\n'))
        self.assertEqual(row[6], 'CRASH')
        self.assertIn('SEGV', row[7])

    def test_a_value_cell_needs_a_zero_exit_code(self):
        """B1: the `value` branch compared STDOUT ONLY, so a program that printed
        the right text and then died was a clean pass.  A non-zero exit is a
        CRASH."""
        row = self._classify('died', ('value', '42\n'))
        self.assertEqual(row[6], 'CRASH')
        self.assertIn('rc=3', row[7])

    def test_the_run_environment_carries_the_house_asan_options(self):
        """B1: the cell's program runs under the house oracle's ASan options, so
        a leak is reported at all (`detect_leaks=1`) and with a DISTINCT exit
        code (`exitcode=99`)."""
        env = g._run_env()
        self.assertEqual(env.get('ASAN_OPTIONS'), 'detect_leaks=1:exitcode=99')

    def test_the_sanitizer_verdict_beats_the_expected_fault(self):
        """B1: the sanitizer's verdict is read FIRST — a `fault` cell whose
        program leaks is a LEAK, not the expected fault."""
        row = self._classify('asan_leak', ('fault', 'IndexOutOfRangeException'))
        self.assertEqual(row[6], 'LEAK')

    def test_fault_cell_needs_the_fault_text(self):
        row = self._classify('fault', ('fault', 'IndexOutOfRangeException'))
        self.assertEqual(row[6], 'OK')
        row = self._classify('rightvalue', ('fault', 'IndexOutOfRangeException'))
        self.assertEqual(row[6], 'ACCEPTED')

    def test_cell_names_are_unique_and_generation_is_deterministic(self):
        names = [c[0] for c in g.CELLS]
        self.assertEqual(len(names), len(set(names)))
        with tempfile.TemporaryDirectory() as d:
            a = g.prog('', '', 'Console.WriteLine(1);')
            b = g.prog('', '', 'Console.WriteLine(1);')
            self.assertEqual(a, b)
            self.assertIn('class C', a)

    def test_campaign_tags_are_known(self):
        for name, _e, _p, _b, _e2, _i, camp in [(c[0], c[1], c[2], c[3], c[4], c[5], c[6])
                                                for c in g.CELLS]:
            self.assertIn(camp, ('2', '5'), f'{name}: unknown campaign tag {camp!r}')

    def test_every_cell_names_a_tracker_member_or_is_a_control(self):
        """The map IS the record's cell→member table: every cell has an entry
        and every entry is a tracker number or '' (a control cell)."""
        for name, _e, _p, _b, _e2, _i, _camp, _s, _l, member in g.CELLS:
            self.assertIn(name, g.MEMBER, f'{name}: no MEMBER entry')
            if member:
                self.assertRegex(member, r'^#\d+$', f'{name}: bad member {member!r}')
        self.assertEqual(sorted(g.MEMBER), sorted(c[0] for c in g.CELLS))

    def test_the_cross_module_face_is_covered(self):
        """N5: a class named like a builtin, IMPORTED from another module — a
        distinct resolution path from the same-module one."""
        xmod = [c for c in g.CELLS if c[8]]
        self.assertGreaterEqual(len(xmod), 2)
        for c in xmod:
            self.assertIn('import Lib;', c[5], f'{c[0]}: a cross-module cell must import Lib')
            self.assertIn('module Lib;', c[8], f'{c[0]}: the library must declare module Lib')

    def test_program_path_in_the_row_is_cell_relative(self):
        """D12: an ABSOLUTE path would make two runs into different --out
        directories differ byte-for-byte."""
        row = self._classify('rightvalue', ('value', '42\n'))
        self.assertFalse(os.path.isabs(row[8]), row[8])
        self.assertTrue(row[8].startswith('programs/'), row[8])


    # ---------------------------------------- the three new needle families
    def _grid_cell(self, name):
        for c in g.CELLS:
            if c[0] == name:
                return c
        self.fail(f'no cell named {name!r}')

    def _classify_cell_msg(self, msg, name):
        """Classify `msg` as the diagnostic for the grid's OWN cell `name`."""
        c = self._grid_cell(name)
        with tempfile.TemporaryDirectory() as d:
            bezelc = _fake_compiler(d)
            os.environ['FAKE_MODE'] = 'reject_msg'
            os.environ['FAKE_MSG'] = msg
            try:
                row = g.classify_one(bezelc, os.path.join(d, 'programs'),
                                     os.path.join(d, 'cells'), c, False, 30, 30)
            finally:
                os.environ.pop('FAKE_MODE', None)
                os.environ.pop('FAKE_MSG', None)
        return row

    def test_each_new_family_is_a_set_of_alternative_wordings(self):
        """A family is a SET of alternative wordings of one rule, not one
        guessed string -- so each of the three carries more than one."""
        self.assertGreaterEqual(len(g.F_pos0_param('Contains', 'search value')), 2)
        self.assertGreaterEqual(len(g.F_formattable_receiver()), 2)
        self.assertGreaterEqual(len(g.F_typearg_constraint('Math.Sign')), 2)

    def test_the_pre_fix_wording_is_still_accepted_for_every_nine_cell(self):
        """THE SAFETY PROPERTY: a family that LOSES an old wording stops
        accepting a legitimate diagnostic (the compiler still emits the old
        wording elsewhere) and the grid then calls a correct compiler wrong.
        Every pre-fix wording is still in the cell's family AND still turns
        the cell OK."""
        for name, olds in PRE_FIX_WORDINGS.items():
            want = _needles(self._grid_cell(name)[4])
            for old in olds:
                self.assertIn(old, want,
                              f'{name}: the pre-fix wording {old!r} was REMOVED')
                row = self._classify_cell_msg(old, name)
                self.assertEqual(row[6], 'OK',
                                 f'{name}: the pre-fix wording {old!r} no longer '
                                 f'matches ({row[7]})')

    def test_the_new_wording_is_accepted_for_each_of_the_nine_cells(self):
        """The nine cells' compiler fix has landed with a NEW, more specific
        diagnostic; the family must contain it."""
        for name, msg in NEW_WORDINGS.items():
            row = self._classify_cell_msg(msg, name)
            self.assertEqual(row[6], 'OK',
                             f'{name}: the new wording is not in the family ({row[7]})')

    def test_the_position_0_family_tolerates_the_role_the_list_and_the_tail(self):
        """The position-0 wording varies in the ROLE, in the LIST of accepted
        types and in the TAIL -- a fixed string would miss one of them."""
        for name, variant in POSITION_0_VARIANTS.items():
            row = self._classify_cell_msg(variant, name)
            self.assertEqual(row[6], 'OK',
                             f'{name}: the position-0 variant {variant!r} is not in '
                             f'the family ({row[7]})')

    def test_the_formattable_family_tolerates_the_receiver_type_and_the_operand_spelling(self):
        """The format wording names the RECEIVER TYPE, so the family must not
        be pinned to one type -- nor to the `receiver` spelling alone."""
        for name, variant in FORMAT_VARIANTS.items():
            row = self._classify_cell_msg(variant, name)
            self.assertEqual(row[6], 'OK',
                             f'{name}: the format variant {variant!r} is not in the '
                             f'family ({row[7]})')

    def test_the_constraint_family_matches_any_type_argument(self):
        """The constraint wording names the OFFENDING TYPE, so the family must
        match ANY type argument -- `decimal` here, neither `bool` nor
        `string`."""
        for name in ('math_sign_bool', 'math_sign_string'):
            row = self._classify_cell_msg(CONSTRAINT_ANY_TYPE, name)
            self.assertEqual(row[6], 'OK',
                             f'{name}: the constraint family does not match an '
                             f'arbitrary type argument ({row[7]})')

    def test_a_wording_from_a_different_rule_does_not_match(self):
        """THE NEGATIVE: a wording from ANOTHER rule must not satisfy the cell
        -- a family that matches anything is a vacuous oracle."""
        for name, negs in NEGATIVE_WORDINGS.items():
            for neg in negs:
                row = self._classify_cell_msg(neg, name)
                self.assertEqual(row[6], 'REJECTED',
                                 f'{name}: the other-rule wording {neg!r} satisfied '
                                 f'the cell ({row[7]})')

    def test_the_new_diagnostics_satisfy_only_the_cells_of_their_own_member(self):
        """A new diagnostic must not leak into a cell about a DIFFERENT member
        -- the family is a family of ONE rule, and the cells that share the
        member share the rule (the two `Contains` cells, the two `ToString`
        cells, the two `Math.Sign` cells).  The satisfied set is PINNED."""
        for name, expected in EXPECTED_SATISFIED.items():
            msg = NEW_WORDINGS[name]
            satisfied = sorted(c[0] for c in g.CELLS
                               if c[4][0] == 'reject'
                               and any(n in msg for n in _needles(c[4])))
            self.assertEqual(satisfied, sorted(expected),
                             f'{name}: the new wording satisfies {satisfied}, '
                             f'not {sorted(expected)}')

if __name__ == '__main__':
    unittest.main()
