// Spec-derived regression test for issue #7218 — a `Math` / `Convert`
// built-in's ARGUMENT was never type-checked.
//
// spec §5.1 "Implicit conversions" (v0.343);
// spec §22.6 "Overloading" (v0.343);
// spec §22.5 "Optional parameters and named arguments" (v0.343);
// stdlib §2.2 `Math`; stdlib §2.3 `Convert`.
//
// WHAT THE DEFECT WAS.
//
// `Math` and `Convert` are compiler built-ins: they have no user declaration
// and their members have no MethodSymbol, so §22.5's per-argument
// assignability check (`checkCallArity` → `checkArgsAgainstParams`) never ran
// for them, and `resolveMemberAccess` typed the member by NAME alone. An
// argument of a type the declared parameter does not take therefore reached
// codegen unchecked, and the compiler ABORTED inside the LLVM verifier — with
// no diagnostic and no source location:
//
//   Math.Pow("not a double", 3)
//     SIToFP source must be integer or integer vector
//     bezelc: codegen error: IR verification failed (malformed IR emitted)
//
// The same hole produced a SILENT WRONG ANSWER for a `bool` argument
// (`Math.Abs(true)` compiled and printed 1), because §5.1 lists "between
// integers and bool" as never implicit.
//
// WHAT THE FIX DOES.
//
// §5.1 (v0.343) applies the implicit conversions at every position whose
// target type is DECLARED, and a built-in member's parameter type IS
// declared — stdlib §2.2 declares `Math.Pow(x: double, y: double): double`
// and stdlib §2.3 declares the `Convert` family. §22.6's stage 3 (v0.343)
// is that applicability test ("Applicability over the (substituted)
// signatures: parameter types under the implicit conversions (§5.1)"), and
// §5.1's constant-narrowing paragraph (v0.343) puts a call argument among
// the positions the conversions apply at ("Constant narrowing applies at
// every position where this section's implicit conversions apply — there is
// no position carve-out: … call arguments (`Set(3)`) …"). The argument is
// therefore checked against the declared parameter type, and a call that
// matches NO declared form is an error (§22.6), reported at the CALL SITE
// with a located §5.1 diagnostic naming the offending argument and the
// expected type.
//
// NOTE ON THE PROOF HIERARCHY.
//
// Every rejection row below FAILS pre-fix: the program aborted inside LLVM
// (or, for the `bool` rows, compiled and ran), so there was no §5.1
// diagnostic, no source location, and no "no matching overload" text. Each
// row therefore pins THREE things at once — the refusal, the diagnostic's
// wording, and the ABSENCE of the old IR-verification abort — so a fix that
// merely changed the crash into a different crash would not pass.
//
// The acceptance rows are the controls: they must stay green on both arms
// (the conforming spellings are unaffected by the check). The `long`
// constant rows are the fix-forward's regression pins: they are green on the
// BASE arm and were RED on the first #7218 commit, whose probe re-typed the
// argument (see the comment above each row).

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

// The §5.1 refusal: the program did not run, the driver rejected it at a
// REAL source location (never a crash / signal / timeout / ASan report), the
// diagnostic carries the "no matching overload" wording with the expected
// type, and the old IR-verification abort is GONE.
void expectRefused(const JitResult& r, const char* overload,
                   const char* expectedType) {
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, overload));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, expectedType));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§5.1"));
    // The pre-fix symptom: the compiler aborted inside the LLVM verifier.
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "compiler CRASHED"));
}

}  // namespace

// ===========================================================================
// stdlib §2.2 `Math` — a `double` parameter rejects a `string`.
// ===========================================================================

// The issue's own repro (positional spelling).
BEZEL_TEST(math_convert_arg_type_7218,
           math_pow_string_argument_is_refused_5_1) {
    auto r = runAot(prog(
        "    Console.WriteLine(Math.Pow(\"not a double\", 3));"));
    expectRefused(r, "no matching overload of `Math.Pow`",
                  "the `x` argument must be a `double`");
}

// The issue's own repro (named spelling) — §22.5 binds `y :=` to the
// parameter `y`, so the offending argument is the SECOND one.
BEZEL_TEST(math_convert_arg_type_7218,
           math_pow_named_wrongly_typed_argument_is_refused_5_1) {
    auto r = runAot(prog(
        "    Console.WriteLine(Math.Pow(y := \"not a double\", x := 3));"));
    expectRefused(r, "no matching overload of `Math.Pow`",
                  "the `y` argument must be a `double`");
}

