// Regression tests for #7226 — a `float` argument to a `double` PARAMETER of
// a `Math` built-in aborted the compiler inside LLVM.
//
// WHAT THE DOCUMENTS REQUIRE.
//
// spec §5.1 "Implicit conversions" (v0.344) lists `float` among the types
// that implicitly convert to `double`:
//
//   | `float` | `double` |
//
// stdlib §2.2 "Math" (v0.344) declares the `double` forms
// `Pow(x: double, y: double): double`, `Sqrt(x: double): double`,
// `Round(x: double, digits: int := 0): double`, `Floor`/`Ceiling`/
// `Truncate(x: double): double` and `Fmod`/`IEEERemainder(x: double,
// y: double): double`. §22.6's applicability stage (v0.344) reads the
// parameter types "under the implicit conversions (§5.1)", and a built-in
// member's parameter type IS declared — so a `float` argument reaches a
// `double` parameter and converts there. A legal program must compile.
//
// WHAT THE DEFECT WAS.
//
// The `double` lowerings converted such an operand with `sitofp`, which is
// ill-typed on an already-floating operand. The compiler ABORTED in the LLVM
// verifier, with no diagnostic and no source location:
//
//   Math.Pow(1.0f, 2.0f)
//     SIToFP source must be integer or integer vector
//     bezelc: codegen error: IR verification failed (malformed IR emitted)
//
// The same abort reached `Math.Sqrt(2.0f)`, `Math.Round(2.5f)`, the rest of
// the rounding family, `Math.Fmod` and `Math.IEEERemainder`, and every
// MIXED spelling (`Math.Pow(2.0f, 10.0)`).
//
// WHAT THE FIX DOES.
//
// The operand is WIDENED, not the call refused: §5.1 admits the conversion,
// so refusing a `float` argument would reject a legal program. The `double`
// lowering widens a floating operand with `fpext` (the same conversion the
// explicit-type-list path already applied) and keeps `sitofp` for an integer
// operand.
//
// THE CONTROL IN THE OTHER DIRECTION.
//
// §5.1 also lists, as NEVER implicit, "Between `decimal` and binary floats
// (`float`, `double`)". The `decimal` spellings therefore stay REFUSED —
// each row below pins that refusal, so a later change that widened
// `decimal` too would fail a row. The `floor`/`ceiling`/`truncate`/
// `abs` `decimal` FORMS are declared (stdlib §2.2 declares
// `Floor(x: decimal): decimal` and its siblings), so those stay working.
//
// THE PROOF HIERARCHY.
//
// Every ACCEPT row below FAILS pre-fix: the program aborted inside LLVM, so
// it never ran and produced no value. Each accepting row therefore pins the
// VALUE §5.1's widening implies — not merely "it compiled" — and the
// "SIToFP source must be integer or integer vector" abort is gone. The
// REFUSAL rows are green on both arms (the front-end check owns them) and
// are the control that shows this change is a widening and not a loosening.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::diagnosticMessageContains;

