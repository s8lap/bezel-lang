// §7.11 Lifted operators — codegen tests for nullable value type arithmetic,
// relational, and unary operators (issue #486).
//
// §7.11 rules exercised:
//   - Arithmetic/bitwise/shift (`+`, `-`, `*`, `/`, `mod`, `&`, `|`, `^`,
//     `<<`, `>>`): result is `T?`; null if EITHER operand is null; the
//     underlying op (incl. its OverflowException / DivideByZeroException check)
//     runs ONLY when BOTH operands are present.
//   - Unary `-` / `~` on `T?`: result `T?`; null iff operand null; overflow
//     check for `-` runs only when operand is present.
//   - Relational (`<`, `<=`, `>`, `>=`): result `bool` (NOT `bool?`); false
//     if EITHER operand is null; correct comparison when both present.
//   - Mixed `T?` + `T`: result `T?` (the non-nullable operand is treated as
//     always present).
//   - Narrowing required: lifted arithmetic result is `T?`; coalescing with
//     `??` narrows to non-nullable.
//   - Control: non-nullable int/double arithmetic + relational unchanged.
//   - Equality (`=`, `<>`) unchanged (already null-safe per §8.4).

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

// Helper: run a Bezel program that returns an int exit code.
int run(const std::string& body) {
    auto r = runJit(
        std::string("module M; class C public static method Main(): int ") +
        body + " end Main; end C;");
    return r.ran ? r.exitCode : -999;
}

} // namespace


// =========================================================================
// §7.11 — int? arithmetic: both present
// =========================================================================

// §7.11: a + b where a: int? = 3, b: int? = 4 → 7 (present, unwrap via ??).
BEZEL_TEST(codegen_lifted_ops_486, int_add_both_present_spec_7_11) {
    BEZEL_EXPECT_EQ(run(
        "var a: int? := 3; var b: int? := 4; "
        "var r: int? := a + b; "
        "return (r ?? -1);"), 7);
}

// §7.11: subtraction both present.
BEZEL_TEST(codegen_lifted_ops_486, int_sub_both_present_spec_7_11) {
    BEZEL_EXPECT_EQ(run(
        "var a: int? := 10; var b: int? := 3; "
        "var r: int? := a - b; "
        "return (r ?? -1);"), 7);
}

// §7.11: multiplication both present.
BEZEL_TEST(codegen_lifted_ops_486, int_mul_both_present_spec_7_11) {
    BEZEL_EXPECT_EQ(run(
        "var a: int? := 6; var b: int? := 7; "
        "var r: int? := a * b; "
        "return (r ?? -1);"), 42);
}

// §7.11: division both present.
BEZEL_TEST(codegen_lifted_ops_486, int_div_both_present_spec_7_11) {
    BEZEL_EXPECT_EQ(run(
        "var a: int? := 20; var b: int? := 4; "
        "var r: int? := a / b; "
        "return (r ?? -1);"), 5);
}

// §7.11: mod both present.
BEZEL_TEST(codegen_lifted_ops_486, int_mod_both_present_spec_7_11) {
    BEZEL_EXPECT_EQ(run(
        "var a: int? := 17; var b: int? := 5; "
        "var r: int? := a mod b; "
        "return (r ?? -1);"), 2);
}


// =========================================================================
// §7.11 — int? arithmetic: either operand null → null result
// =========================================================================

// §7.11: a null, b present → result null.
BEZEL_TEST(codegen_lifted_ops_486, int_add_left_null_yields_null_spec_7_11) {
    BEZEL_EXPECT_EQ(run(
        "var a: int? := null; var b: int? := 4; "
        "var r: int? := a + b; "
        "return int(r = null);"), 1);
}

// §7.11: a present, b null → result null.
BEZEL_TEST(codegen_lifted_ops_486, int_add_right_null_yields_null_spec_7_11) {
    BEZEL_EXPECT_EQ(run(
        "var a: int? := 3; var b: int? := null; "
        "var r: int? := a + b; "
        "return int(r = null);"), 1);
}

