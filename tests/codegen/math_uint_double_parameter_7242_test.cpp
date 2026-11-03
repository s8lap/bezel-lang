// Spec-derived regression test for issue #7242 — an UNSIGNED operand at a
// `Math` built-in's `double` parameter was SIGN-EXTENDED, so the VALUE did
// not survive the conversion.
//
// THE SPEC.
//
//   spec §5.1 "Implicit conversions" (v0.350) lists:
//
//     | `uint` | `long`, `ulong`, `float`, `double`, `decimal` |
//
//   So §5.1 ADMITS `uint -> double`, and a conversion converts VALUES, not
//   bit patterns.  The `uint` 4000000000 is the positive number 4000000000;
//   a conversion that yields -294967296 has converted the 32-bit BIT
//   PATTERN (0xEE6B2800 read as an `int32`), not the value.
//
//   stdlib §2.2 `Math` (v0.350) declares `Sqrt(x: double): double`,
//   `Pow(x: double, y: double): double` and the `Floor` / `Ceiling` /
//   `Truncate` families — every one of them a `double` (or a `decimal`)
//   parameter.
//
// THE DEFECT.
//
// The `double`-parameter path converted with a SIGNED float conversion, so
// the operand was read as a signed `int32` and then widened:
//
//   Math.Sqrt(4000000000u)    printed -nan              (should be 63245.553203367585)
//   Math.Sqrt(4000000000L)    printed 63245.553203367585 (the control)
//   Math.Pow(4000000000u, 2.0) printed 87005705709551620 (should be 1.6e+19)
//
// The control proves it is the UNSIGNEDNESS, not the magnitude.
//
// THE ORACLE IS POSITIVE: every PIN row asserts the EXACT VALUE the
// documents imply — not "the answer is not NaN".  A fix that produced 0, or
// Infinity, or any other non-NaN value, would fail these rows.
//
// THE MATRIX.  The issue's own warning: "A fix that changed the conversion
// for ALL integer operands ... would break the `long` case (and the `int`
// case, where the sign DOES matter)."  So the `int` / `long` / `uint` /
// `ulong` rows are here: the unsigned ones pin the value, the signed ones
// pin the sign.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::runAot;

namespace {

std::string prog(const std::string& body) {
    return "module M; "
           "public class C "
           "  public static method Main(): int " +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

// The accepting shape: the program RAN, it produced exactly this text, and
// it did not fail the compiler.
void expectValue(const std::string& program, const std::string& expected) {
    auto r = runAot(program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "compiler CRASHED"));
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

}  // namespace

// ===========================================================================
// THE PIN ROW — the issue's own repro.
//
// `sqrt(4000000000)` is 63245.553203367585 — the SAME value the `long`
// spelling produces, because the two spellings are the same number.
// ===========================================================================
BEZEL_TEST(math_uint_double_parameter_7242,
           math_sqrt_uint_argument_keeps_its_value_5_1) {
    expectValue(prog("    Console.WriteLine(Math.Sqrt(4000000000u).ToString());"),
                "63245.553203367585");
}

// ===========================================================================
// THE CONTROL — the `long` spelling of the same number, which was already
// right.  A fix that routed EVERY integer operand through the unsigned
// conversion would break this row.
// ===========================================================================
BEZEL_TEST(math_uint_double_parameter_7242,
           math_sqrt_long_argument_keeps_its_value_5_1) {
    expectValue(prog("    Console.WriteLine(Math.Sqrt(4000000000L).ToString());"),
                "63245.553203367585");
}

// ===========================================================================
// THE `ulong` SPELLING — the same number at the other unsigned width.
// ===========================================================================
BEZEL_TEST(math_uint_double_parameter_7242,
           math_sqrt_ulong_argument_keeps_its_value_5_1) {
    expectValue(prog("    Console.WriteLine(Math.Sqrt(4000000000UL).ToString());"),
                "63245.553203367585");
}

// ===========================================================================
// THE ISSUE'S SECOND SHAPE — a WRONG VALUE, not just a NaN.
//
// `4000000000^2` is 1.6e+19.  Pre-fix the program printed
// 87005705709551620, i.e. `(-294967296)^2` scaled by floating-point
// rounding.
// ===========================================================================
BEZEL_TEST(math_uint_double_parameter_7242,
           math_pow_uint_argument_keeps_its_value_5_1) {
    expectValue(prog("    Console.WriteLine(Math.Pow(4000000000u, 2.0).ToString());"),
                "1.6e+19");
}

// ===========================================================================
// THE `int` SPELLING — a small signed operand, where the sign does not
// matter but the VALUE must.
// ===========================================================================
BEZEL_TEST(math_uint_double_parameter_7242,
           math_sqrt_int_argument_keeps_its_value_5_1) {
    expectValue(prog("    Console.WriteLine(Math.Sqrt(9).ToString());"), "3");
}

// ===========================================================================
// THE SIGN CONTROL — a negative SIGNED operand keeps its sign, so the
// conversion is not "always unsigned".
// ===========================================================================
BEZEL_TEST(math_uint_double_parameter_7242,
           math_abs_signed_long_argument_keeps_its_sign_5_1) {
    expectValue(prog("    Console.WriteLine(Math.Abs(-3000000000L).ToString());"),
                "3000000000");
}

BEZEL_TEST(math_uint_double_parameter_7242,
           math_abs_signed_double_argument_keeps_its_sign_5_1) {
    expectValue(prog("    Console.WriteLine(Math.Abs(-2.5).ToString());"), "2.5");
}

// ===========================================================================
// THE `decimal` FAMILY — the sweep the issue asks for.
//
// §5.1 admits `uint -> decimal` too, and stdlib §2.2 declares
// `Floor(x: decimal): decimal`.  The `uint` reaches the `decimal` parameter
// through an explicit conversion (§5.2), and the value must survive it as
// well: 4000000000, not -294967296.
// ===========================================================================
BEZEL_TEST(math_uint_double_parameter_7242,
           math_floor_decimal_of_the_unsigned_value_keeps_its_value_5_1) {
    expectValue(
        prog("    Console.WriteLine(Math.Floor(decimal(4000000000u)).ToString());"),
        "4000000000");
}
