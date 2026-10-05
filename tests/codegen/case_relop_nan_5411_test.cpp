// Fix verification tests for bug #5411 (§19.3/§19.6).
//
// §19.3 / §4.3.1: a NaN scrutinee satisfies no relational pattern.
// The ordered floating-point comparisons (CreateFCmpOLT/OGT/OLE/OGE) used in
// codegen return false when either operand is NaN — so the matching half
// (§4.3.1 "NaN is unordered: every comparison is false") is already correct.
// These tests verify the runtime behavior (matching half).
//
// §19.6: no relational arm counts toward a proof of exhaustiveness.
// A `case` over float/double with only relational arms is NON-EXHAUSTIVE
// (NaN is never matched by any relational arm, and no relational arm covers
// the full value space). The exhaustiveness warning tests are in
// tests/sema/case_relop_nan_5411_test.cpp.
//
// Stdlib §9.2: Double.NaN, Double.IsNaN(value: double): bool
// Stdlib §9.2: Float.NaN, Float.IsNaN(value: float): bool

#include "support/aot_support.hpp"   // runAot — AOT test harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

// ── §4.3.1 / §19.3: NaN double does not match a relational arm ────────────────
//
// BEHAVIOR BEFORE FIX: if the lowering negated the comparison (e.g. using
// unordered instead of ordered fcmp, or using fcmp eq + NOT), a NaN scrutinee
// would incorrectly match the arm. The fix proves this cannot happen.
//
// The ordered fcmp OLT/OGT/OLE/OGE used in codegen already returns false when
// either operand is NaN, so this is a REGRESSION PIN (passes before AND after).
// We verify it with a positive control: a finite value 0.5 < 1.0 DOES match.

// Positive control: finite value 0.5 < 1.0 matches, returns 1
BEZEL_TEST(case_nan_5411, nan_double_not_in_relational_lt_arm_positive_control) {
    BEZEL_EXPECT_EQ(runAot(
        "module M; class C public static method Main(): int "
        "var v: double := 0.5; "
        "var r := (case v of < 1.0 => 1, _ => 2); "
        "return r; "
        "end Main; end C;").exitCode, 1);
}

// NaN double: (case nan of < 1.0 => 1, _ => 2) should return 2 (else arm).
// Before fix: if codegen wrongly negated the comparison, this would return 1.
// After fix: ordered fcmp OLT returns false for NaN, so first arm does not
// match, else arm is taken, returns 2.
BEZEL_TEST(case_nan_5411, nan_double_falls_through_relational_lt_arm) {
    BEZEL_EXPECT_EQ(runAot(
        "module M; class C public static method Main(): int "
        "var nan: double := double.NaN; "
        "var r := (case nan of < 1.0 => 1, _ => 2); "
        "return r; "
        "end Main; end C;").exitCode, 2);
}

// NaN double with >= arm: nan >= 1.0 is also false, so else is taken.
BEZEL_TEST(case_nan_5411, nan_double_falls_through_relational_ge_arm) {
    BEZEL_EXPECT_EQ(runAot(
        "module M; class C public static method Main(): int "
        "var nan: double := double.NaN; "
        "var r := (case nan of >= 0.0 => 1, _ => 2); "
        "return r; "
        "end Main; end C;").exitCode, 2);
}

// NaN confirmed by double.IsNaN before the case.
BEZEL_TEST(case_nan_5411, nan_double_confirmed_by_isnan_before_case) {
    BEZEL_EXPECT_EQ(runAot(
        "module M; class C public static method Main(): int "
        "var nan: double := double.NaN; "
        "if double.IsNaN(nan) then "
        "  var r := (case nan of < 1.0 => 1, _ => 2); "
        "  return r; "
        "end if; "
        "return 99; "
        "end Main; end C;").exitCode, 2);
}

// ── §19.6: positive control — finite double still matches relational arm ───────
//
// After the fix excludes relational arms from the proof, a finite value must
// still match its relational arm. This positive control verifies the arm still
// functions correctly.

BEZEL_TEST(case_nan_5411, finite_double_matches_lt_arm) {
    BEZEL_EXPECT_EQ(runAot(
        "module M; class C public static method Main(): int "
        "var v: double := 0.5; "
        "var r := (case v of < 1.0 => 1, _ => 2); "
        "return r; "
        "end Main; end C;").exitCode, 1);
}

BEZEL_TEST(case_nan_5411, finite_double_matches_ge_arm) {
    BEZEL_EXPECT_EQ(runAot(
        "module M; class C public static method Main(): int "
        "var v: double := 2.0; "
        "var r := (case v of < 1.0 => 1, >= 1.0 => 2, _ => 3); "
        "return r; "
        "end Main; end C;").exitCode, 2);
}

// Positive control: infinity is finite? No — IsFinite(+inf) = false.
// +infinity is NOT NaN, so it matches relational arms correctly.
BEZEL_TEST(case_nan_5411, positive_infinity_matches_gt_arm) {
    BEZEL_EXPECT_EQ(runAot(
        "module M; class C public static method Main(): int "
        "var inf: double := 1.0 / 0.0; "  // +infinity
        "var r := (case inf of > 0.0 => 1, _ => 2); "
        "return r; "
        "end Main; end C;").exitCode, 1);
}