// §7.11: both null → result null.
BEZEL_TEST(codegen_lifted_ops_486, int_add_both_null_yields_null_spec_7_11) {
    BEZEL_EXPECT_EQ(run(
        "var a: int? := null; var b: int? := null; "
        "var r: int? := a + b; "
        "return int(r = null);"), 1);
}


// =========================================================================
// §7.11 — DivideByZeroException fires only when BOTH operands are present
// =========================================================================

// §7.11: null / 0 → null result, NO exception (the division never executes).
BEZEL_TEST(codegen_lifted_ops_486, int_div_null_divisor_zero_no_exception_spec_7_11) {
    BEZEL_EXPECT_EQ(run(
        "var a: int? := null; var b: int? := 0; "
        "var r: int? := a / b; "
        "return int(r = null);"), 1);
}

// §7.11: 10 / null → null result, NO exception.
BEZEL_TEST(codegen_lifted_ops_486, int_div_null_dividend_no_exception_spec_7_11) {
    BEZEL_EXPECT_EQ(run(
        "var a: int? := 10; var b: int? := null; "
        "var r: int? := a / b; "
        "return int(r = null);"), 1);
}

// §7.11: 10 / 0 (both present) → DivideByZeroException fires.
BEZEL_TEST(codegen_lifted_ops_486, int_div_both_present_zero_raises_spec_7_11) {
    BEZEL_EXPECT_EQ(run(
        "var a: int? := 10; var b: int? := 0; "
        "try var r: int? := a / b; return 0; "
        "catch DivideByZeroException => return 1; end;"), 1);
}

// §7.11: null mod 0 → null, NO exception.
BEZEL_TEST(codegen_lifted_ops_486, int_mod_null_no_exception_spec_7_11) {
    BEZEL_EXPECT_EQ(run(
        "var a: int? := null; var b: int? := 0; "
        "var r: int? := a mod b; "
        "return int(r = null);"), 1);
}


// =========================================================================
// §7.11 — int? relational: result bool (not bool?)
// =========================================================================

// §7.11: both present, a < b → true.
BEZEL_TEST(codegen_lifted_ops_486, int_lt_both_present_true_spec_7_11) {
    BEZEL_EXPECT_EQ(run(
        "var a: int? := 3; var b: int? := 7; "
        "return int(a < b);"), 1);
}

// §7.11: both present, a < b false.
BEZEL_TEST(codegen_lifted_ops_486, int_lt_both_present_false_spec_7_11) {
    BEZEL_EXPECT_EQ(run(
        "var a: int? := 7; var b: int? := 3; "
        "return int(a < b);"), 0);
}

// §7.11: left null → false (regardless of right).
BEZEL_TEST(codegen_lifted_ops_486, int_lt_left_null_yields_false_spec_7_11) {
    BEZEL_EXPECT_EQ(run(
        "var a: int? := null; var b: int? := 3; "
        "return int(a < b);"), 0);
}

// §7.11: right null → false.
BEZEL_TEST(codegen_lifted_ops_486, int_lt_right_null_yields_false_spec_7_11) {
    BEZEL_EXPECT_EQ(run(
        "var a: int? := 3; var b: int? := null; "
        "return int(a < b);"), 0);
}

// §7.11: both null → false (null is not "less than" anything).
BEZEL_TEST(codegen_lifted_ops_486, int_lt_both_null_yields_false_spec_7_11) {
    BEZEL_EXPECT_EQ(run(
        "var a: int? := null; var b: int? := null; "
        "return int(a < b);"), 0);
}

// §7.11: <= both present, equal → true.
BEZEL_TEST(codegen_lifted_ops_486, int_le_both_present_equal_spec_7_11) {
    BEZEL_EXPECT_EQ(run(
        "var a: int? := 5; var b: int? := 5; "
        "return int(a <= b);"), 1);
}

// §7.11: <= left null → false.
BEZEL_TEST(codegen_lifted_ops_486, int_le_left_null_yields_false_spec_7_11) {
    BEZEL_EXPECT_EQ(run(
        "var a: int? := null; var b: int? := 5; "
        "return int(a <= b);"), 0);
}

