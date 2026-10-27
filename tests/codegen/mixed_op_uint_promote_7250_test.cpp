// Regression tests for #7250 — a mixed int/floating BINARY OPERATOR
// converted its UNSIGNED integer operand with `sitofp`, which SIGN-EXTENDS,
// so `1.5 + 4000000000u` computed -294967294.5 instead of 4000000001.5.
//
// WHAT THE DOCUMENTS REQUIRE.
//
// spec §6.2 "Mixed-type arithmetic" (v0.344):
//
//   "When operands have different types, the smaller-range operand is
//    implicitly promoted to the larger type using §5.1. The result has the
//    larger type."
//
// spec §5.1 "Implicit conversions" (v0.344) admits the promotion of an
// unsigned integer to a floating type:
//
//   | `uint` | `long`, `ulong`, `float`, `double`, `decimal` |
//
// §5.1 converts VALUES, not bit patterns. The `uint` 4000000000 has the bit
// pattern 0xEE6B2800; read as a SIGNED `int32` that is -294967296. So the
// unsigned-to-floating promotion must be value-preserving (a zero-extension,
// `uitofp`), not a sign-extension (`sitofp`).
//
// WHAT THE DEFECT WAS.
//
// The mixed int/floating lowering of a binary operator promoted the INTEGER
// operand with an unconditional `sitofp`. The `uint` 4000000000 was read as
// the `int32` -294967296, so `1.5 + 4000000000u` computed -294967294.5, and
// every comparison against such an operand took the wrong branch.
//
// WHAT THE FIX DOES.
//
// The promotion asks the operand's own SOURCE expression whether it is
// unsigned (`isUnsignedTargetExpr`, the same predicate the `double`-parameter
// converter of #7242 uses) and converts with `uitofp` for an unsigned source
// and `sitofp` for a signed one. The SIGNED path is unchanged.
//
// THE PROOF HIERARCHY.
//
// Every row below asserts the program's VALUE, never "it compiled" or "not
// NaN". Every FIX row is RED on the base arm (it printed the sign-extended
// value) and GREEN with the fix. The CONTROL rows are green on BOTH arms —
// they pin that the signed path, the already-correct `long`/`ulong` spellings
// and the pure-floating spellings did not move.
//
// `char` is NOT pinned here: §3.5 does not make `char` an arithmetic type, so
// it can never reach this promotion (the compiler rejects `char` arithmetic).

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

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

// The accepting shape: the program RAN and produced exactly this text.
void expectValue(const std::string& body, const std::string& expected) {
    auto r = runAot(prog(body));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "compiler CRASHED"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(
        r, "IR verification failed"));
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
// The issue's own repro — the `+` case (§6.2, §5.1).
// ===========================================================================

// The `uint` 4000000000 promotes to the `double` of the operation (§6.2), and
// the promotion converts the VALUE 4000000000 (§5.1), so the sum is
// 4000000001.5 — not the -294967294.5 the sign-extension produced.
BEZEL_TEST(mixed_op_uint_promote_7250, mixed_add_uint_operand_keeps_value_6_2) {
    expectValue("    Console.WriteLine(1.5 + 4000000000u);", "4000000001.5");
}

// ===========================================================================
// The rest of the arithmetic family §6.2 covers with the same promotion.
// ===========================================================================

// `1.5 - 4000000000u` = -3999999998.5 (the base arm computed
// 294967297.5, the sign-extended value subtracted from 1.5).
BEZEL_TEST(mixed_op_uint_promote_7250, mixed_sub_uint_operand_keeps_value_6_2) {
    expectValue("    Console.WriteLine(1.5 - 4000000000u);", "-3999999998.5");
}

// `1.5 * 4000000000u` = 6e9 exactly (the base arm computed -442450944).
BEZEL_TEST(mixed_op_uint_promote_7250, mixed_mul_uint_operand_keeps_value_6_2) {
    expectValue("    Console.WriteLine(1.5 * 4000000000u);", "6e+09");
}

// `1.5 / 4000000000u` = 3.75e-10 (the base arm computed -5.084e-09).
BEZEL_TEST(mixed_op_uint_promote_7250, mixed_div_uint_operand_keeps_value_6_2) {
    expectValue("    Console.WriteLine(1.5 / 4000000000u);", "3.75e-10");
}

// ===========================================================================
// The comparison operators — they share the promotion, and a wrong operand
// changes the BOOLEAN, so the boolean's VALUE is asserted.
// ===========================================================================

// 1.5 < 4000000000 is TRUE; the base arm compared against the sign-extended
// -294967296 and answered FALSE.
BEZEL_TEST(mixed_op_uint_promote_7250, mixed_less_than_uint_operand_keeps_value_6_2) {
    expectValue("    Console.WriteLine(1.5 < 4000000000u);", "True");
}

// 1.5 > 4000000000 is FALSE; the base arm answered TRUE.
BEZEL_TEST(mixed_op_uint_promote_7250, mixed_greater_than_uint_operand_keeps_value_6_2) {
    expectValue("    Console.WriteLine(1.5 > 4000000000u);", "False");
}

// 1.5 <= 4000000000 is TRUE; the base arm answered FALSE.
BEZEL_TEST(mixed_op_uint_promote_7250, mixed_less_or_equal_uint_operand_keeps_value_6_2) {
    expectValue("    Console.WriteLine(1.5 <= 4000000000u);", "True");
}

