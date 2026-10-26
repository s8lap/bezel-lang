// Spec-derived regression test for issue #7266 — stdlib §2.3's
// overload-ambiguity rule was enforced for `ulong` and NOT for `byte`,
// `ushort`, `sbyte` and `char`.
//
// stdlib §2.3 `Convert` (v0.344); spec §22.6 "Overloading" (v0.344);
// spec §5.1 "Implicit conversions" (v0.344); spec §5.2 (v0.344);
// spec §9.1 (v0.344).
//
// WHAT THE DEFECT WAS.
//
// §2.3 declares one `ToString` form per parameter type — `ToString(value:
// int)`, `ToString(value: long)`, `ToString(value: float)`,
// `ToString(value: double)`, `ToString(value: decimal)`,
// `ToString(value: bool)`, and the two-argument `ToString(value: int,
// toBase: int)` / `ToString(value: long, toBase: int)` — and states the
// rule for them verbatim:
//
//   "A `char` and the integer types other than `int` and `long` each
//    convert to two or more of these parameter types and match none
//    exactly, so the call is ambiguous and a compile error (§22.6); the
//    remedy is an explicit cast to one parameter type, or the value's own
//    `ToString()`, which renders it in its own type."
//
// §22.6's resolution has an APPLICABILITY step and a RANKING step. The
// compiler applied the first and not the second, so the verdict turned on
// WHICH TYPES the argument happened to have rather than on the rule:
//
//   byte   -> converts to `int` AND `long`, matches neither exactly
//             => two applicable candidates, equally ranked => AMBIGUOUS
//   ulong  -> converts to NEITHER `int` nor `long`
//             => no applicable candidate => "no matching overload"
//
// The same rule, the same call, opposite verdicts by argument type:
// `Convert.ToString(b, 10)` compiled and printed 200, while
// `Convert.ToString(u, 10)` was refused.
//
// WHAT THE FIX DOES.
//
// The probe that refuses the `ulong` argument now also RANKS the
// candidates it collects (§22.6's stage 4) and reports the tie (§22.6's
// ambiguity error). There is no list of types anywhere in the rule: the
// verdict follows from the declared forms and §5.1's table, so `byte`,
// `sbyte`, `short`, `ushort`, `char` and `uint` are judged by the same
// question, and the next type to reach the position will be too.
//
// The `ulong` rejection is NOT weakened: `ulong` converts to neither
// `int` nor `long`, so no candidate is applicable and the probe's own
// "no matching overload" diagnostic still fires, unchanged (the CONTROL
// row below pins its wording).
//
// THE PROOF HIERARCHY.
//
// Every rejection row below FAILS pre-fix: the program compiled, ran and
// printed the value, so there was no diagnostic at all. Each row
// therefore pins the refusal AND the diagnostic's wording AND the
// absence of the old crash/abort symptoms — a fix that merely changed
// the crash into a different crash would not pass.
//
// The acceptance rows are the CONTROLS: they are green on both arms, and
// they prove the fix did not over-reject. In particular the `uint` row
// is the one that keeps §22.6 honest: a `uint` converts to `long` but
// NOT to `int` (§5.1: "Between signed and unsigned types of the same or
// narrower width ... [is] never implicit"), so exactly ONE candidate is
// applicable and §22.6 selects it — no ambiguity, no refusal.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotOut;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::diagnosticMessageContains;

namespace {

std::string prog(const std::string& body) {
    return "module M; "
           "class C "
           "  public static method Main(): int " +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

// The §22.6 AMBIGUITY refusal: the program did not run, the driver
// rejected it at a REAL source location (never a crash / signal /
// timeout / ASan report), the diagnostic is the ambiguity one, and the
// §5.1 "no matching overload" wording is ABSENT — the ambiguity rule
// owns this call, so the applicability probe must add nothing.
void expectAmbiguous(const JitResult& r) {
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "ambiguous call to `Convert.ToString`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "no overload is unambiguously better (§22.6)"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "stdlib §2.3"));
    // The applicability probe must not double-report: `byte` DOES
    // convert to `int` and to `long`, so no candidate is inapplicable.
    BEZEL_EXPECT_FALSE(
        diagnosticMessageContains(r, "no matching overload of `Convert.ToString`"));
    // The pre-fix symptoms: the program compiled, ran and printed.
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "compiler CRASHED"));
}

}  // namespace

