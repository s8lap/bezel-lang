// fix #1981 / §8.4 (null-safe `=`/`<>`) / §7.11 (lifted operators) / §6.2
// (numeric promotion): a nullable FP value (`double?`, `float?`) compared with
// `=`/`<>` against a non-nullable operand emitted MALFORMED IR. Equality is NOT
// lifted by the §7.11 block (which handles only arithmetic/relational), so a
// `double? = <double>` fell through to the scalar FP promotion path, which ran
// `toFP()` on the whole `{i8 hasValue, double}` wrapper struct →
// `sitofp {i8,double} to double` (IR-verify failure). The `double? = <int>`
// variant instead reached the null-safe nullable-value equality path but that
// path only widened INTEGER payloads, so the FP-payload-vs-int-plain pair fell
// through to a bare ICmp on mismatched types (an LLVM assert / crash).
//
// The fix (a) excludes a `=`/`<>` with a nullable-value struct operand from the
// scalar FP path so it reaches the null-safe equality path, and (b) promotes an
// integer plain operand to the FP payload type (SIToFP/UIToFP) and widens a
// narrower float to the wider FP type (FPExt) — on the UNWRAPPED payload/plain
// SCALARS, never the wrapper struct — so the value comparison sees matching
// types (§6.2 mixed-type numeric promotion, exactly as the non-nullable path).
//
// Only `double?`/`float?` (the FP nullable-prims) were affected; int?/bool?/
// enum?/decimal? equality was already correct. Both-null → equal and
// null-vs-present → not-equal (§8.4) are exercised so the null tag still governs.
//
// Value tests — runAot returns the process exit code (0/1).

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
std::string mainOf(const std::string& body) {
    return "module M; class C public static method Main(): int " + body +
           " end Main; end C;";
}
}  // namespace

// §8.4 / §7.11: `double? = <double literal>` — the exact #1981 repro. Without
// the fix codegen emitted `sitofp {i8,double} to double` → IR-verify failure
// (compile abort). Present value equal to the literal → true (return 1).
BEZEL_TEST(nullable_fp_equality_1981, double_nullable_eq_double_literal_true) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var d: double? := 3.14; "
        "if d = 3.14 then return 1; else return 0; end;")), 1);
}

// §8.4: present value NOT equal to the literal → false (return 0).
BEZEL_TEST(nullable_fp_equality_1981, double_nullable_eq_double_literal_false) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var d: double? := 3.14; "
        "if d = 2.0 then return 1; else return 0; end;")), 0);
}

// §8.4: `null = nonNull` is FALSE — a null double? is not equal to any value.
BEZEL_TEST(nullable_fp_equality_1981, double_nullable_null_eq_value_false) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var d: double? := null; "
        "if d = 3.14 then return 1; else return 0; end;")), 0);
}

// §8.4: `<>` is the negation — a present value equal to the literal → false.
BEZEL_TEST(nullable_fp_equality_1981, double_nullable_ne_double_literal_false) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var d: double? := 3.14; "
        "if d <> 3.14 then return 1; else return 0; end;")), 0);
}

// §8.4: `null <> nonNull` is TRUE.
BEZEL_TEST(nullable_fp_equality_1981, double_nullable_null_ne_value_true) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var d: double? := null; "
        "if d <> 3.14 then return 1; else return 0; end;")), 1);
}

// §8.4 / §6.2: `double? = <int literal>` — the int promotes to double before the
// compare (was a bare ICmp on {double payload} vs i32 → LLVM assert/crash).
// 3.0 = 3 → true.
BEZEL_TEST(nullable_fp_equality_1981, double_nullable_eq_int_literal_true) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var d: double? := 3.0; "
        "if d = 3 then return 1; else return 0; end;")), 1);
}

// §6.2: 3.14 = 3 → false (the int widens losslessly; no truncation of the
// payload to the int makes 3.14 spuriously equal 3).
BEZEL_TEST(nullable_fp_equality_1981, double_nullable_eq_int_literal_false) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var d: double? := 3.14; "
        "if d = 3 then return 1; else return 0; end;")), 0);
}

// §8.4: reversed operand order `<double literal> = <double?>` also reaches the
// null-safe path (structSide detection covers either side). 3.14 = d → true.
BEZEL_TEST(nullable_fp_equality_1981, double_literal_eq_double_nullable_rev) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var d: double? := 3.14; "
        "if 3.14 = d then return 1; else return 0; end;")), 1);
}

// §6.2: `float? = <double literal>` — the f32 payload widens to f64 (FPExt)
// before the compare. 1.5f = 1.5 → true.
BEZEL_TEST(nullable_fp_equality_1981, float_nullable_eq_double_literal_true) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var f: float? := 1.5f; "
        "if f = 1.5 then return 1; else return 0; end;")), 1);
}

// §7.11: `float? = <float literal>` (matching widths) still compares correctly.
BEZEL_TEST(nullable_fp_equality_1981, float_nullable_eq_float_literal_true) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var f: float? := 1.5f; "
        "if f = 1.5f then return 1; else return 0; end;")), 1);
}

