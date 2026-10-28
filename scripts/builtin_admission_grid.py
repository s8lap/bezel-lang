#!/usr/bin/env python3
"""builtin_admission_grid.py — the campaign-2 (builtin-call admission) DONE instrument.

WHAT IT IS
    A grid of generated Bezel programs.  Each cell is one built-in call reached
    with a receiver kind, an argument kind and a spelling, and each cell states
    what the compiler MUST do with it (accept with this exact output, reject
    with a located error, raise this fault, or at minimum not abort the
    compiler).  The grid is the campaign's DONE instrument: §2.3 of the master
    plan wants it green under ASan at every gate afterwards.

SHAPE
    Deliberately the shape of `scripts/regression_matrix.py`: a deterministic
    generator, one generated program per cell, a compile+run leg per cell, a
    per-cell CLASS, a TSV of every cell, and a non-zero exit when a cell is
    wrong.  Cells are named so a row can be cited in a commit body.

USAGE
    scripts/builtin_admission_grid.py --out DIR [--bezelc build/bezelc]
                                      [--jobs N] [--filter SUBSTR] [--list]

    BEZEL_AOT_FORCE_SANITIZE=address  compiles every cell with --sanitize=address
                                      (the campaign's DONE leg).  The script prints
                                      `NATIVE SANITIZER: N check(s) skipped` — N must be 0.

EXIT STATUS
    0  every selected cell matched its expectation
    1  at least one cell is wrong (the campaign's baseline number)
    2  usage / infrastructure error

WHAT A CELL'S CLASS MEANS
    OK          the cell matched its expectation
    ACCEPTED    the program compiled and ran, but not as the expectation says
    REJECTED    the program was rejected, but not as the expectation says
    ABORT       the compiler aborted (IR verification failed, an `internal:`
                degrade, a signal, a timeout) — the worst class
    INTERNAL    the program compiled with an `internal:` note — a silent
                acceptance inversion (campaign 0(a): every `internal:` degrade
                site is to become Severity::Error)

THE REJECT NEEDLES ARE FAMILIES, NOT ONE GUESSED WORDING
    A reject cell names the DIAGNOSTIC FAMILIES a correct fix may use: a list of
    the wordings the compiler emits today for that family (or the plausible
    rewording of the same message).  A fix that lands on a different wording of
    the SAME family still turns the cell green; a fix that lands on a DIFFERENT
    family turns it red.  Each cell also names the SUBJECT — the built-in member
    the cell is about — so a diagnostic about some other call in the same
    program cannot satisfy it.
"""
import argparse
import concurrent.futures
import os
import re
import subprocess
import sys


# --- diagnostic families -----------------------------------------------------
#
# The wordings below are transcribed from `src/` (see the campaign record for
# the file:function of each).  `CONTEXT_FREE` lists the family phrases that name
# NO call: the compiler's own wording for them carries no member name, so a
# cell that accepts one accepts a message that could in principle be about
# another call in the same program.  Every cell that uses one is listed in the
# record with the reason the program's only other call cannot produce that
# family.

CONTEXT_FREE = frozenset({
    "not implicitly convertible to the target type",
    "is nullable but the target type is non-nullable",
    "explicit type argument(s)",
    "inconsistent type-argument inference",
    "is a reference to a generic method",
    "the initializer for",
    "argument(s) but stdlib §3.1 declares",
    "argument(s) but stdlib §9.6 declares",
    "argument(s): stdlib §2.1 defines only",
    "expects `Array<byte>`",
    "expects `Array<string>`",
    "is ambiguous: an imported public from",
    "shadows the always-available type",
    # §2.18/§18.5: the format-spec rule names the RECEIVER TYPE, not the call.
    "a format spec requires an intrinsically-formattable",
    # §14.2: the constraint rule names the type ARGUMENT, not the call.
    "does not satisfy the constraint `where T: INumber<T>`",
})


def R(*families):
    """Merge diagnostic families into ONE needle list (order preserving)."""
    out = []
    for fam in families:
        for needle in (fam if isinstance(fam, (list, tuple)) else [fam]):
            if needle not in out:
                out.append(needle)
    return out


def _parts(subject):
    """(head, short) of a dotted member name: "StringBuilder.Append" -> ("StringBuilder", "Append")."""
    head, _, short = subject.rpartition('.')
    return head, short


def F_overload(subject):
    """§22.6: the call has no applicable candidate."""
    return [f"no matching overload of `{subject}`",
            f"no applicable overload of `{subject}`",
            f"no matching overload for `{subject}`"]


def F_convertible(subject):
    """§5.1: the argument's type does not implicitly convert to the parameter's."""
    return ["not implicitly convertible to the target type",
            f"the argument to `{subject}` has a type that is not implicitly "
            f"convertible to the target type"]


def F_nullable(subject):
    """§7.3/§7.4: the argument is nullable and the parameter is not."""
    return ["is nullable but the target type is non-nullable",
            f"the argument to `{subject}` is nullable but the target type is "
            f"non-nullable"]


def F_arity(subject, section, count):
    """The call supplies the wrong number of arguments."""
    return [f"supplies {count} argument(s) but stdlib §{section} declares",
            f"argument(s) but stdlib §{section} declares",
            f"`{subject}` supplies {count} argument(s) but stdlib §{section} declares",
            f"expects {count} argument(s) but stdlib §{section} declares"]


def F_ambiguous(subject):
    """§22.6: the call is ambiguous — no candidate is unambiguously better."""
    return [f"ambiguous call to `{subject}`",
            f"ambiguous call to {subject}",
            f"`{subject}` is ambiguous"]


def F_dispatch(subject):
    """Codegen's dispatch backstop, promoted from a degrade to an error."""
    _head, short = _parts(subject)
    return [f"cannot dispatch member '{short}'",
            f"codegen cannot dispatch member '{short}' on receiver of type",
            f"member `{short}` on `string` is not yet supported in codegen",
            f"string member `.{short}` is not yet supported in codegen"]


def F_no_such_member(receiver, member):
    """The compiler's 'no such member' families (three wordings).  Only for a
    cell whose called FORM has no member at all — MEASURED on HEAD, no other
    reject cell's member is missing in the form it is called."""
    return [f"member `{member}` on `{receiver}` is not yet supported in codegen",
            f"'{receiver}' has no static method or property '{member}' (§3.4)",
            f"type '{receiver}' has no static method or property '{member}' (§3.4)"]


def F_property(subject):
    """The member exists, but as a PROPERTY — the call spelling has no candidate."""
    _head, short = _parts(subject)
    return [f"`{short}` is a property",
            f"property `{short}`",
            f"no matching overload of `{subject}`"]


def F_typearg(subject):
    """§14: the call spells type arguments the callee does not declare."""
    return ["explicit type argument(s)",
            f"`{subject}` declares no type parameter",
            f"`{subject}` is not a generic method"]


def F_inference(subject):
    """§14.1: the arguments disagree on a type parameter."""
    return ["inconsistent type-argument inference",
            f"no matching overload of `{subject}`"]


def F_generic_ref(subject):
    """A method reference to a generic method names no type argument."""
    return ["is a reference to a generic method",
            f"`{subject}` is a reference to a generic method",
            f"`{subject}` names no type argument"]


def F_ambiguous_name(subject):
    """§2.3: a bare name is ambiguous between an imported public and an
    always-available type."""
    _head, short = _parts(subject)
    return [f"bare name `{short}` is ambiguous",
            "is ambiguous: an imported public from",
            "shadows the always-available type"]


def F_initializer(subject, var):
    """The initializer's type does not fit the declared type."""
    return ["the initializer for",
            f"the initializer for `{var}` has a type that is not implicitly "
            f"convertible to the target type"]


