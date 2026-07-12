// Fix #1241: register the PascalCase `Float` alias for `float` (stdlib §9.2).
//
// §3.4: "Each primitive keyword aliases a standard library type. The two forms
// are interchangeable: … `float`/`Single`, `double`/`Double` …"
// stdlib §9.2: `public record Float: IComparable<float>, IEquatable<float>`
// with static methods IsNaN/IsInfinity/IsPositiveInfinity/IsNegativeInfinity/
// IsFinite/IeeeEquals/BitwiseEquals and constants NaN/PositiveInfinity/
// NegativeInfinity/Epsilon/MinValue/MaxValue.
//
// Bug: before the fix, `Float` (the PascalCase name for `float`) was NOT
// registered in primitiveFromName() (TypeResolution.cpp), so any reference
// to `Float` — as a type annotation, as a static-member receiver, or as an
// explicit cast — produced "unresolved identifier `Float`". `Single` was
// registered (it maps to PrimitiveType::Float), but stdlib §9.2 names the
// record `Float`, not `Single`, so `Float.IsNaN(...)` failed while
// `Double.IsNaN(...)` and `float.IsNaN(...)` worked.
//
// Fix: added `if (name == "Float") return PrimitiveType::Float;` to
// primitiveFromName() (TypeResolution.cpp, §3.4/#1241), and added "Float" to
// isReservedPrimitiveAliasName() (SymbolCollection.cpp) so a user type named
// `Float` is cleanly rejected (§3.4/#1081).

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

// ── §9.2 Float.IsNaN — stdlib §9.2: IsNaN(value: float): bool ───────────────

// #1241 exact reproducer: Float.IsNaN(float(0.0)/float(0.0)) → 1 (was: unresolved).
BEZEL_TEST(codegen_float_pascal_1241, Float_IsNaN_nan_returns_true_1241) {
    // stdlib §9.2: Float.IsNaN — `Float` resolves to PrimitiveType::Float (#1241).
    BEZEL_EXPECT_EQ(run(
        "var nan := Float.NaN; "
        "if Float.IsNaN(nan) then return 1; end; "
        "return 0;"), 1);
}

BEZEL_TEST(codegen_float_pascal_1241, Float_IsNaN_finite_returns_false_1241) {
    // stdlib §9.2: IsNaN on a finite float → false.
    BEZEL_EXPECT_EQ(run(
        "if Float.IsNaN(1.0f) then return 1; end; "
        "return 0;"), 0);
}

// ── §9.2 Float.IsInfinity ────────────────────────────────────────────────────

BEZEL_TEST(codegen_float_pascal_1241, Float_IsInfinity_posInf_returns_true_1241) {
    // stdlib §9.2: IsInfinity(+Infinity) → true.
    BEZEL_EXPECT_EQ(run(
        "var inf := Float.PositiveInfinity; "
        "if Float.IsInfinity(inf) then return 1; end; "
        "return 0;"), 1);
}

BEZEL_TEST(codegen_float_pascal_1241, Float_IsInfinity_finite_returns_false_1241) {
    // stdlib §9.2: IsInfinity on a finite float → false.
    BEZEL_EXPECT_EQ(run(
        "if Float.IsInfinity(float(1.5)) then return 1; end; "
        "return 0;"), 0);
}

// ── §9.2 Float.IsFinite ──────────────────────────────────────────────────────

BEZEL_TEST(codegen_float_pascal_1241, Float_IsFinite_finite_returns_true_1241) {
    // stdlib §9.2: IsFinite(1.5f) → true.
    BEZEL_EXPECT_EQ(run(
        "if Float.IsFinite(1.5f) then return 1; end; "
        "return 0;"), 1);
}

BEZEL_TEST(codegen_float_pascal_1241, Float_IsFinite_nan_returns_false_1241) {
    // stdlib §9.2: IsFinite(NaN) → false.
    BEZEL_EXPECT_EQ(run(
        "var nan := Float.NaN; "
        "if Float.IsFinite(nan) then return 1; end; "
        "return 0;"), 0);
}

BEZEL_TEST(codegen_float_pascal_1241, Float_IsFinite_posInf_returns_false_1241) {
    // stdlib §9.2: IsFinite(+Infinity) → false.
    BEZEL_EXPECT_EQ(run(
        "var inf := Float.PositiveInfinity; "
        "if Float.IsFinite(inf) then return 1; end; "
        "return 0;"), 0);
}

// ── §9.2 Float constant: NaN ─────────────────────────────────────────────────