// 1.5 >= 4000000000 is FALSE; the base arm answered TRUE.
BEZEL_TEST(mixed_op_uint_promote_7250, mixed_greater_or_equal_uint_operand_keeps_value_6_2) {
    expectValue("    Console.WriteLine(1.5 >= 4000000000u);", "False");
}

// §4.3.1's reflexive equality over the PROMOTED operands: 4000000000.0 equals
// 4000000000u, so the answer is TRUE. The base arm promoted the `uint` to
// -294967296.0 and answered FALSE.
BEZEL_TEST(mixed_op_uint_promote_7250, mixed_equal_uint_operand_keeps_value_8_4) {
    expectValue("    Console.WriteLine(4000000000.0 = 4000000000u);", "True");
}

// §4.3.1: `<>` is the negation of `=`, so 4000000000.0 <> 4000000000u is
// FALSE. The base arm answered TRUE.
BEZEL_TEST(mixed_op_uint_promote_7250, mixed_not_equal_uint_operand_keeps_value_8_4) {
    expectValue("    Console.WriteLine(4000000000.0 <> 4000000000u);", "False");
}

// ===========================================================================
// The other unsigned widths that reach this promotion.
// ===========================================================================

// A `byte` operand: 200 promotes to 200.0 (§5.1), so the sum is 201.5. The
// base arm read the `i8` bit pattern 0xC8 as the signed -56 and answered
// -54.5.
BEZEL_TEST(mixed_op_uint_promote_7250, mixed_add_byte_operand_keeps_value_6_2) {
    expectValue("    var b: byte := 200; "
                "    Console.WriteLine(1.5 + b);",
                "201.5");
}

// A `ushort` operand: 40000 promotes to 40000.0 (§5.1), so the sum is
// 40001.5. The base arm read the `i16` bit pattern 0x9C40 as the signed
// -25536 and answered -25534.5.
BEZEL_TEST(mixed_op_uint_promote_7250, mixed_add_ushort_operand_keeps_value_6_2) {
    expectValue("    var s: ushort := 40000; "
                "    Console.WriteLine(1.5 + s);",
                "40001.5");
}

// A `ulong` operand ABOVE the signed `long` range: 18446744073709551615
// promotes to its own magnitude (§5.1). The base arm read the `i64` bit
// pattern as the signed -1 and answered 0.5.
BEZEL_TEST(mixed_op_uint_promote_7250, mixed_add_ulong_operand_keeps_value_6_2) {
    expectValue("    var u: ulong := 18446744073709551615ul; "
                "    Console.WriteLine(1.5 + u);",
                "18446744073709552000");
}

// ===========================================================================
// The `float` operand mixed with a `uint`, and the operand's POSITION.
// ===========================================================================

// A `float` operand mixed with a `uint`: §6.2 promotes the `uint` to the
// operation's `float`, and the promoted value is 4e9 (the base arm promoted
// it to -294967296 and answered -294967300).
BEZEL_TEST(mixed_op_uint_promote_7250, mixed_float_operand_with_uint_keeps_value_6_2) {
    expectValue("    Console.WriteLine(1.5f + 4000000000u);", "4e+09");
}

// The unsigned operand on the LEFT: the promotion is per-operand, so the
// `uint` is converted whichever side it is on. The base arm answered
// -294967294.5.
BEZEL_TEST(mixed_op_uint_promote_7250, mixed_add_uint_operand_on_left_keeps_value_6_2) {
    expectValue("    Console.WriteLine(4000000000u + 1.5);", "4000000001.5");
}

// The unsigned operand in a VARIABLE, not in a literal: the promotion happens
// at run time, not at a constant fold. The base arm answered -294967294.5.
BEZEL_TEST(mixed_op_uint_promote_7250, mixed_add_uint_variable_keeps_value_6_2) {
    expectValue("    var u: uint := 4000000000u; "
                "    Console.WriteLine(1.5 + u);",
                "4000000001.5");
}

// ===========================================================================
// THE CONTROLS — green on BOTH arms. The signed path must not move, and the
// spellings that were already correct must stay correct.
// ===========================================================================

// The `long` spelling of the same number: `long` is signed, so the
// sign-extension is the VALUE-preserving conversion here, and the answer was
// already 4000000001.5 before the fix.
BEZEL_TEST(mixed_op_uint_promote_7250, mixed_add_long_operand_control_6_2) {
    expectValue("    Console.WriteLine(1.5 + 4000000000L);", "4000000001.5");
}

// A negative signed `int` operand keeps its sign.
BEZEL_TEST(mixed_op_uint_promote_7250, mixed_add_negative_int_operand_control_6_2) {
    expectValue("    Console.WriteLine(1.5 + (-3));", "-1.5");
}

// A plain `int` operand.
BEZEL_TEST(mixed_op_uint_promote_7250, mixed_add_int_operand_control_6_2) {
    expectValue("    Console.WriteLine(1.5 + 3);", "4.5");
}

// A `ulong` operand BELOW the signed `long` range was already converted
// correctly (the `i64` bit pattern is the same either way), so the answer did
// not move.
BEZEL_TEST(mixed_op_uint_promote_7250, mixed_add_ulong_operand_control_6_2) {
    expectValue("    var u: ulong := 4000000000ul; "
                "    Console.WriteLine(1.5 + u);",
                "4000000001.5");
}

// The pure-floating spellings are untouched.
BEZEL_TEST(mixed_op_uint_promote_7250, mixed_add_double_operands_control_6_2) {
    expectValue("    Console.WriteLine(1.5 + 2.5); "
                "    Console.WriteLine(1.5 * 2.0);",
                "4\n3");
}
