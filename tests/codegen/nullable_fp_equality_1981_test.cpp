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