// ===========================================================================
// stdlib §2.3 — the two-argument form. `value` is declared `int` in one
// form and `long` in the other.
// ===========================================================================

// `byte` converts to BOTH `int` and `long` (§5.1's table) and matches
// neither exactly — the §2.3 rule's own shape.
BEZEL_TEST(convert_tostring_ambiguity_7266,
           convert_tostring_byte_value_is_ambiguous_2_3) {
    expectAmbiguous(runAot(prog(
        "    var b: byte := 200; "
        "    Console.WriteLine(Convert.ToString(b, 10));")));
}

BEZEL_TEST(convert_tostring_ambiguity_7266,
           convert_tostring_ushort_value_is_ambiguous_2_3) {
    expectAmbiguous(runAot(prog(
        "    var s: ushort := 40000; "
        "    Console.WriteLine(Convert.ToString(s, 10));")));
}

BEZEL_TEST(convert_tostring_ambiguity_7266,
           convert_tostring_sbyte_value_is_ambiguous_2_3) {
    expectAmbiguous(runAot(prog(
        "    var sb: sbyte := -5; "
        "    Console.WriteLine(Convert.ToString(sb, 10));")));
}

// The `char` row is its own: §2.3 names `char` first, and §5.1's table
// gives it `int`, `uint`, `long`, `ulong`, `float`, `double` and
// `decimal` — `int` and `long` among them.
BEZEL_TEST(convert_tostring_ambiguity_7266,
           convert_tostring_char_value_is_ambiguous_2_3) {
    expectAmbiguous(runAot(prog(
        "    var c: char := 'a'; "
        "    Console.WriteLine(Convert.ToString(c, 10));")));
}

// `short` is the sixth integer type of the same shape, and it is NOT in
// the issue's list: the rule is the declared forms and §5.1's table, not
// a list of types, so `short` is refused by the same question.
BEZEL_TEST(convert_tostring_ambiguity_7266,
           convert_tostring_short_value_is_ambiguous_2_3) {
    expectAmbiguous(runAot(prog(
        "    var sh: short := -300; "
        "    Console.WriteLine(Convert.ToString(sh, 10));")));
}

// ===========================================================================
// stdlib §2.3 — the ONE-argument form. The same rule over the same
// declared forms: `int`, `long`, `float`, `double`, `decimal` and `bool`.
// ===========================================================================

BEZEL_TEST(convert_tostring_ambiguity_7266,
           convert_tostring_one_arg_byte_value_is_ambiguous_2_3) {
    expectAmbiguous(runAot(prog(
        "    var b: byte := 200; "
        "    Console.WriteLine(Convert.ToString(b));")));
}

// A `uint` CONSTANT: §5.1's constant narrowing reaches the `int` form
// by VALUE ("an expression of integer type that is a compile-time
// constant implicitly converts to any integer type whose range contains
// its value") and §22.6 grades it "as any other conversion is, never as
// an exact match" — so it is a conversion, and the `int` and `long`
// candidates tie.
BEZEL_TEST(convert_tostring_ambiguity_7266,
           convert_tostring_one_arg_uint_constant_is_ambiguous_2_3) {
    expectAmbiguous(runAot(prog(
        "    Console.WriteLine(Convert.ToString(5U));")));
}

// ===========================================================================
// The NAMED-ARGUMENT spellings must behave like the positional one
// (§22.5 binds the argument to the parameter it NAMES).
// ===========================================================================

BEZEL_TEST(convert_tostring_ambiguity_7266,
           convert_tostring_named_byte_value_is_ambiguous_2_3) {
    expectAmbiguous(runAot(prog(
        "    var b: byte := 200; "
        "    Console.WriteLine(Convert.ToString(value := b, toBase := 10));")));
}

BEZEL_TEST(convert_tostring_ambiguity_7266,
           convert_tostring_named_one_arg_byte_value_is_ambiguous_2_3) {
    expectAmbiguous(runAot(prog(
        "    var b: byte := 200; "
        "    Console.WriteLine(Convert.ToString(value := b));")));
}