// §5.1: "Between `decimal` and binary floats (`float`, `double`)" is never
// implicit — `Math.Pow` declares `x: double`.
BEZEL_TEST(math_convert_arg_type_7218,
           math_pow_decimal_argument_is_refused_5_1) {
    auto r = runAot(prog("    Console.WriteLine(Math.Pow(1.0m, 2.0));"));
    expectRefused(r, "no matching overload of `Math.Pow`",
                  "the `x` argument must be a `double`");
}

// §5.1: "Between integers and `bool`" is never implicit.
BEZEL_TEST(math_convert_arg_type_7218,
           math_pow_bool_argument_is_refused_5_1) {
    auto r = runAot(prog("    Console.WriteLine(Math.Pow(true, 3));"));
    expectRefused(r, "no matching overload of `Math.Pow`",
                  "the `x` argument must be a `double`");
}

// `Math.Sqrt(x: double): double` — the same §5.1 hole.
BEZEL_TEST(math_convert_arg_type_7218,
           math_sqrt_string_argument_is_refused_5_1) {
    auto r = runAot(prog("    Console.WriteLine(Math.Sqrt(\"x\"));"));
    expectRefused(r, "no matching overload of `Math.Sqrt`",
                  "the `x` argument must be a `double`");
}

// §5.1: decimal ↔ binary float is never implicit.
BEZEL_TEST(math_convert_arg_type_7218,
           math_sqrt_decimal_argument_is_refused_5_1) {
    auto r = runAot(prog("    Console.WriteLine(Math.Sqrt(2.0m));"));
    expectRefused(r, "no matching overload of `Math.Sqrt`",
                  "the `x` argument must be a `double`");
}

// `Math.Abs` declares four overloads (int / long / double / decimal), so the
// diagnostic names all four — §22.6: a call that matches no declared form.
BEZEL_TEST(math_convert_arg_type_7218,
           math_abs_string_argument_is_refused_5_1) {
    auto r = runAot(prog("    Console.WriteLine(Math.Abs(\"x\"));"));
    expectRefused(r, "no matching overload of `Math.Abs`",
                  "the `x` argument must be an `int`, a `long`, a `double` or "
                  "a `decimal`");
}

// The `bool` argument that used to compile and print 1 — a SILENT WRONG
// ANSWER, not a crash.
BEZEL_TEST(math_convert_arg_type_7218,
           math_abs_bool_argument_is_refused_5_1) {
    auto r = runAot(prog("    Console.WriteLine(Math.Abs(true));"));
    expectRefused(r, "no matching overload of `Math.Abs`",
                  "the `x` argument must be an `int`, a `long`, a `double` or "
                  "a `decimal`");
}

// `Math.Round(x: double, digits: int := 0): double` — the FIRST parameter.
BEZEL_TEST(math_convert_arg_type_7218,
           math_round_string_argument_is_refused_5_1) {
    auto r = runAot(prog("    Console.WriteLine(Math.Round(\"x\"));"));
    expectRefused(r, "no matching overload of `Math.Round`",
                  "the `x` argument must be a `double`");
}

// ... and the SECOND parameter (`digits: int`).
BEZEL_TEST(math_convert_arg_type_7218,
           math_round_digits_string_argument_is_refused_5_1) {
    auto r = runAot(prog("    Console.WriteLine(Math.Round(2.5, \"x\"));"));
    expectRefused(r, "no matching overload of `Math.Round`",
                  "the `digits` argument must be an `int`");
}

// §5.1: decimal ↔ binary float is never implicit — `Math.Round` declares
// `x: double` (the decimal rounding is `Decimal.Round`, stdlib §9.3).
BEZEL_TEST(math_convert_arg_type_7218,
           math_round_decimal_argument_is_refused_5_1) {
    auto r = runAot(prog("    Console.WriteLine(Math.Round(2.5m));"));
    expectRefused(r, "no matching overload of `Math.Round`",
                  "the `x` argument must be a `double`");
}