def F_pos0_param(member, role):
    """stdlib §9.6 + §5.1 (v0.345): the POSITION-0 parameter of a §9.6 string
    member takes its <role> as a `char` (or a `string`, or an `Array<char>`);
    the argument's type converts to none of them.

    THE WORDING VARIES IN THREE PLACES, so one fixed string misses one of
    them: the ROLE (`search value` vs `separator` vs `replacement value`), the
    LIST of accepted types (char-only vs char-or-string vs char-or-string-or-
    Array<char>) and the TAIL (`converts to neither` vs `converts to
    none of them`).  The family therefore carries the role-specific wording
    AND the member-named PREFIX, which survives a reword of the role, of the
    list and of the tail alike (`_needle_hit` is a plain substring test, so
    the prefix is the fragment that must not be a fixed string)."""
    return [f"`string.{member}` takes its {role} at position 0",
            f"`string.{member}` takes its"]


def F_formattable_receiver():
    """§2.18 + §18.5 (v0.345): a format spec requires an intrinsically-
    formattable receiver (a numeric/temporal/Guid) or one implementing
    `IFormattable`.

    The wording names the RECEIVER TYPE (`bool`, `char`, `string`), not the
    member, so a fixed string would work for one type only.  The family
    carries the type-agnostic PREFIX instead, plus the `operand` spelling the
    interpolation path (§18.5) uses for the same rule."""
    return ["a format spec requires an intrinsically-formattable receiver "
            "(a numeric/temporal/Guid) or one implementing `IFormattable`",
            "a format spec requires an intrinsically-formattable",
            "a format spec requires an intrinsically-formattable operand"]


def F_typearg_constraint(subject, constraint="INumber<T>"):
    """§14.2 + stdlib §2.2/§2.19 (v0.345): the type argument does not satisfy
    the callee's constraint.

    The wording NAMES THE OFFENDING TYPE (`bool`, `string`, ...), so a fixed
    string works for one type only.  The family carries a fragment that
    EXCLUDES the type name — the constraint phrase — and a member-named one
    (the callee's own requirement), so it matches ANY type argument."""
    return [f"does not satisfy the constraint `where T: {constraint}`",
            f"`{subject}` requires an argument type that conforms to `{constraint}`"]


CELLS = []


def C(name, body, expect, pre="", extra="", imports="", campaign="2", subject="", lib=""):
    """`expect` is (kind, want):
         ("value", TEXT)  the program must compile, run, and print exactly TEXT
         ("reject", LIST) the program must be REJECTED with a located ERROR whose
                          message contains ONE of the listed alternatives.  A
                          reject cell MUST list the DIAGNOSTIC FAMILIES a correct
                          fix may use — never a single guessed wording.
         ("fault", FRAG)  the program must compile, run, and the output must contain FRAG
         ("noabort", None) the compiler must not abort AND the program must run to
                          completion (exit code 0)
       `subject` names the built-in member the cell is about ("StringBuilder.Append");
       it is what makes a diagnostic about ANOTHER call in the same program
       unable to satisfy the cell.
       `lib` is the source of a SECOND module the cell's program imports; it is
       compiled to a library object first (`bezelc -c`) and the cell's program
       is compiled against it (`bezelc --import`) — the cross-module face.
       `campaign` names the campaign that owns the cell: "2" (this campaign) or "5"
       (the nullable-carrier unwrap, which is campaign 5's mechanism, not an
       admission gate's).
    """
    if expect[0] == 'reject':
        if not expect[1]:
            raise ValueError(f'{name}: a reject cell must name the diagnostic it requires')
        needles = [expect[1]] if isinstance(expect[1], str) else list(expect[1])
        if len(needles) < 2:
            raise ValueError(f'{name}: a reject cell must list the diagnostic FAMILIES '
                             f'it accepts, not one guessed wording')
        for n in needles:
            if not n or not n.strip():
                raise ValueError(f'{name}: empty needle')
    CELLS.append((name, extra, pre, body, expect, imports, campaign, subject, lib,
                  MEMBER.get(name, '')))


# The tracker member each cell covers.  '' marks a CONTROL cell: the legal
# spelling of a call the tracker does not name (its program must still compile
# and print what the cell says).  This map IS the campaign record's cell→member
# table, in machine-checkable form: `scripts/gate/tests/` asserts that every
# cell has an entry and that every entry names a cell.
MEMBER = {
    # B1 parse
    'int_parse_string': '',
    'int_parse_int': '#7294',
    'int_parse_bool': '#7299',
    'int_parse_char': '#7299',
    'int_parse_double': '#7294',
    'int_parse_object': '#7294',
    'int_parse_radix_ok': '',
    'int_parse_radix_uint': '#7285',
    'int_parse_radix_string': '#7286',
    'int_parse_radix_bool': '#7285',
    'int_tryparse_string': '',
    'int_tryparse_int': '#7290',
    'int_tryparse_double': '#7323',
    # A CONTROL, not a #6812 shape: `int.TryParse`'s string parameter is INSIDE
    # the #6782 table (`{"Int32", "TryParse", 3, 0}` / `{"Int32", "TryParse", 2,
    # 0}` in `common::kBuiltinNonNullableParams`), and #6812 names the members
    # OUTSIDE it.  The cell exercises §7.4's initializer narrowing, which the
    # tracker does not name.
    'int_tryparse_nullable': '',
    'double_parse_string': '',
    'double_parse_int': '#7293',
    'double_tryparse_int': '#7293',
    'decimal_parse_string': '',
    'decimal_parse_double': '#7300',
    'decimal_tryparse_double': '#7300',
    'bool_parse_string': '',
    'bool_parse_int': '#7299',
    'bool_tryparse_int': '#7299',
    'char_parse_string': '',
    'char_parse_radix': '#7295',
    'char_tryparse_int': '#7299',
    'guid_tryparse_string': '',
    'guid_tryparse_int': '',
    'long_parse_string': '',
    'long_parse_intarg': '',
    # B2 ToString / format / Convert
    'int_tostring_radix': '',
    'int_tostring_radix_uint': '#7271',
    'int_tostring_radix_null': '#7296',
    'int_tostring_radix_null_named': '#7296',
    'int_tostring_radix_string': '#7286',
    'long_tostring_radix': '',
    'long_tostring_null': '#6825',
    'long_parse_null': '#6825',
    'long_tryparse_null': '#6825',
    'uint_parse_null': '#6825',
    'int_tostring_format_d': '',
    'int_tostring_format_x': '#7359',
    'int_tostring_format_X': '#7359',
    'bool_tostring_format': '#7333',
    'char_tostring_format': '#7336',
    'double_tostring_format_c': '#7303',
    'double_tostring_format_d': '#7362',
    'double_tostring_format_bad': '#7303',
    'string_tostring_format_bad': '',
    'decimal_tostring_radix': '#7284',
    'decimal_tostring_format': '',
    'convert_tostring_int': '',
    'convert_tostring_radix': '',
    'convert_tostring_radix_long': '#7239',
    'convert_tostring_char': '#7234',
    'convert_tostring_uint': '#7234',
    'convert_tostring_double': '',
    # B3 Math
    'math_max_uint': '#6864',
    'math_min_char': '#6865',
    'math_max_int': '',
    'math_abs_double': '',
    'math_abs_decimal': '',
    'math_abs_float': '#7061',
    'math_abs_decimal_nullable': '#6647',
    'math_abs_uint': '#7254',
    'math_round_decimal': '',
    'math_sqrt_uint': '#7242',
    'math_floor_double': '',
    'math_floor_nullable': '#7297',
    'math_floor_narrowed': '#7297',
    'math_sign_string': '#7227',
    'math_sign_bool': '#7227',
    'math_sign_double': '',
    'math_max_mixed': '#6884',
    'math_minmax_typed_result': '#6648',
    # B4 StringBuilder / string
    'sb_append_string': '',
    'sb_append_int': '',
    'sb_append_null': '#6812',
    'sb_replace_int': '#6809',
    'sb_insert_two_strings': '#6809',
    'sb_insert_zero': '#6809',
    'sb_remove_two_args': '#6809',
    'sb_replace_two_strings': '',
    'sb_appendformat_string': '#7244',
    'string_contains_int': '#6635',
    'string_contains_string': '',
    'string_contains_object': '#6826',
    'string_split_int': '#6636',
    'string_lastindexof_int': '#6635',
    'string_replace_object': '#6826',
    'string_repeat_char': '',
    'string_repeat_string': '#7129',
    'string_remove_oob': '#7108',
    'string_equals_named_reordered': '#7180',
    'string_equals_named_ordered': '#7180',
    'string_compareto_named_reordered': '#7180',
    'string_join_int_array': '',
    'string_isascii': '',
    'string_isascii_property': '#7245',
    'string_isascii_width': '#7245',
    'debug_writeline_stringbuilder': '#6780',
    # B5 nullable carrier (campaign 5)
    'sb_append_narrowed_int': '',
    'int_tostring_radix_narrowed': '',
    'int_parse_from_narrowed': '',
    'math_floor_nullable_null': '#7297',
    # B6 null at a parameter
    'sb_append_null_literal': '#6812',
    'string_equals_null': '',
    'datetime_tostring_null': '#6782',
    # receiver kinds (symbol vs name)
    'user_class_named_stringbuilder': '#7217',
    'object_holding_stringbuilder': '#6755',
    'encoding_getstring_int_array': '#6957',
    'encoding_getstring_byte_array': '',
    'builtin_typearg_list': '#7173',
    'builtin_typearg_arity': '#7173',
    # method-reference face
    'math_max_reference': '#6942',
    'math_sign_reference': '#6942',
    # cross-module face
    'imported_user_class_stringbuilder_qualified': '#7217',
    'imported_user_class_stringbuilder_bare': '',
}


