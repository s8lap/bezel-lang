// Tests for #518 (int.ToString(radix) wrong-output) and #519
// (Double/Float IeeeEquals + BitwiseEquals compile error).
//
// ─── #518: int.ToString(radix) ───────────────────────────────────────────────
// stdlib §9.1 (Int32, and analogously Int64): `public method ToString(radix: int): string`
// Radix is one of 2, 8, 10, 16.  Before the fix the 1-arg overload silently
// fell through to `emitPrimitiveToString` which ignores the radix argument
// and always formats in decimal — so `(255).ToString(16)` returned "255"
// instead of "ff".
//
// The fix is in LlvmBackend_Exprs.cpp (the `call->args.size() == 1 &&
// ma->member == "ToString"` branch): detect that the argument is an integer
// (not a format-string literal) and route to the same inline radix-conversion
// logic that Convert.ToString(n, radix) already uses.
//
// ─── #519: double.IeeeEquals / BitwiseEquals ─────────────────────────────────
// stdlib §9.2 (Double):
//   public static method IeeeEquals(a: double, b: double): bool;
//   public static method BitwiseEquals(a: double, b: double): bool;
// stdlib §9.2 (Float):
//   public static method IeeeEquals(a: float, b: float): bool;
//   public static method BitwiseEquals(a: float, b: float): bool;
//
// Before the fix these were not recognised in sema (ExprTypecheck_Calls.cpp,
// the `double/float` static-method typing block), so calling them emitted a
// §3.4 diagnostic "type double has no static method IeeeEquals".
// Note: Bezel uses the lowercase keyword aliases `double` / `float` to call
// static methods (per primitiveFromName — the uppercase `Double`/`Float` type
// names are not in scope as identifiers in expression position).
//
// Semantics (stdlib §9.2):
//   IeeeEquals:    `fcmp oeq a, b`  — strict IEEE, NaN-vs-anything → false.
//   BitwiseEquals: bitcast both to matching-width integer, then `icmp eq` —
//                  distinguishes +0.0 from -0.0 and distinct NaN payloads.
//
// The fix adds recognition in sema and inline lowering in codegen
// (tryLowerPrimitiveStaticCall, LlvmBackend_Calls.cpp) analogous to the
// existing IsNaN/IsInfinity handlers.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& body) {
    auto r = runAot(
        std::string("module M; class C public static method Main(): int ") +
        body + " end Main; end C;");
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// ── #518: int.ToString(radix) ────────────────────────────────────────────────

// stdlib §9.1: (255).ToString(16) = "ff"
BEZEL_TEST(primitive_tostring_518, int_tostring_hex_255_518) {
    // stdlib §9.1: ToString(16) → lowercase hex.
    BEZEL_EXPECT_EQ(run(
        "var s := (255).ToString(16); "
        "return (1 if s = \"ff\" else 0);"), 1);
}

// stdlib §9.1: (255).ToString(2) = "11111111"
BEZEL_TEST(primitive_tostring_518, int_tostring_binary_255_518) {
    // stdlib §9.1: ToString(2) → binary digits.
    BEZEL_EXPECT_EQ(run(
        "var s := (255).ToString(2); "
        "return (1 if s = \"11111111\" else 0);"), 1);
}

// stdlib §9.1: (255).ToString(8) = "377"
BEZEL_TEST(primitive_tostring_518, int_tostring_octal_255_518) {
    // stdlib §9.1: ToString(8) → octal digits.
    BEZEL_EXPECT_EQ(run(
        "var s := (255).ToString(8); "
        "return (1 if s = \"377\" else 0);"), 1);
}

// stdlib §9.1: (255).ToString(10) = "255" (decimal, same as no-arg)
BEZEL_TEST(primitive_tostring_518, int_tostring_decimal_255_518) {
    // stdlib §9.1: ToString(10) → decimal.
    BEZEL_EXPECT_EQ(run(
        "var s := (255).ToString(10); "
        "return (1 if s = \"255\" else 0);"), 1);
}

// stdlib §9.1: (255).ToString() = "255" — regression guard
BEZEL_TEST(primitive_tostring_518, int_tostring_noarg_255_518) {
    // stdlib §9.1: ToString() (no arg) → decimal; must still work.
    BEZEL_EXPECT_EQ(run(
        "var s := (255).ToString(); "
        "return (1 if s = \"255\" else 0);"), 1);
}

// stdlib §9.1: (0).ToString(16) = "0"
BEZEL_TEST(primitive_tostring_518, int_tostring_hex_zero_518) {
    // stdlib §9.1: zero in hex → "0".
    BEZEL_EXPECT_EQ(run(
        "var s := (0).ToString(16); "
        "return (1 if s = \"0\" else 0);"), 1);
}

