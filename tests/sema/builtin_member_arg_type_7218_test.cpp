// Spec-derived regression test for issue #7218 — a `Math` / `Convert`
// built-in member's ARGUMENT was never type-checked against the member's
// own DECLARED parameter type.
//
// THE SPEC.
//
//   spec §5.1 "Implicit conversions" (v0.350): a conversion is applied
//   "where the target type is known"; §5.1's constant-narrowing paragraph
//   puts a CALL ARGUMENT among the positions the conversions apply at.
//   For a built-in member the target type IS known — stdlib §2.2 declares
//   `Math.Pow(x: double, y: double): double` and stdlib §2.3 declares the
//   `Convert` family — so the argument must be checked against it.
//
//   spec §22.6 "Overloading" (v0.350): a call that matches no declared form
//   is an ERROR, reported AT THE CALL.  §22.5's per-argument assignability
//   check is the mechanism for a member with a declaration.
//
// THE DEFECT.
//
// `Math` and `Convert` are compiler built-ins: they have no user
// declaration and their members have no MethodSymbol, so §22.5's per-
// argument assignability check never ran for them and `resolveMemberAccess`
// typed the member by NAME alone.  A wrongly-typed argument therefore
// reached LLVM unchecked, and the compiler emitted malformed IR and ABORTED
// inside the LLVM verifier — with NO diagnostic and NO source location:
//
//   Math.Pow("not a double", 3)
//     SIToFP source must be integer or integer vector
//     bezelc: codegen error: IR verification failed (malformed IR emitted)
//
// The same hole produced a SILENT WRONG ANSWER for a `bool` argument
// (`Math.Abs(true)` compiled and printed 1), because §5.1 lists "between
// integers and bool" as never implicit.
//
// WHAT IS PINNED HERE.
//
// Every refusal row pins THREE things at once: the refusal itself, the
// LOCATED §5.1 diagnostic naming the offending argument and the expected
// type, and the ABSENCE of the old IR-verification abort.  A fix that merely
// changed the crash into a different crash would not pass.
//
// THE ORACLE IS POSITIVE.  Each refusal row asserts the TEXT of the
// diagnostic the documents require ("no matching overload of `Math.Pow`",
// "the `x` argument must be a `double`", "§5.1").  No row asserts that some
// diagnostic is ABSENT, and the control rows assert the CONFORMING
// programs' VALUES — so nothing here can pass vacuously.
//
// The conforming spellings are the CONTROLS: they are unaffected by the
// check and must keep producing their values on both arms.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotOut;

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

// The §5.1 refusal, in the shape the documents require:
//
//   * the program did NOT run (it was rejected, not crashed into a wrong
//     answer);
//   * the rejection is a located diagnostic at a REAL source location — not
//     a compiler crash, not a signal, not a timeout;
//   * the diagnostic carries the "no matching overload" wording and names
//     the offending argument and its expected type;
//   * the old LLVM-verification abort is gone.
void expectRefused(const JitResult& r, const char* overload,
                   const char* expectedType) {
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, overload));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, expectedType));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§5.1"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "compiler CRASHED"));
}

}  // namespace

// ===========================================================================
// stdlib §2.2 `Math` — the issue's own repro, POSITIONAL spelling.
// ===========================================================================
BEZEL_TEST(builtin_member_arg_type_7218,
           math_pow_string_argument_is_refused_with_a_located_5_1_diagnostic) {
    auto r = runAot(prog(
        "    Console.WriteLine(Math.Pow(\"not a double\", 3));"));
    expectRefused(r, "no matching overload of `Math.Pow`",
                  "the `x` argument must be a `double`");
}

// ===========================================================================
// stdlib §2.2 `Math` — the SECOND parameter, so the diagnostic must name
// `y` and not `x` (the overload table is per-parameter).
// ===========================================================================
BEZEL_TEST(builtin_member_arg_type_7218,
           math_pow_second_argument_is_refused_naming_the_y_parameter) {
    auto r = runAot(prog("    Console.WriteLine(Math.Pow(2, \"x\"));"));
    expectRefused(r, "no matching overload of `Math.Pow`",
                  "the `y` argument must be a `double`");
}

// ===========================================================================
// stdlib §2.2 `Math` — `Sqrt(x: double): double`.
// ===========================================================================
BEZEL_TEST(builtin_member_arg_type_7218,
           math_sqrt_string_argument_is_refused_with_a_located_5_1_diagnostic) {
    auto r = runAot(prog("    Console.WriteLine(Math.Sqrt(\"x\"));"));
    expectRefused(r, "no matching overload of `Math.Sqrt`",
                  "the `x` argument must be a `double`");
}

// ===========================================================================
// stdlib §2.2 `Math` — the SILENT WRONG ANSWER: `Abs` declares `int`,
// `long`, `double` and `decimal` overloads and §5.1 lists "between
// integers and bool" as never implicit, so `Math.Abs(true)` compiled and
// printed 1.  It must be REFUSED, not answered.
// ===========================================================================
BEZEL_TEST(builtin_member_arg_type_7218,
           math_abs_bool_argument_is_refused_not_silently_answered) {
    auto r = runAot(prog("    Console.WriteLine(Math.Abs(true));"));
    expectRefused(r, "no matching overload of `Math.Abs`",
                  "the `x` argument must be an `int`");
}

// ===========================================================================
// stdlib §2.3 `Convert` — the issue names the `Convert` family too.
// `ToDouble(value: string): double`, so an `int` argument is refused.
// ===========================================================================
BEZEL_TEST(builtin_member_arg_type_7218,
           convert_to_double_int_argument_is_refused_with_a_located_5_1_diagnostic) {
    auto r = runAot(prog("    Console.WriteLine(Convert.ToDouble(7));"));
    expectRefused(r, "no matching overload of `Convert.ToDouble`",
                  "the `value` argument must be a `string`");
}

// ===========================================================================
// stdlib §2.3 `Convert` — `ToBoolean(value: string): bool`; a `bool`
// argument is not a `string` and §5.1 admits no `bool` -> `string`
// conversion, so this is refused too.
// ===========================================================================
BEZEL_TEST(builtin_member_arg_type_7218,
           convert_to_boolean_bool_argument_is_refused_with_a_located_5_1_diagnostic) {
    auto r = runAot(prog("    Console.WriteLine(Convert.ToBoolean(true));"));
    expectRefused(r, "no matching overload of `Convert.ToBoolean`",
                  "the `value` argument must be a `string`");
}

// ===========================================================================
// THE CONTROLS — the conforming spellings keep their VALUES.
//
// These are green on both arms: the check rejects non-convertible
// arguments, it does not disturb the convertible ones.
// ===========================================================================
BEZEL_TEST(builtin_member_arg_type_7218,
           math_pow_accepts_two_doubles_and_keeps_its_value) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog("    Console.WriteLine(Math.Pow(2.0, 3.0).ToString());")),
        std::string("8"));
}

BEZEL_TEST(builtin_member_arg_type_7218,
           convert_to_double_accepts_a_string_and_keeps_its_value) {
    BEZEL_EXPECT_EQ(
        runAotOut(
            prog("    Console.WriteLine(Convert.ToDouble(\"2.5\").ToString());")),
        std::string("2.5"));
}

BEZEL_TEST(builtin_member_arg_type_7218,
           convert_to_int32_accepts_a_string_and_keeps_its_value) {
    BEZEL_EXPECT_EQ(
        runAotOut(
            prog("    Console.WriteLine(Convert.ToInt32(\"42\").ToString());")),
        std::string("42"));
}