def prog(extra, pre, body, imports=""):
    out = "module M;\n"
    if imports:
        out += imports.rstrip("\n") + "\n"
    if extra:
        out += extra.rstrip("\n") + "\n"
    out += "class C\n  public static method Main(): int\n"
    for l in (pre + "\n" + body).split("\n"):
        if l.strip():
            out += "    " + l.strip() + "\n"
    out += "    return 0;\n  end Main;\nend C;\n"
    return out


# ---------------------------------------------------------------- B1 parse

C("int_parse_string", 'Console.WriteLine(int.Parse("16"));', ("value", "16\n"))
C("int_parse_int", 'Console.WriteLine(int.Parse(16));',
  ("reject", R(F_overload("int.Parse"), F_convertible("int.Parse"))), subject="int.Parse")
C("int_parse_bool", 'Console.WriteLine(int.Parse(true));',
  ("reject", R(F_overload("int.Parse"), F_convertible("int.Parse"))), subject="int.Parse")
C("int_parse_char", "Console.WriteLine(int.Parse('a'));",
  ("reject", R(F_overload("int.Parse"), F_convertible("int.Parse"))), subject="int.Parse")
C("int_parse_double", 'Console.WriteLine(int.Parse(1.5));',
  ("reject", R(F_overload("int.Parse"), F_convertible("int.Parse"))), subject="int.Parse")
C("int_parse_object", 'Console.WriteLine(int.Parse(o));',
  ("reject", R(F_overload("int.Parse"), F_convertible("int.Parse"))),
  pre="var o: Object := 1;", subject="int.Parse")
C("int_parse_radix_ok", 'Console.WriteLine(int.Parse("ff", 16));', ("value", "255\n"))
# The radix is a `uint` CONSTANT, and the call is ADMITTED.
# Spec §5.1 (v0.345), "Constant narrowing": "The rule is **value-based, not
# type-based**: the source may be a compile-time constant of **any** integer type
# — not only `int` — and the target any integer type whose range contains its
# value, **across signedness** ... a `uint`-typed constant `7` is admissible
# where `int` is expected".  16u is a compile-time constant whose value fits an
# `int`, so the radix parameter's type is satisfied.  The #7285 fix landed
# deliberately admits it; the anchor lane's control row
# `uint_literal_radix_still_parses_hex` (tests/codegen/parse_radix_binding_7285_test.cpp)
# pins the same verdict.  This cell PINS that verdict.
C("int_parse_radix_uint", 'Console.WriteLine(int.Parse("ff", 16u));', ("value", "255\n"))
C("int_parse_radix_string", 'Console.WriteLine(int.Parse("ff", "16"));',
  ("reject", R(F_overload("int.Parse"), F_convertible("int.Parse"))), subject="int.Parse")
C("int_parse_radix_bool", 'Console.WriteLine(int.Parse("ff", true));',
  ("reject", R(F_overload("int.Parse"), F_convertible("int.Parse"))), subject="int.Parse")
C("int_tryparse_string",
  'Console.WriteLine(int.TryParse("16", out v));\nConsole.WriteLine(v);',
  ("value", "True\n16\n"), pre="var v: int := 0;")
C("int_tryparse_int", '_ := int.TryParse(16, out v);',
  ("reject", R(F_overload("int.TryParse"), F_convertible("int.TryParse"))),
  pre="var v: int := 0;", subject="int.TryParse")
C("int_tryparse_double", '_ := int.TryParse(1.5, out v);',
  ("reject", R(F_overload("int.TryParse"), F_convertible("int.TryParse"))),
  pre="var v: int := 0;", subject="int.TryParse")
# The argument is a `string?` LOCAL, and the call is ADMITTED.
# Spec §7.4 (v0.345) narrows a local "from a declaration whose initializer is
# such an expr — `var x: T? := expr;`", so `s` is narrowed non-null from its
# declaration onwards.  Spec §5.1 (v0.345) makes the word load-bearing in the
# other direction: "so an **un-narrowed** `Animal?` is rejected at an `Object`
# parameter" — the rejection belongs to the UN-NARROWED nullable, not to every
# `T?` binding.  A read of a binding §7.4 has narrowed non-null is of its
# non-nullable type (§22.6), so `s` reaches the `string` parameter.
# This is the anti-over-rejection control for the nullability position; it is
# the anchor lane's control row `int_tryparse_with_a_narrowed_local_is_admitted`
# (tests/sema/builtin_call_admission_7372_test.cpp) verbatim — same program,
# same expected stdout.
#
# NOTE: this cell is NOT a one-line expectation change.  A `value` cell is
# graded on the program's stdout, so the body must PRINT: the parse result is
# discarded and `v` is printed, exactly as the anchor's control row does.
C("int_tryparse_nullable", '_ := int.TryParse(s, out v);\nConsole.WriteLine(v);',
  ("value", "16\n"),
  pre="var v: int := 0;\nvar s: string? := \"16\";")
C("double_parse_string", 'Console.WriteLine(double.Parse("1.5"));', ("value", "1.5\n"))
C("double_parse_int", 'Console.WriteLine(double.Parse(1));',
  ("reject", R(F_overload("double.Parse"), F_convertible("double.Parse"))), subject="double.Parse")
C("double_tryparse_int", '_ := double.TryParse(1, out v);',
  ("reject", R(F_overload("double.TryParse"), F_convertible("double.TryParse"))),
  pre="var v: double := 0.0;", subject="double.TryParse")