// The rounding family: `Floor` / `Ceiling` / `Truncate` declare a `double`
// and a `decimal` form, so a `string` matches neither.
BEZEL_TEST(math_convert_arg_type_7218,
           math_floor_string_argument_is_refused_5_1) {
    auto r = runAot(prog("    Console.WriteLine(Math.Floor(\"x\"));"));
    expectRefused(r, "no matching overload of `Math.Floor`",
                  "the `x` argument must be a `double` or a `decimal`");
}

BEZEL_TEST(math_convert_arg_type_7218,
           math_ceiling_string_argument_is_refused_5_1) {
    auto r = runAot(prog("    Console.WriteLine(Math.Ceiling(\"x\"));"));
    expectRefused(r, "no matching overload of `Math.Ceiling`",
                  "the `x` argument must be a `double` or a `decimal`");
}

BEZEL_TEST(math_convert_arg_type_7218,
           math_truncate_string_argument_is_refused_5_1) {
    auto r = runAot(prog("    Console.WriteLine(Math.Truncate(\"x\"));"));
    expectRefused(r, "no matching overload of `Math.Truncate`",
                  "the `x` argument must be a `double` or a `decimal`");
}

// `Math.Fmod(x: double, y: double): double`.
BEZEL_TEST(math_convert_arg_type_7218,
           math_fmod_string_argument_is_refused_5_1) {
    auto r = runAot(prog("    Console.WriteLine(Math.Fmod(\"x\", 2.0));"));
    expectRefused(r, "no matching overload of `Math.Fmod`",
                  "the `x` argument must be a `double`");
}

// `Math.IEEERemainder(x: double, y: double): double`.
BEZEL_TEST(math_convert_arg_type_7218,
           math_ieeeremainder_string_argument_is_refused_5_1) {
    auto r = runAot(prog(
        "    Console.WriteLine(Math.IEEERemainder(\"x\", 2.0));"));
    expectRefused(r, "no matching overload of `Math.IEEERemainder`",
                  "the `x` argument must be a `double`");
}

// ===========================================================================
// stdlib §2.3 `Convert` — a `string` parameter rejects a non-`string`.
// ===========================================================================

// stdlib §2.3 declares `ToInt32(value: string): int` — an `int` argument
// converts to no declared parameter type, so §22.6 admits no candidate.
BEZEL_TEST(math_convert_arg_type_7218,
           convert_toint32_non_string_argument_is_refused_5_1) {
    auto r = runAot(prog("    Console.WriteLine(Convert.ToInt32(7));"));
    expectRefused(r, "no matching overload of `Convert.ToInt32`",
                  "the `value` argument must be a `string`");
}

BEZEL_TEST(math_convert_arg_type_7218,
           convert_todouble_non_string_argument_is_refused_5_1) {
    auto r = runAot(prog("    Console.WriteLine(Convert.ToDouble(7));"));
    expectRefused(r, "no matching overload of `Convert.ToDouble`",
                  "the `value` argument must be a `string`");
}

BEZEL_TEST(math_convert_arg_type_7218,
           convert_toboolean_non_string_argument_is_refused_5_1) {
    auto r = runAot(prog("    Console.WriteLine(Convert.ToBoolean(true));"));
    expectRefused(r, "no matching overload of `Convert.ToBoolean`",
                  "the `value` argument must be a `string`");
}

// The `Try*` form declares `value: string` too — the `out` slot is not the
// question here.
BEZEL_TEST(math_convert_arg_type_7218,
           convert_trytoint32_non_string_argument_is_refused_5_1) {
    auto r = runAot(prog(
        "    var ok := Convert.TryToInt32(5, out var r); "
        "    Console.WriteLine(ok);"));
    expectRefused(r, "no matching overload of `Convert.TryToInt32`",
                  "the `value` argument must be a `string`");
}

// `Convert.ToString` declares one overload per numeric primitive and one for
// `bool`; a `string` matches none of them, and §5.1 has no conversion TO a
// `string` parameter.
BEZEL_TEST(math_convert_arg_type_7218,
           convert_tostring_string_argument_is_refused_5_1) {
    auto r = runAot(prog("    Console.WriteLine(Convert.ToString(\"abc\"));"));
    expectRefused(r, "no matching overload of `Convert.ToString`",
                  "the `value` argument must be an `int`, a `long`, a `float`, "
                  "a `double`, a `decimal` or a `bool`");
}