// §7.11: > both present, true.
BEZEL_TEST(codegen_lifted_ops_486, int_gt_both_present_spec_7_11) {
    BEZEL_EXPECT_EQ(run(
        "var a: int? := 9; var b: int? := 3; "
        "return int(a > b);"), 1);
}

// §7.11: > right null → false.
BEZEL_TEST(codegen_lifted_ops_486, int_gt_right_null_yields_false_spec_7_11) {
    BEZEL_EXPECT_EQ(run(
        "var a: int? := 9; var b: int? := null; "
        "return int(a > b);"), 0);
}

// §7.11: >= both present equal → true.
BEZEL_TEST(codegen_lifted_ops_486, int_ge_both_equal_spec_7_11) {
    BEZEL_EXPECT_EQ(run(
        "var a: int? := 4; var b: int? := 4; "
        "return int(a >= b);"), 1);
}

// §7.11: >= left null → false.
BEZEL_TEST(codegen_lifted_ops_486, int_ge_left_null_yields_false_spec_7_11) {
    BEZEL_EXPECT_EQ(run(
        "var a: int? := null; var b: int? := 4; "
        "return int(a >= b);"), 0);
}


// =========================================================================
// §7.11 — unary `-` and `~` on int?
// =========================================================================

// §7.11: unary `-` on present int? → negated value (narrow with ??).
BEZEL_TEST(codegen_lifted_ops_486, int_unary_neg_present_spec_7_11) {
    BEZEL_EXPECT_EQ(run(
        "var a: int? := 5; "
        "var r: int? := -a; "
        "return (r ?? -99);"), -5);
}

// §7.11: unary `-` on null int? → null.
BEZEL_TEST(codegen_lifted_ops_486, int_unary_neg_null_yields_null_spec_7_11) {
    BEZEL_EXPECT_EQ(run(
        "var a: int? := null; "
        "var r: int? := -a; "
        "return int(r = null);"), 1);
}

// §7.11: unary `~` on present int? → bitwise complement (~0 == -1).
BEZEL_TEST(codegen_lifted_ops_486, int_unary_bitnot_present_spec_7_11) {
    BEZEL_EXPECT_EQ(run(
        "var a: int? := 0; "
        "var r: int? := ~a; "
        "return (r ?? 0);"), -1);
}

// §7.11: unary `~` on null int? → null.
BEZEL_TEST(codegen_lifted_ops_486, int_unary_bitnot_null_yields_null_spec_7_11) {
    BEZEL_EXPECT_EQ(run(
        "var a: int? := null; "
        "var r: int? := ~a; "
        "return int(r = null);"), 1);
}


// =========================================================================
// §7.11 — Mixed T? + T (non-nullable operand treated as always present)
// =========================================================================

// §7.11: int? + int (non-nullable treated as present) → int?.
BEZEL_TEST(codegen_lifted_ops_486, int_mixed_nullable_plus_nonnull_spec_7_11) {
    BEZEL_EXPECT_EQ(run(
        "var a: int? := 3; var b: int := 4; "
        "var r: int? := a + b; "
        "return (r ?? -1);"), 7);
}

// §7.11: null int? + non-null int → null.
BEZEL_TEST(codegen_lifted_ops_486, int_mixed_null_plus_nonnull_spec_7_11) {
    BEZEL_EXPECT_EQ(run(
        "var a: int? := null; var b: int := 4; "
        "var r: int? := a + b; "
        "return int(r = null);"), 1);
}

// §7.11: int + int? (other side), right is null → null.
BEZEL_TEST(codegen_lifted_ops_486, int_mixed_nonnull_plus_null_spec_7_11) {
    BEZEL_EXPECT_EQ(run(
        "var a: int := 3; var b: int? := null; "
        "var r: int? := a + b; "
        "return int(r = null);"), 1);
}

// §7.11: int + int? (other side), right is present → sum.
BEZEL_TEST(codegen_lifted_ops_486, int_mixed_nonnull_plus_present_spec_7_11) {
    BEZEL_EXPECT_EQ(run(
        "var a: int := 3; var b: int? := 4; "
        "var r: int? := a + b; "
        "return (r ?? -1);"), 7);
}