C("decimal_parse_string", 'Console.WriteLine(decimal.Parse("1.5"));', ("value", "1.5\n"))
C("decimal_parse_double", 'Console.WriteLine(decimal.Parse(1.5));',
  ("reject", R(F_overload("decimal.Parse"), F_convertible("decimal.Parse"))), subject="decimal.Parse")
C("decimal_tryparse_double", '_ := decimal.TryParse(1.5, out v);',
  ("reject", R(F_overload("decimal.TryParse"), F_convertible("decimal.TryParse"))),
  pre="var v: decimal := 0m;", subject="decimal.TryParse")
C("bool_parse_string", 'Console.WriteLine(bool.Parse("true"));', ("value", "True\n"))
C("bool_parse_int", 'Console.WriteLine(bool.Parse(1));',
  ("reject", R(F_overload("bool.Parse"), F_convertible("bool.Parse"))), subject="bool.Parse")
C("bool_tryparse_int", '_ := bool.TryParse(1, out v);',
  ("reject", R(F_overload("bool.TryParse"), F_convertible("bool.TryParse"))),
  pre="var v: bool := false;", subject="bool.TryParse")
C("char_parse_string", "Console.WriteLine(char.Parse(\"a\"));", ("value", "a\n"))
C("char_parse_radix", "Console.WriteLine(char.Parse(\"a\", 16));",
  ("reject", R(F_overload("char.Parse"), F_convertible("char.Parse"))), subject="char.Parse")
C("char_tryparse_int", "_ := char.TryParse(1, out v);",
  ("reject", R(F_overload("char.TryParse"), F_convertible("char.TryParse"))),
  pre="var v: char := 'x';", subject="char.TryParse")
C("guid_tryparse_string",
  'Console.WriteLine(Guid.TryParse("00000000-0000-0000-0000-000000000000", out g));',
  ("value", "True\n"), pre="var g: Guid := Guid.Empty;")
C("guid_tryparse_int", '_ := Guid.TryParse(1, out g);',
  ("reject", R(F_overload("Guid.TryParse"), F_convertible("Guid.TryParse"))),
  pre="var g: Guid := Guid.Empty;", subject="Guid.TryParse")
C("long_parse_string", 'Console.WriteLine(long.Parse("16"));', ("value", "16\n"))
C("long_parse_intarg", 'Console.WriteLine(long.Parse(16));',
  ("reject", R(F_overload("long.Parse"), F_convertible("long.Parse"))), subject="long.Parse")

# ------------------------------------------------------- B2 ToString/format
C("int_tostring_radix", 'Console.WriteLine((255).ToString(16));', ("value", "ff\n"))
# The radix is a `uint` CONSTANT, and the call is ADMITTED.
# Spec §5.1 (v0.345), "Constant narrowing": "The rule is **value-based, not
# type-based**: the source may be a compile-time constant of **any** integer type
# — not only `int` — and the target any integer type whose range contains its
# value, **across signedness** ... a `uint`-typed constant `7` is admissible
# where `int` is expected".  16u is a compile-time constant whose value fits an
# `int`, so the radix parameter's type is satisfied, and the radix text "ff" is
# rendered.  This cell PINS that verdict (the #7271 fix made an unsigned
# CONSTANT radix render the radix form).
C("int_tostring_radix_uint", 'Console.WriteLine((255).ToString(16u));', ("value", "ff\n"))
C("int_tostring_radix_null", 'Console.WriteLine((255).ToString(null));',
  ("reject", R(F_overload("int.ToString"), F_nullable("int.ToString"))), subject="int.ToString")
# #7296 is FIXED: the NAMED-argument spelling of a primitive ToString radix is
# now nullability-checked like the positional one.  MEASURED on HEAD `d9f11d15`:
# `(255).ToString(radix := null)` is REJECTED ("the argument to `int.ToString` is
# nullable but the target type is non-nullable (§7)"), and `(255).ToString(radix :=
# 16)` prints "ff".  This cell PINS that fix.
C("int_tostring_radix_null_named", 'Console.WriteLine((255).ToString(radix := null));',
  ("reject", R(F_overload("int.ToString"), F_nullable("int.ToString"))), subject="int.ToString")
# stdlib §2.18: "16" is not in the integer vocabulary (D/Dn/X/Xn/B), so the call is
# ADMITTED and the format is unrecognized at RUN time -> FormatException.
C("int_tostring_radix_string", 'Console.WriteLine((255).ToString("16"));', ("fault", "FormatException"))
C("long_tostring_radix", 'Console.WriteLine((255L).ToString(16));', ("value", "ff\n"))
C("long_tostring_null", 'Console.WriteLine((255L).ToString(null));',
  ("reject", R(F_overload("long.ToString"), F_nullable("long.ToString"))), subject="long.ToString")
# #6825's LIVE half: the `Parse`/`TryParse` statics on the heads the #6782 type
# map missed (Long and the other integer kinds) still accept null.  MEASURED on
# HEAD `d9f11d15`: `long.Parse(null)`, `long.TryParse(null, out v)` and
# `uint.Parse(null)` all COMPILE (rc 0) — the defect.  The `ToString(null)` half
# of #6825 is FIXED (the cell `long_tostring_null` is rejected today).
C("long_parse_null", 'Console.WriteLine(long.Parse(null));',
  ("reject", R(F_overload("long.Parse"), F_nullable("long.Parse"))), subject="long.Parse")
C("long_tryparse_null", '_ := long.TryParse(null, out v);',
  ("reject", R(F_overload("long.TryParse"), F_nullable("long.TryParse"))),
  pre="var v: long := 0;", subject="long.TryParse")
C("uint_parse_null", 'Console.WriteLine(uint.Parse(null));',
  ("reject", R(F_overload("uint.Parse"), F_nullable("uint.Parse"))), subject="uint.Parse")
C("int_tostring_format_d", 'Console.WriteLine((255).ToString("D"));', ("value", "255\n"))
C("int_tostring_format_x", 'Console.WriteLine((255).ToString("x"));', ("value", "ff\n"))
C("int_tostring_format_X", 'Console.WriteLine((255).ToString("X"));', ("value", "FF\n"))
C("bool_tostring_format", 'Console.WriteLine((true).ToString("F4"));',
  ("reject", R(F_overload("bool.ToString"), F_formattable_receiver())),
  subject="bool.ToString")
C("char_tostring_format", "Console.WriteLine(('a').ToString(\"F4\"));",
  ("reject", R(F_overload("char.ToString"), F_formattable_receiver())),
  subject="char.ToString")
C("double_tostring_format_c", 'Console.WriteLine((1.5).ToString("C"));', ("fault", "FormatException"))
C("double_tostring_format_d", 'Console.WriteLine((3.5).ToString("D"));', ("fault", "FormatException"))
C("double_tostring_format_bad", 'Console.WriteLine((1.5).ToString("Q"));', ("fault", "FormatException"))
C("string_tostring_format_bad", 'Console.WriteLine("x".ToString("Q"));',
  ("reject", R(F_overload("String.ToString"), F_arity("String.ToString", "9.6", 1))),
  subject="String.ToString")
C("decimal_tostring_radix", 'Console.WriteLine((15m).ToString(16));',
  ("reject", R(F_overload("decimal.ToString"), F_convertible("decimal.ToString"))),
  subject="decimal.ToString")
C("decimal_tostring_format", 'Console.WriteLine((1.5m).ToString("F2"));', ("value", "1.50\n"))
C("convert_tostring_int", 'Console.WriteLine(Convert.ToString(255));', ("value", "255\n"))
C("convert_tostring_radix", 'Console.WriteLine(Convert.ToString(255, 16));', ("value", "ff\n"))
C("convert_tostring_radix_long", 'Console.WriteLine(Convert.ToString(255, lv));',
  ("reject", R(F_overload("Convert.ToString"), F_convertible("Convert.ToString"))),
  pre="var lv: long := 16;", subject="Convert.ToString")