BEZEL_TEST(codegen_float_pascal_1241, Float_NaN_constant_is_nan_1241) {
    // stdlib §9.2: Float.NaN constant — when tested with float.IsNaN → true.
    BEZEL_EXPECT_EQ(run(
        "var n := Float.NaN; "
        "return (1 if float.IsNaN(n) else 0);"), 1);
}

// ── §9.2 Float constant: MaxValue ────────────────────────────────────────────

BEZEL_TEST(codegen_float_pascal_1241, Float_MaxValue_greater_than_million_1241) {
    // stdlib §9.2: Float.MaxValue is the largest finite float (~3.4e38).
    BEZEL_EXPECT_EQ(run(
        "return (1 if Float.MaxValue > float(1000000.0) else 0);"), 1);
}

// ── §9.2 Float constant: Epsilon ─────────────────────────────────────────────

BEZEL_TEST(codegen_float_pascal_1241, Float_Epsilon_positive_1241) {
    // stdlib §9.2: Float.Epsilon is the smallest positive float — must be > 0.
    BEZEL_EXPECT_EQ(run(
        "return (1 if Float.Epsilon > float(0.0) else 0);"), 1);
}

// ── §9.2 Float.IeeeEquals ────────────────────────────────────────────────────

BEZEL_TEST(codegen_float_pascal_1241, Float_IeeeEquals_equal_values_1241) {
    // stdlib §9.2: IeeeEquals(1.5f, 1.5f) → true (strict IEEE ordered-equal).
    BEZEL_EXPECT_EQ(run(
        "if Float.IeeeEquals(1.5f, 1.5f) then return 1; end; "
        "return 0;"), 1);
}

BEZEL_TEST(codegen_float_pascal_1241, Float_IeeeEquals_nan_vs_nan_false_1241) {
    // stdlib §9.2: IeeeEquals(NaN, NaN) → false (NaN not equal to itself in IEEE).
    BEZEL_EXPECT_EQ(run(
        "var nan := Float.NaN; "
        "if Float.IeeeEquals(nan, nan) then return 1; end; "
        "return 0;"), 0);
}

// ── §9.2 Float.BitwiseEquals ─────────────────────────────────────────────────

BEZEL_TEST(codegen_float_pascal_1241, Float_BitwiseEquals_equal_values_1241) {
    // stdlib §9.2: BitwiseEquals(1.5f, 1.5f) → true (same bit pattern).
    BEZEL_EXPECT_EQ(run(
        "if Float.BitwiseEquals(1.5f, 1.5f) then return 1; end; "
        "return 0;"), 1);
}

BEZEL_TEST(codegen_float_pascal_1241, Float_BitwiseEquals_different_values_false_1241) {
    // stdlib §9.2: BitwiseEquals(1.5f, 2.5f) → false (different bit patterns).
    BEZEL_EXPECT_EQ(run(
        "if Float.BitwiseEquals(1.5f, 2.5f) then return 1; end; "
        "return 0;"), 0);
}

// ── §3.4 Float as type annotation ────────────────────────────────────────────

BEZEL_TEST(codegen_float_pascal_1241, Float_as_type_annotation_1241) {
    // §3.4: `Float` in a type position is interchangeable with `float`.
    // `var x: Float := float(3.14)` — the Float type annotation is resolved as
    // Primitive/Float, so Float.IsFinite(x) works correctly.
    BEZEL_EXPECT_EQ(run(
        "var x: Float := float(3.14); "
        "return (1 if Float.IsFinite(x) else 0);"), 1);
}

// ── Regression: lowercase float.IsNaN / double.IsNaN still work ──────────────

BEZEL_TEST(codegen_float_pascal_1241, float_IsNaN_lowercase_regression_1241) {
    // Regression: the `float` keyword form must not be broken by the Float fix.
    BEZEL_EXPECT_EQ(run(
        "var nan := float.NaN; "
        "if float.IsNaN(nan) then return 1; end; "
        "return 0;"), 1);
}

BEZEL_TEST(codegen_float_pascal_1241, Double_IsNaN_regression_1241) {
    // Regression: Double.IsNaN (PascalCase double alias) still works after Float fix.
    BEZEL_EXPECT_EQ(run(
        "var nan := Double.NaN; "
        "if Double.IsNaN(nan) then return 1; end; "
        "return 0;"), 1);
}

BEZEL_TEST(codegen_float_pascal_1241, Int32_MaxValue_regression_1241) {
    // Regression: Int32.MaxValue still resolves — other PascalCase aliases unaffected.
    BEZEL_EXPECT_EQ(run(
        "return (1 if Int32.MaxValue > 2000000000 else 0);"), 1);
}