// §8.4: two nullable doubles — both null → EQUAL (return 1). Regression guard
// that the FP-payload promotion does not wrongly fire on the both-struct path.
BEZEL_TEST(nullable_fp_equality_1981, double_nullable_both_null_equal) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var a: double? := null; var b: double? := null; "
        "if a = b then return 1; else return 0; end;")), 1);
}

// §8.4: present vs null → NOT equal (return 0).
BEZEL_TEST(nullable_fp_equality_1981, double_nullable_present_vs_null_unequal) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var a: double? := 3.14; var b: double? := null; "
        "if a = b then return 1; else return 0; end;")), 0);
}

// §8.4: two present nullable doubles with equal payloads → EQUAL.
BEZEL_TEST(nullable_fp_equality_1981, double_nullable_both_present_equal) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var a: double? := 3.14; var b: double? := 3.14; "
        "if a = b then return 1; else return 0; end;")), 1);
}

// Regression: `int? = <int>` (the integer nullable-prim) is unaffected — the FP
// promotion must not perturb the integer path. 3 = 3 → true.
BEZEL_TEST(nullable_fp_equality_1981, int_nullable_eq_int_unaffected) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var n: int? := 3; "
        "if n = 3 then return 1; else return 0; end;")), 1);
}

// =========================================================================
// #1991 — BOTH-nullable equality with DIFFERENT FP widths (`float? = double?`).
// This case took neither the same-width both-nullable branch (LLVM struct types
// differ) nor the mixed nullable-vs-scalar #1981 promotion (the "plain" side is
// itself a struct), and fell through to the bare struct-equality path — which
// extracted a float payload from one operand and a double from the other →
// `fcmp oeq float, double` (malformed IR, compile abort). The fix promotes the
// narrower payload to the wider FP type (FPExt) and applies the null-safe §8.4
// rule on the promoted scalars.
// =========================================================================

// §6.2 / §8.4: `float? = double?`, both present with equal value — the exact
// #1991 repro. float(1.5) widens to double 1.5 → EQUAL (return 1). Without the
// fix this did not compile (IR-verify abort).
BEZEL_TEST(nullable_fp_equality_1991, float_nullable_eq_double_nullable_true) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var a: float? := 1.5f; var b: double? := 1.5; "
        "if a = b then return 1; else return 0; end;")), 1);
}

// §8.4: `<>` is the negation — both present and equal → false (return 0).
BEZEL_TEST(nullable_fp_equality_1991, float_nullable_ne_double_nullable_false) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var a: float? := 1.5f; var b: double? := 1.5; "
        "if a <> b then return 1; else return 0; end;")), 0);
}

// §8.4: both present, values differ → NOT equal (return 0).
BEZEL_TEST(nullable_fp_equality_1991, float_nullable_eq_double_nullable_differ) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var a: float? := 1.5f; var b: double? := 2.5; "
        "if a = b then return 1; else return 0; end;")), 0);
}

// §8.4: BOTH null → EQUAL (return 1). The null-safe both-null rule must hold
// across widths — the struct-equality fall-through would have compared the
// (meaningless) payload slots instead.
BEZEL_TEST(nullable_fp_equality_1991, float_double_both_null_equal) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var a: float? := null; var b: double? := null; "
        "if a = b then return 1; else return 0; end;")), 1);
}

// §8.4: exactly one null → NOT equal (return 0), regardless of the other's value.
BEZEL_TEST(nullable_fp_equality_1991, float_null_vs_double_present_unequal) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var a: float? := null; var b: double? := 1.5; "
        "if a = b then return 1; else return 0; end;")), 0);
}

// §8.4: exactly one null with `<>` → TRUE (return 1).
BEZEL_TEST(nullable_fp_equality_1991, float_null_vs_double_present_ne_true) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var a: float? := null; var b: double? := 1.5; "
        "if a <> b then return 1; else return 0; end;")), 1);
}

// §6.2: reversed operand order `double? = float?` reaches the same branch (both
// sides are inspected for the nullable-value struct shape). 1.5 = 1.5f → true.
BEZEL_TEST(nullable_fp_equality_1991, double_nullable_eq_float_nullable_rev) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var a: double? := 1.5; var b: float? := 1.5f; "
        "if a = b then return 1; else return 0; end;")), 1);
}

// §6.2: a value NOT exactly representable in float — `float? = double?` promotes
// the float payload to double, so float(0.1) (≈0.100000001…) is NOT equal to the
// double 0.1. This documents that the promotion is float→double (mixed-width
// §6.2), not a lossy double→float narrowing that would spuriously equate them.
BEZEL_TEST(nullable_fp_equality_1991, float_double_precision_widening_unequal) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var a: float? := 0.1f; var b: double? := 0.1; "
        "if a = b then return 1; else return 0; end;")), 0);
}