C("convert_tostring_char", "Console.WriteLine(Convert.ToString('A'));",
  ("reject", R(F_ambiguous("Convert.ToString"))), subject="Convert.ToString")
C("convert_tostring_uint", 'Console.WriteLine(Convert.ToString(255U));',
  ("reject", R(F_ambiguous("Convert.ToString"))), subject="Convert.ToString")
C("convert_tostring_double", 'Console.WriteLine(Convert.ToString(1.5));', ("value", "1.5\n"))

# ------------------------------------------------------------- B3 Math
C("math_max_uint", 'Console.WriteLine(Math.Max(3u, 4000000000u));', ("value", "4000000000\n"))
C("math_min_char", "Console.WriteLine(Math.Min('a', 'b'));", ("value", "a\n"))
C("math_max_int", 'Console.WriteLine(Math.Max(1, 2));', ("value", "2\n"))
C("math_abs_double", 'Console.WriteLine(Math.Abs(-1.5));', ("value", "1.5\n"))
C("math_abs_decimal", 'Console.WriteLine(Math.Abs(-1.5m));', ("value", "1.5\n"))
# RULING R2 (coordinator, 2026-10-28): the STDLIB'S DECLARED SURFACE IS THE CONTRACT.
# stdlib §2.2 declares exactly FOUR `Math.Abs` overloads — Abs(int):int, Abs(long):long,
# Abs(double):double, Abs(decimal):decimal — and NO float and NO uint.  Spec §5.1's table
# gives `float` -> `double` and NOTHING else, and puts `decimal` <-> the binary floats on
# its NEVER-IMPLICIT list, so a `float` argument converts to exactly ONE of the four
# parameter types (`double`): the call is NOT ambiguous and NOT rejected — it RESOLVES to
# `Abs(double)` and ITS RESULT IS A `double`.  => `var f: float := Math.Abs(-1.5f);` must be
# REJECTED, because `double` -> `float` never narrows implicitly (§5.1).  MEASURED on HEAD
# `d9f11d15`: the compiler types the result `float` and ACCEPTS that assignment — that is
# #7061, and this cell pins it.
C("math_abs_float", 'var f: float := Math.Abs(-1.5f);',
  ("reject", R(F_initializer("Math.Abs", "f"), F_convertible("Math.Abs"))), subject="Math.Abs")
C("math_abs_decimal_nullable", 'Console.WriteLine(Math.Abs(dn));',
  ("reject", R(F_overload("Math.Abs"), F_nullable("Math.Abs"), F_convertible("Math.Abs"))),
  pre="var dn: decimal? := 1.5m;", subject="Math.Abs")
# RULING R2 (coordinator, 2026-10-28): the SAME ruling says `uint` converts to THREE of
# the four declared parameter types — `long`, `double` AND `decimal` — and to NONE of them
# exactly, so §22.6's ambiguity rule applies and the call IS a compile error.  **#7254 is
# NOT A DEFECT: the compiler's rejection is CORRECT.**  This cell therefore STANDS as a
# REJECT, and it is the guard against a lane "fixing" a correct rejection.
C("math_abs_uint", 'Console.WriteLine(Math.Abs(4000000000u));',
  ("reject", R(F_ambiguous("Math.Abs"))), subject="Math.Abs")
C("math_round_decimal", 'Console.WriteLine(Math.Round(1.5m));',
  ("reject", R(F_overload("Math.Round"), F_convertible("Math.Round"))), subject="Math.Round")
C("math_sqrt_uint", 'Console.WriteLine(Math.Sqrt(4000000000u));', ("value", "63245.553203367585\n"))
C("math_floor_double", 'Console.WriteLine(Math.Floor(2.5));', ("value", "2\n"))
C("math_floor_nullable", 'Console.WriteLine(Math.Floor(dn));',
  ("reject", R(F_overload("Math.Floor"), F_nullable("Math.Floor"), F_convertible("Math.Floor"))),
  pre="var dn: double? := 2.5;", subject="Math.Floor")
# A narrowed `double?` is LEGAL (§7.4) and §5.1 widens it to `double`; the call
# must therefore COMPILE AND RUN.  It cannot: the carrier reaches the Math arm.
# That is the UNWRAP (campaign 5's mechanism), not an admission gate's, so the
# cell is tagged campaign "5".
C("math_floor_narrowed",
  'if d <> null then Console.WriteLine(Math.Floor(d)); end if;',
  ("value", "2\n"), pre="var d: double? := 2.5;", campaign="5")
C("math_sign_string", 'Console.WriteLine(Math.Sign("x"));',
  ("reject", R(F_overload("Math.Sign"), F_convertible("Math.Sign"),
               F_typearg_constraint("Math.Sign"))), subject="Math.Sign")
C("math_sign_bool", 'Console.WriteLine(Math.Sign(true));',
  ("reject", R(F_overload("Math.Sign"), F_convertible("Math.Sign"),
               F_typearg_constraint("Math.Sign"))), subject="Math.Sign")
C("math_sign_double", 'Console.WriteLine(Math.Sign(-1.5));', ("value", "-1\n"))
C("math_max_mixed", 'Console.WriteLine(Math.Max(1, 2L));',
  ("reject", R(F_inference("Math.Max"))), subject="Math.Max")
C("math_minmax_typed_result", 'var r: string := Math.Max(1, 2);',
  ("reject", R(F_initializer("Math.Max", "r"), F_convertible("Math.Max"))), subject="Math.Max")

# --------------------------------------------------------- B4 StringBuilder / string
C("sb_append_string", '_ := sb.Append("a");\nConsole.WriteLine(sb.ToString());',
  ("value", "a\n"), pre="var sb := new StringBuilder();")
C("sb_append_int", '_ := sb.Append(1);\nConsole.WriteLine(sb.ToString());',
  ("value", "1\n"), pre="var sb := new StringBuilder();")
C("sb_append_null", '_ := sb.Append(null);',
  ("reject", R(F_overload("StringBuilder.Append"), F_nullable("StringBuilder.Append"))),
  pre="var sb := new StringBuilder();", subject="StringBuilder.Append")
C("sb_replace_int", '_ := sb.Replace(1, 2);',
  ("reject", R(F_overload("StringBuilder.Replace"), F_convertible("StringBuilder.Replace"))),
  pre="var sb := new StringBuilder();", subject="StringBuilder.Replace")
C("sb_insert_two_strings", '_ := sb.Insert("x", "y");',
  ("reject", R(F_overload("StringBuilder.Insert"), F_convertible("StringBuilder.Insert"))),
  pre="var sb := new StringBuilder();", subject="StringBuilder.Insert")
C("sb_insert_zero", '_ := sb.Insert(0);',
  ("reject", R(F_overload("StringBuilder.Insert"), F_arity("StringBuilder.Insert", "3.1", 1))),
  pre="var sb := new StringBuilder();", subject="StringBuilder.Insert")
C("sb_remove_two_args", '_ := sb.Remove("a", 1);',
  ("reject", R(F_overload("StringBuilder.Remove"), F_convertible("StringBuilder.Remove"))),
  pre="var sb := new StringBuilder();", subject="StringBuilder.Remove")
C("sb_replace_two_strings", '_ := sb.Replace("a", "b");\nConsole.WriteLine(sb.ToString());',
  ("value", "\n"), pre="var sb := new StringBuilder();")
C("sb_appendformat_string", '_ := sb.AppendFormat("x", a);', ("noabort", None),
  pre="var sb := new StringBuilder();\nusing var a := new Array<Object>();")