// =========================================================================
// §7.11 — Narrowing required: result is T?, coalesce to get non-nullable
// =========================================================================

// §7.11: `(a + b) ?? 0` narrows from int? → int correctly.
BEZEL_TEST(codegen_lifted_ops_486, int_add_coalesced_to_nonnull_spec_7_11) {
    BEZEL_EXPECT_EQ(run(
        "var a: int? := 10; var b: int? := 5; "
        "var r: int := (a + b) ?? 0; "
        "return r;"), 15);
}

// §7.11: null + null coalesced to default.
BEZEL_TEST(codegen_lifted_ops_486, int_add_null_coalesced_default_spec_7_11) {
    BEZEL_EXPECT_EQ(run(
        "var a: int? := null; var b: int? := null; "
        "var r: int := (a + b) ?? 99; "
        "return r;"), 99);
}


// =========================================================================
// §7.11 — double? arithmetic and relational
// =========================================================================

// §7.11: double? + double? both present.
BEZEL_TEST(codegen_lifted_ops_486, double_add_both_present_spec_7_11) {
    // 1.5 + 2.5 = 4.0 → int cast → 4.
    BEZEL_EXPECT_EQ(run(
        "var a: double? := 1.5; var b: double? := 2.5; "
        "var r: double? := a + b; "
        "return int(r ?? 0.0);"), 4);
}

// §7.11: double? + null → null.
BEZEL_TEST(codegen_lifted_ops_486, double_add_null_yields_null_spec_7_11) {
    BEZEL_EXPECT_EQ(run(
        "var a: double? := 1.5; var b: double? := null; "
        "var r: double? := a + b; "
        "return int(r = null);"), 1);
}

// §7.11: double? < double? both present (1.0 < 2.0).
BEZEL_TEST(codegen_lifted_ops_486, double_lt_both_present_spec_7_11) {
    BEZEL_EXPECT_EQ(run(
        "var a: double? := 1.0; var b: double? := 2.0; "
        "return int(a < b);"), 1);
}

// §7.11: double? < null → false.
BEZEL_TEST(codegen_lifted_ops_486, double_lt_null_yields_false_spec_7_11) {
    BEZEL_EXPECT_EQ(run(
        "var a: double? := 1.0; var b: double? := null; "
        "return int(a < b);"), 0);
}

// §7.11: null double? < double? → false.
BEZEL_TEST(codegen_lifted_ops_486, double_lt_left_null_yields_false_spec_7_11) {
    BEZEL_EXPECT_EQ(run(
        "var a: double? := null; var b: double? := 1.0; "
        "return int(a < b);"), 0);
}

// §7.11 + IEEE 754: both operands present but one is NaN. NaN is unordered, so
// the ordered relational comparison yields false for every ordering operator —
// `<`, `<=`, `>`, `>=` all return false. (This matches the non-nullable double
// comparison: lifting does not change the IEEE ordering semantics; it only adds
// the "false if either operand is null" rule on top.) `double.NaN` per stdlib
// §2.8. We pack all four results into the exit code; every operator → 0.
BEZEL_TEST(codegen_lifted_ops_486, double_relational_nan_both_present_false_spec_7_11) {
    BEZEL_EXPECT_EQ(run(
        "var a: double? := double.NaN; var b: double? := 1.0; "
        "var lt: int := int(a < b); var gt: int := int(a > b); "
        "var le: int := int(a <= b); var ge: int := int(a >= b); "
        "return lt * 1000 + gt * 100 + le * 10 + ge;"), 0);
}

// §7.11 + IEEE 754: NaN on the right operand — same all-false outcome.
BEZEL_TEST(codegen_lifted_ops_486, double_relational_nan_right_false_spec_7_11) {
    BEZEL_EXPECT_EQ(run(
        "var a: double? := 1.0; var b: double? := double.NaN; "
        "return int(a < b) + int(a > b) + int(a <= b) + int(a >= b);"), 0);
}


// =========================================================================
// §7.11 — long? arithmetic
// =========================================================================