// ===========================================================================
// The refusals that belong to OTHER rules must stay theirs — the §5.1 check
// must not double-report them.
// ===========================================================================

// §7 owns the NULLABILITY of an argument: `Convert.ToInt32(null)` is
// refused as "nullable but the target type is non-nullable", not as a §5.1
// type mismatch (stdlib §2.3 declares `value: string`, and §5.1's nullable
// lifting is §7's business, not this check's).
BEZEL_TEST(math_convert_arg_type_7218,
           convert_toint32_null_argument_is_refused_7_not_5_1) {
    auto r = runAot(prog("    Console.WriteLine(Convert.ToInt32(null));"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the argument to `Convert.ToInt32` is nullable but the target type "
           "is non-nullable (§7)"));
    BEZEL_EXPECT_FALSE(
        diagnosticMessageContains(r, "no matching overload of `Convert.ToInt32`"));
}

// §22.6's AMBIGUITY rule owns `Math.Abs('a')` (a `char` converts to several
// of the four declared overloads and matches none exactly): the ambiguity
// diagnostic is the one reported, and the §5.1 check adds nothing.
BEZEL_TEST(math_convert_arg_type_7218,
           math_abs_char_argument_is_refused_22_6_not_5_1) {
    auto r = runAot(prog("    Console.WriteLine(Math.Abs('a'));"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "ambiguous call to `Math.Abs`: no overload is unambiguously better"));
    BEZEL_EXPECT_FALSE(
        diagnosticMessageContains(r, "no matching overload of `Math.Abs`"));
}

// ===========================================================================
// The CONTROLS — the conforming spellings are unaffected (they are green on
// both arms).
// ===========================================================================

// A `long` CONSTANT at `Math.Abs` must keep the `Abs(x: long): long` form
// (stdlib §2.2, v0.343). §22.6's stage 4 (v0.343) ranks the EXACT match
// above a candidate §5.1's constant narrowing reaches, and §5.1 (v0.343)
// grades a narrowed candidate "as any other conversion is, never as an exact
// match" — so a probe that re-typed the `long` constant to `int` before the
// identity match was tried would select `Abs(int)`, and
// `Abs(int.MinValue)` raises OverflowException on the `int` result.
//
// The first two rows below are the fix-forward's regression pins (the
// fix-forward of #7218: the probe mutated the argument's recorded type).
// They are green on the BASE arm (the base picks the `long` form by the
// argument's own type) and were RED on the first #7218 commit.
BEZEL_TEST(math_convert_arg_type_7218,
           math_abs_long_constant_keeps_the_long_overload_2_2) {
    BEZEL_EXPECT_EQ(runAotOut(prog("    Console.WriteLine(Math.Abs(-2147483648L));")),
                    "2147483648");
}

BEZEL_TEST(math_convert_arg_type_7218,
           math_abs_long_constant_plus_int_keeps_the_long_overload_2_2) {
    BEZEL_EXPECT_EQ(runAotOut(prog("    Console.WriteLine(Math.Abs(2000000000L) + 2000000000);")),
                    "4000000000");
}

// The NON-constant control: the narrowing never fires for a run-time value,
// so this was never at risk — it is the row that shows the fix did not
// change the run-time path.
BEZEL_TEST(math_convert_arg_type_7218,
           math_abs_long_variable_keeps_the_long_overload_2_2) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var x: long := 2000000000L; "
                        "    Console.WriteLine(Math.Abs(x) + 2000000000);")),
                    "4000000000");
}

// §5.1's CONSTANT NARROWING still reaches an `int` parameter: a `long`
// compile-time constant whose value fits `int` converts to it ("an
// expression of integer type that is a compile-time constant implicitly
// converts to any integer type whose range contains its value", §5.1,
// v0.343). The narrowing is APPLICABILITY only and is never an exact match.
BEZEL_TEST(math_convert_arg_type_7218,
           math_round_long_constant_digits_narrows_5_1) {
    BEZEL_EXPECT_EQ(runAotOut(prog("    Console.WriteLine(Math.Round(3.14159, 2L));")),
                    "3.14");
}