C("string_contains_int", 'Console.WriteLine("abc".Contains(1));',
  ("reject", R(F_overload("String.Contains"), F_convertible("String.Contains"),
               F_dispatch("String.Contains"),
               F_pos0_param("Contains", "search value"))), subject="String.Contains")
C("string_contains_string", 'Console.WriteLine("abc".Contains("b"));', ("value", "True\n"))
C("string_contains_object", 'Console.WriteLine("abc".Contains(o));',
  ("reject", R(F_overload("String.Contains"), F_convertible("String.Contains"),
               F_dispatch("String.Contains"),
               F_pos0_param("Contains", "search value"))),
  pre="var o: Object := \"b\";", subject="String.Contains")
C("string_split_int", 'Console.WriteLine("abc".Split(1));',
  ("reject", R(F_overload("String.Split"), F_convertible("String.Split"),
               F_dispatch("String.Split"),
               F_pos0_param("Split", "separator"))), subject="String.Split")
C("string_lastindexof_int", 'Console.WriteLine("abc".LastIndexOf(1));',
  ("reject", R(F_overload("String.LastIndexOf"), F_convertible("String.LastIndexOf"),
               F_dispatch("String.LastIndexOf"),
               F_pos0_param("LastIndexOf", "search value"))), subject="String.LastIndexOf")
C("string_replace_object", 'Console.WriteLine("abc".Replace(o, "x"));',
  ("reject", R(F_overload("String.Replace"), F_convertible("String.Replace"),
               F_dispatch("String.Replace"),
               F_pos0_param("Replace", "search value"))),
  pre="var o: Object := \"b\";", subject="String.Replace")
C("string_repeat_char", "Console.WriteLine(string.Repeat('a', 3));", ("value", "aaa\n"))
# RULING R3 (coordinator, 2026-10-28): stdlib §9.6 declares BOTH overloads side by side —
# `Repeat(c: char, count: int): string` and `Repeat(unit: string, count: int): string`
# (same count contract; Repeat("", n) = "") — and **the DECLARED OVERLOAD MUST BE
# IMPLEMENTED: the gate may NOT reject a call the stdlib declares.**  The compiler
# implements only the `char` overload and rejects the `string` one (#7129, #7277) — that is
# a DEFECT IN THE COMPILER, not a contract to be enforced.  => This cell expects the VALUE
# "ababab" and STANDS; #7129/#7277 need an implementation.
C("string_repeat_string", 'Console.WriteLine(string.Repeat("ab", 3));', ("value", "ababab\n"))
# stdlib §9.6: "a `startIndex` above `Length` ... raises IndexOutOfRangeException".
C("string_remove_oob", 'Console.WriteLine("ab".Remove(253).Length);', ("fault", "IndexOutOfRangeException"))
C("string_equals_named_reordered",
  'Console.WriteLine("ABCDEF".Equals(ignoreCase := true, other := "abcdef"));', ("value", "True\n"))
C("string_equals_named_ordered",
  'Console.WriteLine("ABCDEF".Equals(other := "abcdef", ignoreCase := true));', ("value", "True\n"))
C("string_compareto_named_reordered",
  'Console.WriteLine("ABCDEF".CompareTo(ignoreCase := true, other := "abcdef"));', ("value", "0\n"))
C("string_join_int_array", 'Console.WriteLine(string.Join(", ", a));',
  ("reject", R(F_overload("string.Join"), F_convertible("string.Join"),
               ["expects `Array<string>`", "`string.Join` expects `Array<string>`"])),
  pre="using var a := new Array<int>();", subject="string.Join")
# stdlib §9.6 declares `public pure readonly property IsAscii: bool` -- a PROPERTY.
# The call spelling has no candidate (§22.6); the property read is the legal one.
C("string_isascii", 'Console.WriteLine("abc".IsAscii());',
  ("reject", R(F_overload("String.IsAscii"), F_property("String.IsAscii"),
               F_dispatch("String.IsAscii"), F_no_such_member("string", "IsAscii"))),
  subject="String.IsAscii")
C("string_isascii_property", 'Console.WriteLine("abc".IsAscii);', ("value", "True\n"))
# #7245's own SHAPE, which the cell above does NOT reach: the property is
# defined by the CONTENT — stdlib §9.6 (v0.345): "public pure readonly property
# IsAscii: bool;  // true iff every code point < 128 (O(1))" — while
# `bz_string_is_ascii` answers false for any string whose STORAGE WIDTH is not
# 1.  A one-code-point slice of "a" + "日" holds only 'a', so every code point
# in it is < 128 and the property must answer TRUE.  MEASURED on HEAD
# `d9f11d15`: it prints "False".
C("string_isascii_width", 'Console.WriteLine(s.IsAscii);', ("value", "True\n"),
  pre='var s := ("a" + "日")[0..<1];')
C("debug_writeline_stringbuilder", '_ := sb.Append("x");\nDebug.WriteLine(sb);',
  ("reject", R(F_overload("Debug.WriteLine"), F_convertible("Debug.WriteLine"))),
  pre="var sb := new StringBuilder();", imports="import System.Diagnostics;",
  subject="Debug.WriteLine")

# ------------------------------------------------------- B5 nullable carrier (campaign 5)
C("sb_append_narrowed_int", 'if n <> null then _ := sb.Append(n); end if;',
  ("noabort", None), pre="var sb := new StringBuilder();\nvar n: int? := 1;", campaign="5")
C("int_tostring_radix_narrowed", 'if n <> null then Console.WriteLine(n.ToString(16)); end if;',
  ("value", "ff\n"), pre="var n: int? := 255;", campaign="5")
C("int_parse_from_narrowed", 'if n <> null then Console.WriteLine(int.Parse(n.ToString())); end if;',
  ("value", "1\n"), pre="var n: int? := 1;", campaign="5")
C("math_floor_nullable_null", 'Console.WriteLine(Math.Floor(dnull));',
  ("reject", R(F_overload("Math.Floor"), F_nullable("Math.Floor"), F_convertible("Math.Floor"))),
  pre="var dnull: double? := null;", subject="Math.Floor")

# ------------------------------------------------------- B6 null at a parameter
C("sb_append_null_literal", '_ := sb.Append(null);',
  ("reject", R(F_overload("StringBuilder.Append"), F_nullable("StringBuilder.Append"))),
  pre="var sb := new StringBuilder();", subject="StringBuilder.Append")
C("string_equals_null", 'Console.WriteLine("a".Equals(null));', ("value", "False\n"))
C("datetime_tostring_null", 'Console.WriteLine(DateTime.Now.ToString(null));',
  ("reject", R(F_overload("DateTime.ToString"), F_nullable("DateTime.ToString"))),
  subject="DateTime.ToString")

# --------------------------------------------- receiver kinds (symbol vs name)
C("user_class_named_stringbuilder",
  '_ := sb.Replace("l", "L");\nConsole.WriteLine("after");',
  ("value", "USER-REPLACE\nafter\n"), pre="var sb: StringBuilder := new StringBuilder();",
  extra=("class StringBuilder\n"
         "  public method Replace(a: string, b: string): StringBuilder\n"
         "    Console.WriteLine(\"USER-REPLACE\");\n"
         "    return this;\n"
         "  end Replace;\n"
         "end StringBuilder;"))
C("object_holding_stringbuilder", 'Console.WriteLine(o is StringBuilder);',
  ("value", "True\n"), pre="var sb := new StringBuilder();\nvar o: Object := sb;")
C("encoding_getstring_int_array", 'Console.WriteLine(Encoding.UTF8.GetString(a));',
  ("reject", R(["expects `Array<byte>`", "`Encoding.GetString` expects `Array<byte>`"],
               F_overload("Encoding.GetString"), F_convertible("Encoding.GetString"))),
  pre="using var a := new Array<int>();", subject="Encoding.GetString")