// ===========================================================================
// The CONTROLS — the conforming spellings are unaffected (green on both
// arms, and they prove the fix did not over-reject).
// ===========================================================================

// §9.1's radix digits, lower-case: "ff".
BEZEL_TEST(convert_tostring_ambiguity_7266,
           convert_tostring_int_value_renders_hex_2_3) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    Console.WriteLine(Convert.ToString(255, 16));")),
                    "ff");
}

// A `long` at the `long` form — the exact match, and the value above
// `Int32.MaxValue`.
BEZEL_TEST(convert_tostring_ambiguity_7266,
           convert_tostring_long_value_renders_2_3) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    Console.WriteLine(Convert.ToString(4000000000L, 10));")),
                    "4000000000");
}

// The one-argument forms: `int` and `long` are exact matches.
BEZEL_TEST(convert_tostring_ambiguity_7266,
           convert_tostring_one_arg_int_value_renders_2_3) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    Console.WriteLine(Convert.ToString(255));")),
                    "255");
}

BEZEL_TEST(convert_tostring_ambiguity_7266,
           convert_tostring_one_arg_long_value_renders_2_3) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    Console.WriteLine(Convert.ToString(255L));")),
                    "255");
}

// A `uint` VALUE: §5.1 never converts `uint` to `int` ("Between signed
// and unsigned types of the same or narrower width ... [is] never
// implicit"), so the `int` form is INAPPLICABLE and exactly one
// candidate is applicable — §22.6 selects the `long` form, and the call
// is neither ambiguous nor refused. (The constant `4000000000u` does not
// fit `int` either, so the narrowing does not reach the `int` form
// there either.)
BEZEL_TEST(convert_tostring_ambiguity_7266,
           convert_tostring_uint_value_selects_the_long_form_2_3) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var u: uint := 4000000000u; "
                        "    Console.WriteLine(Convert.ToString(u, 10));")),
                    "4000000000");
}

// The `ulong` CONTROL: the refusal the compiler already got right. Its
// wording is pinned here — the fix must not weaken it, and must not
// replace it with the ambiguity diagnostic either: `ulong` converts to
// NEITHER `int` nor `long`, so no candidate is applicable and §22.6's
// applicability step is the rule that fires.
BEZEL_TEST(convert_tostring_ambiguity_7266,
           convert_tostring_ulong_value_is_still_refused_2_3) {
    auto r = runAot(prog(
        "    var u: ulong := 5; "
        "    Console.WriteLine(Convert.ToString(u, 10));"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "no matching overload of `Convert.ToString`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the `value` argument must be an `int` or a `long`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§5.1"));
    BEZEL_EXPECT_FALSE(
        diagnosticMessageContains(r, "ambiguous call to `Convert.ToString`"));
}

// ===========================================================================
// The REMEDIES §2.3 names — "an explicit cast to one parameter type, or
// the value's own `ToString()`, which renders it in its own type". A fix
// that made the remedy unreachable would not be a fix.
// ===========================================================================

BEZEL_TEST(convert_tostring_ambiguity_7266,
           convert_tostring_explicit_cast_remedy_renders_2_3) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var b: byte := 200; "
                        "    Console.WriteLine(Convert.ToString(int(b), 10));")),
                    "200");
}

BEZEL_TEST(convert_tostring_ambiguity_7266,
           convert_tostring_own_tostring_remedy_renders_2_3) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var b: byte := 200; "
                        "    Console.WriteLine(b.ToString());")),
                    "200");
}

// The `ulong` remedies: the same two shapes, and the value's own
// `ToString()` renders it in its own type.
BEZEL_TEST(convert_tostring_ambiguity_7266,
           convert_tostring_ulong_explicit_cast_remedy_renders_2_3) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var u: ulong := 5; "
                        "    Console.WriteLine(Convert.ToString(long(u), 10));")),
                    "5");
}

BEZEL_TEST(convert_tostring_ambiguity_7266,
           convert_tostring_ulong_own_tostring_remedy_renders_2_3) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var u: ulong := 5; "
                        "    Console.WriteLine(u.ToString());")),
                    "5");
}