// The mirror of that row: a RUN-TIME `long` at an `int` parameter is NOT a
// constant, so §5.1's narrowing does not reach it ("The rule applies to
// constants only; a run-time `int` value still requires the explicit
// `byte(x)` narrowing of §5.2", §5.1, v0.343) and §5.1's table has no
// `long -> int` row — so no declared form of `Math.Round` applies and
// §22.6 (v0.343) refuses the call.
BEZEL_TEST(math_convert_arg_type_7218,
           math_round_long_variable_digits_is_refused_5_1) {
    auto r = runAot(prog(
        "    var d: long := 2; "
        "    Console.WriteLine(Math.Round(3.14159, d));"));
    expectRefused(r, "no matching overload of `Math.Round`",
                  "the `digits` argument must be an `int`");
}

// `Convert.ToString` declares a `long` overload (stdlib §2.3, v0.343): a
// `long` constant must select it, not the `int` one.
BEZEL_TEST(math_convert_arg_type_7218,
           convert_tostring_long_constant_keeps_the_long_overload_2_3) {
    BEZEL_EXPECT_EQ(runAotOut(prog("    Console.WriteLine(Convert.ToString(255L));")),
                    "255");
}

BEZEL_TEST(math_convert_arg_type_7218, math_pow_double_arguments_still_work_2_2) {
    BEZEL_EXPECT_EQ(runAotOut(prog("    Console.WriteLine(Math.Pow(2.0, 10.0));")),
                    "1024");
}

BEZEL_TEST(math_convert_arg_type_7218, math_sqrt_double_argument_still_works_2_2) {
    BEZEL_EXPECT_EQ(runAotOut(prog("    Console.WriteLine(Math.Sqrt(16.0));")),
                    "4");
}

// §5.1's widening table: an `int` converts to a `double` parameter.
BEZEL_TEST(math_convert_arg_type_7218, math_sqrt_int_argument_still_works_2_2) {
    BEZEL_EXPECT_EQ(runAotOut(prog("    Console.WriteLine(Math.Sqrt(16));")),
                    "4");
}

BEZEL_TEST(math_convert_arg_type_7218, math_abs_int_argument_still_works_2_2) {
    BEZEL_EXPECT_EQ(runAotOut(prog("    Console.WriteLine(Math.Abs(0 - 5));")),
                    "5");
}

BEZEL_TEST(math_convert_arg_type_7218,
           math_round_double_and_digits_arguments_still_work_2_2) {
    BEZEL_EXPECT_EQ(runAotOut(prog("    Console.WriteLine(Math.Round(3.14159, 2));")),
                    "3.14");
}

// The `decimal` form of the rounding family is DECLARED (stdlib §2.2
// declares `Floor(x: decimal): decimal`), so it is admitted.
BEZEL_TEST(math_convert_arg_type_7218,
           math_floor_decimal_argument_still_works_2_2) {
    BEZEL_EXPECT_EQ(runAotOut(prog("    Console.WriteLine(Math.Floor(2.5m));")),
                    "2");
}

// `Math.Min` / `Max` / `Clamp` declare a TYPE PARAMETER (`where T:
// IComparable<T>`), so §22.6's stage-3 applicability has no concrete
// parameter type to apply §5.1's table to there: the constraint governs,
// and the check must not claim the call.
BEZEL_TEST(math_convert_arg_type_7218, math_min_generic_argument_still_works_2_2) {
    BEZEL_EXPECT_EQ(runAotOut(prog("    Console.WriteLine(Math.Min(3, 8));")),
                    "3");
}

BEZEL_TEST(math_convert_arg_type_7218,
           convert_toint32_string_argument_still_works_2_3) {
    BEZEL_EXPECT_EQ(runAotOut(prog("    Console.WriteLine(Convert.ToInt32(\"42\"));")),
                    "42");
}

BEZEL_TEST(math_convert_arg_type_7218,
           convert_trytoint32_string_argument_still_works_2_3) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var ok := Convert.TryToInt32(\"42\", out var r); "
                        "    Console.WriteLine(r);")),
                    "42");
}

BEZEL_TEST(math_convert_arg_type_7218,
           convert_tostring_int_argument_still_works_2_3) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    Console.WriteLine(Convert.ToString(value := 255, "
                        "toBase := 16));")),
                    "ff");
}