C("encoding_getstring_byte_array", 'a.Add(65);\nConsole.WriteLine(Encoding.UTF8.GetString(a));',
  ("value", "A\n"), pre="using var a := new Array<byte>();")
C("builtin_typearg_list", '_ := Console.WriteLine<int>("x");',
  ("reject", R(F_typearg("Console.WriteLine"), F_overload("Console.WriteLine"))),
  subject="Console.WriteLine")
C("builtin_typearg_arity", '_ := Console.WriteLine<int, int>("x");',
  ("reject", R(F_typearg("Console.WriteLine"), F_overload("Console.WriteLine"))),
  subject="Console.WriteLine")

# ------------------------------------------------------- method-reference face
C("math_max_reference", 'var g := Math.Max;\nConsole.WriteLine(g(1, 2));',
  ("reject", R(F_generic_ref("Math.Max"))), subject="Math.Max")
C("math_sign_reference", 'var g := Math.Sign;', ("reject", R(F_generic_ref("Math.Sign"))),
  subject="Math.Sign")

# ------------------------------------------------- cross-module face (N5)
# The imported-name resolution is a DISTINCT path from the same-module one
# (the user's own `class StringBuilder` shadows the built-in).  MEASURED on HEAD
# `d9f11d15`: the QUALIFIED spelling calls the user's own `Replace`
# ("USER-REPLACE", then "after", rc 0) and the BARE spelling is REJECTED by
# sema as ambiguous (§2.3, #5140).  Both cells pin that behaviour, so a
# regression in the imported path turns the grid RED.
LIB_SB = """module Lib;

public class StringBuilder
  public method Replace(a: string, b: string): StringBuilder
    Console.WriteLine("USER-REPLACE");
    return this;
  end Replace;
end StringBuilder;
"""
C("imported_user_class_stringbuilder_qualified",
  '_ := sb.Replace("l", "L");\nConsole.WriteLine("after");',
  ("value", "USER-REPLACE\nafter\n"),
  pre="var sb: Lib.StringBuilder := new Lib.StringBuilder();",
  imports="import Lib;", subject="Lib.StringBuilder", lib=LIB_SB)
C("imported_user_class_stringbuilder_bare",
  '_ := sb.Replace("l", "L");\nConsole.WriteLine("after");',
  ("reject", R(F_ambiguous_name("Lib.StringBuilder"))),
  pre="var sb: StringBuilder := new StringBuilder();",
  imports="import Lib;", subject="Lib.StringBuilder", lib=LIB_SB)

# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------

# The map and the table must agree: every cell has an entry, and every entry
# names a cell.  A cell added without a map entry is a silent gap.
_names = {c[0] for c in CELLS}
_missing = sorted(n for n in _names if n not in MEMBER)
_extra = sorted(k for k in MEMBER if k not in _names)
if _missing:
    raise ValueError('MEMBER has no entry for: ' + ', '.join(_missing))
if _extra:
    raise ValueError('MEMBER names unknown cells: ' + ', '.join(_extra))

DIAG = re.compile(r'^(?P<file>[^\n:]*?):(?P<line>\d+):(?P<col>\d+):\s*'
                   r'(?:(?P<sev>error|warning|note):\s*)?(?P<msg>.*)$', re.M)
INTERNAL = re.compile(r'\binternal:', re.I)
ABORT_PAT = re.compile(r'IR verification failed|codegen error|Assertion|Aborted|Segmentation fault|'
                       r'AddressSanitizer|SUMMARY: |stack dump|out of memory', re.I)
# A compiler abort is an ORACLE: the sanitizer's own report is one, and so is a
# signal (the exit status is negative when the compiler was killed by one).
SIGNAL_RC = -11

# --- the SANITIZER'S VERDICT at RUN time (B1) ---------------------------------
# The campaign's DONE leg runs the cell under ASan, so the grid MUST read the
# sanitizer's verdict: a program the sanitizer fails is not a pass.  These are
# the model's shapes (`scripts/regression_matrix.py::classify()`): a sanitizer
# error is a CRASH, a leak is a LEAK, and any other non-zero exit is a CRASH.
# The house oracle's run-time options are `ASAN_OPTIONS=detect_leaks=1:exitcode=99`.
ASAN_ERR_RE = re.compile(r'ERROR: AddressSanitizer: ([A-Za-z_-]+)')
GLIBC_RE = re.compile(r'free\(\): |double free|invalid pointer|corrupted|Segmentation')
LEAK_RE = re.compile(r'(\d+) byte\(s\) leaked in (\d+) allocation')
RUN_ASAN_OPTIONS = 'detect_leaks=1:exitcode=99'

CLASSES = ('OK', 'ACCEPTED', 'REJECTED', 'ABORT', 'INTERNAL', 'LEAK', 'CRASH')


def _needle_hit(msg, want):
    """True when the diagnostic contains one of the cell's needles.  `want` may
    be a single substring or a list of acceptable substrings (any one suffices).
    A reject cell's list is the set of diagnostic FAMILIES a correct fix may
    use, so a fix that lands on another wording of the same family still counts.
    """
    if want is None:
        return False
    needles = [want] if isinstance(want, str) else list(want)
    return any(n in msg for n in needles)


def _run_env():
    """The run-time environment for a cell's program: the house oracle's ASan
    options (`detect_leaks=1:exitcode=99`), so the sanitizer's verdict is
    VISIBLE to the classifier (B1)."""
    env = dict(os.environ)
    env['ASAN_OPTIONS'] = RUN_ASAN_OPTIONS
    return env