// §7.11: long? + long? both present.
BEZEL_TEST(codegen_lifted_ops_486, long_add_both_present_spec_7_11) {
    BEZEL_EXPECT_EQ(run(
        "var a: long? := long(3); var b: long? := long(4); "
        "var r: long? := a + b; "
        "return int(r ?? long(-1));"), 7);
}

// §7.11: long? null → null.
BEZEL_TEST(codegen_lifted_ops_486, long_add_null_yields_null_spec_7_11) {
    BEZEL_EXPECT_EQ(run(
        "var a: long? := null; var b: long? := long(4); "
        "var r: long? := a + b; "
        "return int(r = null);"), 1);
}

// §7.11: long? relational both present.
BEZEL_TEST(codegen_lifted_ops_486, long_lt_both_present_spec_7_11) {
    BEZEL_EXPECT_EQ(run(
        "var a: long? := long(1); var b: long? := long(2); "
        "return int(a < b);"), 1);
}

// §7.11: long? relational null → false.
BEZEL_TEST(codegen_lifted_ops_486, long_lt_null_yields_false_spec_7_11) {
    BEZEL_EXPECT_EQ(run(
        "var a: long? := null; var b: long? := long(2); "
        "return int(a < b);"), 0);
}


// =========================================================================
// §7.11 — bool? bitwise (& and |): treated as integer ops on i1
// =========================================================================

// §7.11: bool? & bool? both true → true.
BEZEL_TEST(codegen_lifted_ops_486, bool_bitand_both_present_true_spec_7_11) {
    BEZEL_EXPECT_EQ(run(
        "var a: bool? := true; var b: bool? := true; "
        "var r: bool? := a & b; "
        "return int(r ?? false);"), 1);
}

// §7.11: bool? & bool? one false → false.
BEZEL_TEST(codegen_lifted_ops_486, bool_bitand_one_false_spec_7_11) {
    BEZEL_EXPECT_EQ(run(
        "var a: bool? := true; var b: bool? := false; "
        "var r: bool? := a & b; "
        "return int(r ?? true);"), 0);
}

// §7.11: bool? & null → null.
BEZEL_TEST(codegen_lifted_ops_486, bool_bitand_null_yields_null_spec_7_11) {
    BEZEL_EXPECT_EQ(run(
        "var a: bool? := true; var b: bool? := null; "
        "var r: bool? := a & b; "
        "return int(r = null);"), 1);
}


// =========================================================================
// §7.11 — CONTROL: non-nullable int arithmetic unchanged
// =========================================================================

// §7.11 (control): plain int + int still works.
BEZEL_TEST(codegen_lifted_ops_486, nonnull_int_add_unchanged_control) {
    BEZEL_EXPECT_EQ(run("return 3 + 4;"), 7);
}

// §7.11 (control): plain int relational still works.
BEZEL_TEST(codegen_lifted_ops_486, nonnull_int_lt_unchanged_control) {
    BEZEL_EXPECT_EQ(run("return int(3 < 7);"), 1);
}

// §7.11 (control): plain double relational unchanged.
BEZEL_TEST(codegen_lifted_ops_486, nonnull_double_lt_unchanged_control) {
    BEZEL_EXPECT_EQ(run("return int(1.0 < 2.0);"), 1);
}


// =========================================================================
// §7.11 — Equality (= / <>) is UNCHANGED (§8.4, already null-safe)
// =========================================================================

// §8.4 (control): int? = null still works.
BEZEL_TEST(codegen_lifted_ops_486, int_eq_null_unchanged_spec_8_4) {
    BEZEL_EXPECT_EQ(run(
        "var a: int? := null; return int(a = null);"), 1);
}

// §8.4 (control): int? <> null still works for non-null value.
BEZEL_TEST(codegen_lifted_ops_486, int_neq_null_unchanged_spec_8_4) {
    BEZEL_EXPECT_EQ(run(
        "var a: int? := 5; return int(a <> null);"), 1);
}

// §8.4 (control): int? = int? (both non-null, equal).
BEZEL_TEST(codegen_lifted_ops_486, int_eq_both_nonnull_unchanged_spec_8_4) {
    BEZEL_EXPECT_EQ(run(
        "var a: int? := 5; var b: int? := 5; return int(a = b);"), 1);
}