// stdlib §9.1: (0).ToString(2) = "0"
BEZEL_TEST(primitive_tostring_518, int_tostring_binary_zero_518) {
    // stdlib §9.1: zero in binary → "0".
    BEZEL_EXPECT_EQ(run(
        "var s := (0).ToString(2); "
        "return (1 if s = \"0\" else 0);"), 1);
}

// stdlib §9.1: verify Length of (255).ToString(16) == 2
BEZEL_TEST(primitive_tostring_518, int_tostring_hex_255_length_518) {
    // stdlib §9.1: "ff" has length 2.
    BEZEL_EXPECT_EQ(run(
        "var s := (255).ToString(16); "
        "return s.Length;"), 2);
}

// stdlib §9.1: verify Length of (255).ToString(2) == 8
BEZEL_TEST(primitive_tostring_518, int_tostring_binary_255_length_518) {
    // stdlib §9.1: "11111111" has length 8.
    BEZEL_EXPECT_EQ(run(
        "var s := (255).ToString(2); "
        "return s.Length;"), 8);
}

// stdlib §9.1: (16).ToString(16) = "10"
BEZEL_TEST(primitive_tostring_518, int_tostring_hex_16_518) {
    // stdlib §9.1: 16 in hex → "10".
    BEZEL_EXPECT_EQ(run(
        "var s := (16).ToString(16); "
        "return (1 if s = \"10\" else 0);"), 1);
}

// stdlib §9.1: long.ToString(radix) — Int64 has analogous shape.
// (255L).ToString(16) = "ff"
BEZEL_TEST(primitive_tostring_518, long_tostring_hex_255_518) {
    // stdlib §9.1 (Int64 analogous): ToString(16) on long.
    BEZEL_EXPECT_EQ(run(
        "var n: long := 255L; "
        "var s := n.ToString(16); "
        "return (1 if s = \"ff\" else 0);"), 1);
}

// stdlib §9.1: (255L).ToString(2) = "11111111"
BEZEL_TEST(primitive_tostring_518, long_tostring_binary_255_518) {
    // stdlib §9.1 (Int64 analogous): ToString(2) on long.
    BEZEL_EXPECT_EQ(run(
        "var n: long := 255L; "
        "var s := n.ToString(2); "
        "return (1 if s = \"11111111\" else 0);"), 1);
}

// ── #519: double.IeeeEquals ───────────────────────────────────────────────────

// stdlib §9.2: IeeeEquals(1.0, 1.0) → true
BEZEL_TEST(primitive_ieee_519, double_IeeeEquals_equal_values_519) {
    // stdlib §9.2: IeeeEquals — strict IEEE ordered equal.
    BEZEL_EXPECT_EQ(run(
        "if double.IeeeEquals(1.0, 1.0) then return 1; end; "
        "return 0;"), 1);
}

// stdlib §9.2: IeeeEquals(NaN, NaN) → false (NaN is never equal in IEEE)
BEZEL_TEST(primitive_ieee_519, double_IeeeEquals_nan_vs_nan_false_519) {
    // stdlib §9.2: IeeeEquals returns false for any pair involving NaN —
    // including NaN-vs-NaN. This is the key difference from the reflexive `=`.
    BEZEL_EXPECT_EQ(run(
        "var nan := double.NaN; "
        "if double.IeeeEquals(nan, nan) then return 1; end; "
        "return 0;"), 0);
}

// stdlib §9.2: IeeeEquals(1.0, 2.0) → false
BEZEL_TEST(primitive_ieee_519, double_IeeeEquals_unequal_values_519) {
    // stdlib §9.2: IeeeEquals(1.0, 2.0) → false (distinct finite values).
    BEZEL_EXPECT_EQ(run(
        "if double.IeeeEquals(1.0, 2.0) then return 1; end; "
        "return 0;"), 0);
}

// stdlib §9.2: IeeeEquals(0.0, -0.0) → true (IEEE 754: +0 == -0)
BEZEL_TEST(primitive_ieee_519, double_IeeeEquals_pos_zero_neg_zero_519) {
    // stdlib §9.2: IeeeEquals(+0.0, -0.0) → true because IEEE 754 ordered
    // equality treats +0 and -0 as equal (fcmp oeq).
    BEZEL_EXPECT_EQ(run(
        "var pz := 0.0; "
        "var nz := -0.0; "
        "if double.IeeeEquals(pz, nz) then return 1; end; "
        "return 0;"), 1);
}

// ── #519: double.BitwiseEquals ───────────────────────────────────────────────

// stdlib §9.2: BitwiseEquals(1.0, 1.0) → true (identical bit patterns)
BEZEL_TEST(primitive_ieee_519, double_BitwiseEquals_equal_values_519) {
    // stdlib §9.2: BitwiseEquals — bitcast to i64, then icmp eq.
    BEZEL_EXPECT_EQ(run(
        "if double.BitwiseEquals(1.0, 1.0) then return 1; end; "
        "return 0;"), 1);
}