def classify_one(bezelc, progdir, outdir, cell, asan, timeout, run_timeout):
    name, extra, pre, body, expect, imports, campaign, subject, lib, member = cell
    kind, want = expect
    src = prog(extra, pre, body, imports)
    os.makedirs(progdir, exist_ok=True)
    os.makedirs(outdir, exist_ok=True)
    path = os.path.join(progdir, name + '.bzl')
    with open(path, 'w') as f:
        f.write(src)
    exe = os.path.join(outdir, name + '.exe')
    cmd = [bezelc]
    if lib:
        # The cross-module face: the cell's program IMPORTS a second module.
        # The library object is built WITHOUT the sanitizer (the compiler
        # rejects `--sanitize=address` with `-c`); the executable leg carries it.
        libpath = os.path.join(progdir, name + '_lib.bzl')
        with open(libpath, 'w') as f:
            f.write(lib)
        libobj = os.path.join(outdir, name + '_lib.o')
        librel = os.path.join('programs', name + '_lib.bzl')
        try:
            lc = subprocess.run([bezelc, '-c', libpath, '-o', libobj],
                                capture_output=True, text=True, errors='replace', timeout=timeout)
        except subprocess.TimeoutExpired:
            return (name, campaign, subject, member, kind, want, 'ABORT',
                    'library compile timeout', librel)
        if lc.returncode != 0:
            return (name, campaign, subject, member, kind, want, 'ABORT',
                    'library build failed: ' + (lc.stdout or lc.stderr)[-300:].replace(libpath, librel),
                    librel)
        cmd += ['--import', libobj]
    cmd += ['-o', exe]
    if asan:
        cmd.append('--sanitize=address')
    cmd.append(path)
    try:
        c = subprocess.run(cmd, capture_output=True, text=True, errors='replace', timeout=timeout)
    except subprocess.TimeoutExpired:
        return (name, campaign, subject, member, kind, want, 'ABORT', 'compile timeout', '')
    err = (c.stdout or '') + (c.stderr or '')
    diags = [d.groupdict() for d in DIAG.finditer(err)]
    errors = [d for d in diags if d['sev'] in (None, 'error')]
    warns = [d for d in diags if d['sev'] in ('warning', 'note')]
    internal = bool(INTERNAL.search(err))
    # The detail column carries a CELL-RELATIVE path, so two runs into different
    # --out directories produce byte-identical TSVs.
    rel = os.path.join('programs', name + '.bzl')
    if c.returncode != 0:
        if c.returncode == SIGNAL_RC or ABORT_PAT.search(err) or (not errors and internal):
            return (name, campaign, subject, member, kind, want, 'ABORT',
                    'compiler abort: ' + err[-300:].replace(path, rel), rel)
        if errors:
            first = f"{errors[0]['file']}:{errors[0]['line']}:{errors[0]['col']}: error: {errors[0]['msg']}"
            first = first.replace(path, rel)
            if kind == 'reject':
                if _needle_hit(first, want):
                    return (name, campaign, subject, member, kind, want, 'OK', first, rel)
                return (name, campaign, subject, member, kind, want, 'REJECTED', first, rel)
            return (name, campaign, subject, member, kind, want, 'REJECTED', first, rel)
        return (name, campaign, subject, member, kind, want, 'ABORT',
                err[-300:].replace(path, rel), rel)
    # the program compiled
    if internal:
        return (name, campaign, subject, member, kind, want, 'INTERNAL',
                '; '.join(w['msg'] for w in warns if 'internal:' in w['msg'])[:300], rel)
    try:
        r = subprocess.run([exe], capture_output=True, text=True, errors='replace',
                           timeout=run_timeout, env=_run_env())
    except subprocess.TimeoutExpired:
        return (name, campaign, subject, member, kind, want, 'ABORT', 'run timeout', rel)
    out = r.stdout or ''
    blob = out + (r.stderr or '')
    # THE SANITIZER'S VERDICT COMES FIRST (B1): a leak or a crash is never what a
    # cell expects, whatever the cell's expectation is.  The model's shape
    # (`scripts/regression_matrix.py::classify()`) is adopted here.
    m = ASAN_ERR_RE.search(blob)
    if m:
        return (name, campaign, subject, member, kind, want, 'CRASH',
                'AddressSanitizer: ' + m.group(1), rel)
    if GLIBC_RE.search(blob):
        return (name, campaign, subject, member, kind, want, 'CRASH', 'glibc runtime error', rel)
    if 'ERROR: LeakSanitizer' in blob:
        lm = LEAK_RE.search(blob) or re.search(r'Direct leak of (\d+) byte\(s\) in (\d+) object', blob)
        b, a = (lm.group(1), lm.group(2)) if lm else ('?', '?')
        return (name, campaign, subject, member, kind, want, 'LEAK', f'{b}B/{a} leaked', rel)
    if kind == 'fault':
        # The EXPECTED runtime fault: a non-zero exit is what a fault cell wants.
        if want in blob:
            return (name, campaign, subject, member, kind, want, 'OK', 'fault raised', rel)
        return (name, campaign, subject, member, kind, want, 'ACCEPTED',
                f'no {want!r} in output {blob[:200]!r}', rel)
    if r.returncode != 0:
        # The program ran and died.  That is a CRASH, not a pass and not a
        # "wrong answer": a cell whose expectation is a value or a noabort wants
        # the program to run to completion.
        return (name, campaign, subject, member, kind, want, 'CRASH',
                f'rc={r.returncode} {blob[:200]!r}', rel)
    if kind == 'value':
        if out == want:
            return (name, campaign, subject, member, kind, want, 'OK', repr(out), rel)
        return (name, campaign, subject, member, kind, want, 'ACCEPTED',
                f'printed {out!r}, want {want!r}', rel)
    if kind == 'noabort':
        # The program must run to COMPLETION: a runtime fault is a wrong answer,
        # not a pass.  (The name says what it checks: the compiler must not abort
        # AND the program must not fault.  A non-zero exit is caught above as a
        # CRASH.)
        return (name, campaign, subject, member, kind, want, 'OK', f'rc=0 {out!r}', rel)
    # kind == 'reject' but the program compiled: the wrong shape was admitted
    return (name, campaign, subject, member, kind, want, 'ACCEPTED',
            f'compiled and ran, printed {out!r}', rel)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--out', help='output directory (all scratch goes here)')
    ap.add_argument('--bezelc', default='build/bezelc')
    ap.add_argument('--jobs', type=int, default=4)
    ap.add_argument('--filter', default='', help='substring matched against the cell name')
    ap.add_argument('--campaign', default='', help='only the cells owned by this campaign ("2", "5")')
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--timeout', type=int, default=300)
    ap.add_argument('--run-timeout', type=int, default=60)
    a = ap.parse_args(argv)
    if a.jobs < 1:
        ap.error('--jobs must be at least 1')
    cells = [c for c in CELLS if a.filter in c[0] and (not a.campaign or c[6] == a.campaign)]
    if a.list:
        for c in cells:
            print(c[0], c[9] or '-', c[7])
        return 0
    if not cells:
        print('no cells selected', file=sys.stderr)
        return 2
    if not a.out:
        ap.error('--out is required')
    bezelc = os.path.abspath(a.bezelc)
    if not os.access(bezelc, os.X_OK):
        print(f'no compiler at {bezelc}', file=sys.stderr)
        return 2
    asan = os.environ.get('BEZEL_AOT_FORCE_SANITIZE', '') == 'address'
    progdir = os.path.join(a.out, 'programs')
    outdir = os.path.join(a.out, 'cells')
    os.makedirs(progdir, exist_ok=True)
    os.makedirs(outdir, exist_ok=True)
    rows = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as ex:
        futs = [ex.submit(classify_one, bezelc, progdir, outdir, c, asan, a.timeout, a.run_timeout)
                for c in cells]
        for f in futs:
            rows.append(f.result())
    rows.sort()
    tsv = os.path.join(a.out, 'grid.tsv')
    with open(tsv, 'w') as f:
        f.write('cell\tcampaign\tsubject\tmember\texpectation\twant\tclass\tdetail\tprogram\n')
        for r in rows:
            want = r[5]
            if isinstance(want, list):
                want = ' | '.join(want)
            f.write('\t'.join((x if x is not None else '').replace('\t', ' ').replace('\n', '\\n')
                              for x in (r[0], r[1], r[2], r[3], r[4], want, r[6], r[7], r[8])) + '\n')
    counts = {k: 0 for k in CLASSES}
    per_campaign = {}
    for r in rows:
        counts[r[6]] = counts.get(r[6], 0) + 1
        c = per_campaign.setdefault(r[1], {k: 0 for k in CLASSES})
        c[r[6]] += 1
    wrong = len(rows) - counts['OK']
    print(f'{len(rows)} cells, {wrong} wrong')
    for k in CLASSES:
        print(f'  {k:9s} {counts[k]}')
    for camp in sorted(per_campaign):
        c = per_campaign[camp]
        cw = sum(c.values()) - c['OK']
        print(f'  campaign {camp}: {sum(c.values())} cells, {cw} wrong')
    # The sanitizer skip counter is HONEST: a cell whose compile failed because the
    # ASan runtime was unavailable counts as skipped, not as a wrong cell.
    skip_pat = re.compile(r'cannot find -fsanitize|libasan|ASan runtime|unable to find.*sanitiz', re.I)
    skipped = sum(1 for r in rows if r[6] == 'ABORT' and skip_pat.search(r[7] or ''))
    print(f'NATIVE SANITIZER: {skipped} check(s) skipped')
    if wrong:
        print('\nWRONG CELLS:')
        for r in rows:
            if r[6] != 'OK':
                print(f'  {r[0]:40s} c{r[1]} {r[6]:9s} {r[7][:110]}')
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