namespace {

std::string prog(const std::string& body) {
    return "module M; import System; "
           "class C "
           "  public static method Main(): int " +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

// The accepting shape: the program RAN, it produced exactly this text, and
// the pre-fix IR-verification abort is absent.
void expectValue(const std::string& body, const std::string& expected) {
    auto r = runAot(prog(body));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(
        r, "IR verification failed"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "compiler CRASHED"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(
        r, "SIToFP source must be integer or integer vector"));
    // The value channel: the program's stdout with ONE trailing line-ending
    // stripped (the same rule `runAotOut` applies), so the expectation is
    // platform-independent.
    std::string out = r.stdoutText;
    if (!out.empty() && out.back() == '\n') {
        out.pop_back();
        if (!out.empty() && out.back() == '\r') out.pop_back();
    }
    BEZEL_EXPECT_EQ(out, expected);
}

// The refusing shape: the program did not run, the driver rejected it at a
// REAL source location (never a crash / signal / timeout), the diagnostic
// carries the §5.1 wording, and the pre-fix abort is GONE.
void expectRefused(const std::string& body, const char* overload,
                   const char* expectedType) {
    auto r = runAot(prog(body));
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
// The issue's own three repros — each pins the VALUE §5.1's widening implies.
// ===========================================================================

// `Pow(x: double, y: double): double` (stdlib §2.2). The `float` operands
// convert to `double` at the call site (§5.1), so 1.0^2.0 is 1.
BEZEL_TEST(math_float_double_widen_7226, math_pow_float_arguments_widen_5_1) {
    expectValue("    Console.WriteLine(Math.Pow(1.0f, 2.0f));", "1");
}

// `Sqrt(x: double): double`. The `float` 2.0 widens to the `double` 2.0
// exactly, and the square root is taken in `double` — so the value is
// sqrt(2) at double precision, NOT the float-precision sqrt(2f).
BEZEL_TEST(math_float_double_widen_7226, math_sqrt_float_argument_widens_5_1) {
    expectValue("    Console.WriteLine(Math.Sqrt(2.0f));",
                "1.4142135623730951");
}

// `Round(x: double, digits: int := 0): double` — §2.2's banker's rounding:
// the midpoint 2.5 rounds to the nearest EVEN integer, 2.
BEZEL_TEST(math_float_double_widen_7226, math_round_float_argument_widens_5_1) {
    expectValue("    Console.WriteLine(Math.Round(2.5f));", "2");
}

// ===========================================================================
// The rest of the `double`-parameter family, at a `float` argument.
// ===========================================================================

// The rounding family at a `float` argument (stdlib §2.2's `double` forms).
BEZEL_TEST(math_float_double_widen_7226, math_rounding_family_float_widens_5_1) {
    expectValue("    Console.WriteLine(Math.Floor(2.5f)); "
                "    Console.WriteLine(Math.Ceiling(2.5f)); "
                "    Console.WriteLine(Math.Truncate(2.5f)); "
                "    Console.WriteLine(Math.Round(3.14159f, 2));",
                "2\n3\n2\n3.14");
}

// The two-argument `double` forms (stdlib §2.2's `Pow`, `Fmod` and
// `IEEERemainder`).
BEZEL_TEST(math_float_double_widen_7226, math_pow_fmod_float_widen_5_1) {
    expectValue("    Console.WriteLine(Math.Pow(2.0f, 10.0f)); "
                "    Console.WriteLine(Math.Fmod(10.0f, 3.0f)); "
                "    Console.WriteLine(Math.IEEERemainder(10.0f, 3.0f));",
                "1024\n1\n1");
}

// A MIXED spelling: one operand `float`, the other `double`. Both convert
// to the declared `double` parameter (§5.1), so the call is legal and the
// arithmetic runs in `double`.
BEZEL_TEST(math_float_double_widen_7226, math_mixed_float_double_widens_5_1) {
    expectValue("    Console.WriteLine(Math.Pow(2.0f, 10.0)); "
                "    Console.WriteLine(Math.Pow(2.0, 10.0f)); "
                "    Console.WriteLine(Math.Fmod(10.0f, 3.0));",
                "1024\n1024\n1");
}

// A NON-CONSTANT `float` operand — the conversion happens at run time, not
// at a constant fold.
BEZEL_TEST(math_float_double_widen_7226, math_float_variable_operand_widens_5_1) {
    expectValue("    var f: float := 4.0f; "
                "    Console.WriteLine(Math.Sqrt(f)); "
                "    var g: float := 2.0f; "
                "    Console.WriteLine(Math.Pow(g, 10.0f));",
                "2\n1024");
}

// The RESULT is a `double` — stdlib §2.2 declares `Sqrt(x: double): double`,
// and the `float` argument converted to `double` (§5.1). The value is
// therefore rendered by the `double` renderer: a `float` result would print
// "1.4142135" instead.
BEZEL_TEST(math_float_double_widen_7226, math_float_operand_result_is_double_5_1) {
    expectValue("    var d: double := Math.Sqrt(2.0f); "
                "    Console.WriteLine(d);",
                "1.4142135623730951");
}

// ===========================================================================
// THE CONTROL IN THE OTHER DIRECTION — the `decimal` spellings stay REFUSED.
// ===========================================================================

// §5.1: "Between `decimal` and binary floats (`float`, `double`)" is never
// implicit, and `Math.Pow` declares `x: double` (stdlib §2.2).
BEZEL_TEST(math_float_double_widen_7226, math_pow_decimal_stays_refused_5_1) {
    expectRefused("    Console.WriteLine(Math.Pow(1.0m, 2.0));",
                  "no matching overload of `Math.Pow`",
                  "the `x` argument must be a `double`");
}

BEZEL_TEST(math_float_double_widen_7226, math_sqrt_decimal_stays_refused_5_1) {
    expectRefused("    Console.WriteLine(Math.Sqrt(2.0m));",
                  "no matching overload of `Math.Sqrt`",
                  "the `x` argument must be a `double`");
}

BEZEL_TEST(math_float_double_widen_7226, math_round_decimal_stays_refused_5_1) {
    expectRefused("    Console.WriteLine(Math.Round(2.5m));",
                  "no matching overload of `Math.Round`",
                  "the `x` argument must be a `double`");
}

BEZEL_TEST(math_float_double_widen_7226, math_fmod_decimal_stays_refused_5_1) {
    expectRefused("    Console.WriteLine(Math.Fmod(10.0m, 3.0m));",
                  "no matching overload of `Math.Fmod`",
                  "the `x` argument must be a `double`");
}

BEZEL_TEST(math_float_double_widen_7226,
           math_ieeeremainder_decimal_stays_refused_5_1) {
    expectRefused("    Console.WriteLine(Math.IEEERemainder(10.0m, 3.0m));",
                  "no matching overload of `Math.IEEERemainder`",
                  "the `x` argument must be a `double`");
}

// ===========================================================================
// The CONTROLS — the conforming spellings are unaffected (green on both arms).
// ===========================================================================

// The `double` spellings themselves.
BEZEL_TEST(math_float_double_widen_7226, math_double_arguments_still_work_2_2) {
    expectValue("    Console.WriteLine(Math.Pow(2.0, 10.0)); "
                "    Console.WriteLine(Math.Sqrt(16.0)); "
                "    Console.WriteLine(Math.Round(3.14159, 2));",
                "1024\n4\n3.14");
}

// §5.1's widening table: an `int` and a `long` convert to a `double`
// parameter — the `sitofp` the fix keeps.
BEZEL_TEST(math_float_double_widen_7226, math_int_long_arguments_still_work_5_1) {
    expectValue("    Console.WriteLine(Math.Sqrt(16)); "
                "    Console.WriteLine(Math.Sqrt(16L)); "
                "    Console.WriteLine(Math.Pow(2, 10));",
                "4\n4\n1024");
}

// The EXPLICIT type list already widened a `float` operand (stdlib §2.2's
// `Min<T>`/`Max<T>`/`Clamp<T>` with `T = double`): the fix must not disturb
// that path.
BEZEL_TEST(math_float_double_widen_7226, math_explicit_type_list_float_5_1) {
    expectValue("    Console.WriteLine(Math.Max<double>(1.0f, 2.0)); "
                "    Console.WriteLine(Math.Min<double>(1.0f, 2.0));",
                "2\n1");
}

// The GENERIC family is untouched: `Min`/`Max`/`Clamp` declare a type
// parameter, so `float` is its own `T` (stdlib §2.2, #6849) and the
// intrinsic runs at the operand's own FP type.
BEZEL_TEST(math_float_double_widen_7226, math_generic_minmax_float_unchanged_2_2) {
    expectValue("    Console.WriteLine(Math.Min(1.0f, 2.0f)); "
                "    Console.WriteLine(Math.Max(1.0f, 2.0f)); "
                "    Console.WriteLine(Math.Clamp(1.5f, 0.0f, 2.0f));",
                "1\n2\n1.5");
}

// `Math.Abs` is untouched: stdlib §2.2 declares `Abs(x: double): double`,
// but the compiler types this call at the ARGUMENT's own type (a `float`
// argument yields a `float` result), and the `fabs` runs at the operand's
// own FP type. This row pins that the fix did not move it.
BEZEL_TEST(math_float_double_widen_7226, math_abs_float_unchanged_2_2) {
    expectValue("    Console.WriteLine(Math.Abs(-1.5f));", "1.5");
}