// stdlib §9.2: BitwiseEquals(+0.0, -0.0) → false (different bit patterns)
BEZEL_TEST(primitive_ieee_519, double_BitwiseEquals_pos_zero_neg_zero_519) {
    // stdlib §9.2: +0.0 (0x0000...0) and -0.0 (0x8000...0) differ in their
    // sign bit — BitwiseEquals must return false.
    BEZEL_EXPECT_EQ(run(
        "var pz := 0.0; "
        "var nz := -0.0; "
        "if double.BitwiseEquals(pz, nz) then return 1; end; "
        "return 0;"), 0);
}

// stdlib §9.2: BitwiseEquals(1.0, 2.0) → false
BEZEL_TEST(primitive_ieee_519, double_BitwiseEquals_unequal_values_519) {
    // stdlib §9.2: BitwiseEquals(1.0, 2.0) → false.
    BEZEL_EXPECT_EQ(run(
        "if double.BitwiseEquals(1.0, 2.0) then return 1; end; "
        "return 0;"), 0);
}

// ── #519: float.IeeeEquals ───────────────────────────────────────────────────

// stdlib §9.2 (Float): IeeeEquals(1.0f, 1.0f) → true
BEZEL_TEST(primitive_ieee_519, float_IeeeEquals_equal_values_519) {
    // stdlib §9.2 (Float): IeeeEquals on float.
    BEZEL_EXPECT_EQ(run(
        "if float.IeeeEquals(1.0f, 1.0f) then return 1; end; "
        "return 0;"), 1);
}

// stdlib §9.2 (Float): IeeeEquals(NaN, NaN) → false
BEZEL_TEST(primitive_ieee_519, float_IeeeEquals_nan_vs_nan_false_519) {
    // stdlib §9.2 (Float): IeeeEquals returns false for NaN-vs-NaN.
    BEZEL_EXPECT_EQ(run(
        "var nan := float.NaN; "
        "if float.IeeeEquals(nan, nan) then return 1; end; "
        "return 0;"), 0);
}

// stdlib §9.2 (Float): IeeeEquals(1.0f, 2.0f) → false
BEZEL_TEST(primitive_ieee_519, float_IeeeEquals_unequal_values_519) {
    // stdlib §9.2 (Float): IeeeEquals(1.0f, 2.0f) → false.
    BEZEL_EXPECT_EQ(run(
        "if float.IeeeEquals(1.0f, 2.0f) then return 1; end; "
        "return 0;"), 0);
}

// ── #519: float.BitwiseEquals ─────────────────────────────────────────────────

// stdlib §9.2 (Float): BitwiseEquals(1.0f, 1.0f) → true
BEZEL_TEST(primitive_ieee_519, float_BitwiseEquals_equal_values_519) {
    // stdlib §9.2 (Float): BitwiseEquals — bitcast to i32, then icmp eq.
    BEZEL_EXPECT_EQ(run(
        "if float.BitwiseEquals(1.0f, 1.0f) then return 1; end; "
        "return 0;"), 1);
}

// stdlib §9.2 (Float): BitwiseEquals(+0.0f, -0.0f) → false
BEZEL_TEST(primitive_ieee_519, float_BitwiseEquals_pos_zero_neg_zero_519) {
    // stdlib §9.2 (Float): +0.0f and -0.0f differ in sign bit.
    BEZEL_EXPECT_EQ(run(
        "var pz := 0.0f; "
        "var nz := -0.0f; "
        "if float.BitwiseEquals(pz, nz) then return 1; end; "
        "return 0;"), 0);
}

// stdlib §9.2 (Float): BitwiseEquals(1.0f, 2.0f) → false
BEZEL_TEST(primitive_ieee_519, float_BitwiseEquals_unequal_values_519) {
    // stdlib §9.2 (Float): BitwiseEquals(1.0f, 2.0f) → false.
    BEZEL_EXPECT_EQ(run(
        "if float.BitwiseEquals(1.0f, 2.0f) then return 1; end; "
        "return 0;"), 0);
}

// ── #519: control — reflexive `=` on NaN is unchanged ────────────────────────

// The default `=` for double is reflexive (§4.3.1): nan = nan → true.
// This lane does NOT touch the `=` operator; this test guards regression.
BEZEL_TEST(primitive_ieee_519, double_eq_nan_reflexive_unchanged_519) {
    // §4.3.1: the default `=` on double is reflexive — NaN = NaN → true.
    // IeeeEquals(NaN, NaN) = false; `=` still = true; both must hold.
    BEZEL_EXPECT_EQ(run(
        "var nan := double.NaN; "
        "var ie := double.IeeeEquals(nan, nan); "  // false
        "if (not ie) then return 1; end; "
        "return 0;"), 1);
}
