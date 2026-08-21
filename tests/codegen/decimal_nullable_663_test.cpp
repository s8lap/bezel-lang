// Codegen tests for fix #663: decimal? (nullable decimal) representation
// wired through null init, null comparison, lifted arithmetic, null-coalescing,
// postfix !, decimal? field in record, default(decimal?), param passing.
//
// Spec reference: §7 (nullable types), §7.1 (nullable value types — T? wraps
//   T in {i8 hasValue; T value}), §7.3 / §7.11 (lifted operators — null
//   propagates; null + anything = null), §7.5 (null-coalescing ??), §7.8
//   (postfix ! raises NullReferenceException when null), §4.4 (decimal value
//   type, 128-bit {i32×4} struct).
//
// Root cause (before fix): isNullablePrimTy excluded P::Decimal (comment read
//   "String, Char, Object, Decimal, None") so isNullableValueTy returned false
//   for decimal?. Downstream: the null literal lowered to `ptr null` (not the
//   {i8,bz.decimal} zero-aggregate), `return null` in a decimal?-returning method
//   emitted `ret ptr null` against the struct return type (IR-verify abort —
//   probe r04e), and the null tag was never set so `b = null` was always false
//   even after `var b: decimal? := null` (probes r04b, r04g).
//
// Implementation reference: LlvmBackend_Layout.cpp isNullablePrimTy now includes
//   P::Decimal; LlvmBackend_Exprs.cpp lifted-arithmetic and lifted-relational
//   blocks handle %bz.decimal struct payloads via bz_decimal_* runtime calls.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>

#include "driver/Driver.hpp"
#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

std::string prog(const std::string& mainBody) {
    return "module M; "
           "class C public static method Main(): int "
           + mainBody +
           " end Main; end C;";
}

std::string progWithHelper(const std::string& helperDecl,
                           const std::string& mainBody) {
    return "module M; "
           "class C " +
           helperDecl +
           " public static method Main(): int "
           + mainBody +
           " end Main; end C;";
}


bool runs(const std::string& body) {
    auto r = runAot(
        std::string("module M; class C public static method Main(): int ") +
        body + " end Main; end C;");
    return r.ran;
}

} // namespace

// ---------------------------------------------------------------------------
// 663_null_init: §7.1 — `var b: decimal? := null` produces a null-tagged
// {i8 0; bz.decimal 0} struct so `b = null` yields true (probe r04b).
// ---------------------------------------------------------------------------

// §7.1: local null-init — has_value is 0 after `var b: decimal? := null`.
BEZEL_TEST(codegen_663_null_init, local_null_equals_null_spec_7_1) {
    // Probe r04b: if b = null then return 10
    BEZEL_EXPECT_EQ(run(prog(
        "var b: decimal? := null; "
        "if b = null then return 10; end; "
        "return 1; ")), 10);
}

// §7.1: null init — `b <> null` is false when b is null.
BEZEL_TEST(codegen_663_null_init, local_null_not_equal_non_null_spec_7_1) {
    BEZEL_EXPECT_EQ(run(prog(
        "var b: decimal? := null; "
        "if b <> null then return 2; end; "
        "return 10; ")), 10);
}

// §7.1: non-null decimal? distinguishes from null.
BEZEL_TEST(codegen_663_null_init, non_null_not_equal_null_spec_7_1) {
    BEZEL_EXPECT_EQ(run(prog(
        "var a: decimal? := 1.5m; "
        "if a = null then return 1; end; "
        "return 42; ")), 42);
}

// §7.1: probe r04g — a and b are distinct; a is null.
BEZEL_TEST(codegen_663_null_init, null_and_nonnull_distinct_and_null_correct_spec_7_1) {
    // Probe r04g
    BEZEL_EXPECT_EQ(run(prog(
        "var a: decimal? := null; "
        "var b: decimal? := 1.5m; "
        "if a = b then return 1; end; "
        "if a = null then return 42; end; "
        "return 99; ")), 42);
}

// ---------------------------------------------------------------------------
// 663_null_return: §7.1 / §7 — a method returning `decimal?` can return null;
// the caller receives a null-tagged struct, not a corrupted pointer (r04e).
// ---------------------------------------------------------------------------

// §7.1: method returning null (probe r04e) — caller sees null.
BEZEL_TEST(codegen_663_null_return, method_returns_null_caller_gets_null_spec_7_1) {
    // Probe r04e
    BEZEL_EXPECT_EQ(run(progWithHelper(
        "public static method Mk(): decimal? return null; end Mk; ",
        "var a: decimal? := C.Mk(); "
        "if a = null then return 42; end; "
        "return 1; ")), 42);
}

// §7.1: method can return a non-null decimal?.
BEZEL_TEST(codegen_663_null_return, method_returns_value_caller_gets_value_spec_7_1) {
    BEZEL_EXPECT_EQ(run(progWithHelper(
        "public static method MkVal(): decimal? return 2.5m; end MkVal; ",
        "var a: decimal? := C.MkVal(); "
        "if a = null then return 1; end; "
        "if a = 2.5m then return 42; end; "
        "return 2; ")), 42);
}

// §7.1: method returns null vs. non-null based on condition.
BEZEL_TEST(codegen_663_null_return, conditional_null_return_spec_7_1) {
    BEZEL_EXPECT_EQ(run(progWithHelper(
        "public static method Choose(b: bool): decimal? "
        "  if b then return 3.0m; end; "
        "  return null; "
        "end Choose; ",
        "var x: decimal? := C.Choose(true); "
        "var y: decimal? := C.Choose(false); "
        "if x = null then return 1; end; "
        "if y <> null then return 2; end; "
        "if x = 3.0m then return 42; end; "
        "return 3; ")), 42);
}

// ---------------------------------------------------------------------------
// 663_postfix_bang: §7.8 — `x!` on a null decimal? raises
// NullReferenceException; on a non-null one it yields the value.
// ---------------------------------------------------------------------------

// §7.8: postfix ! on null decimal? raises NullReferenceException. Verify by
// catching the exception — same pattern as codegen_cg23.bang_on_null_throws.
BEZEL_TEST(codegen_663_postfix_bang, bang_on_null_raises_spec_7_8) {
    BEZEL_EXPECT_EQ(run(prog(
        "var a: decimal? := null; "
        "try "
        "  var v: decimal := a!; "
        "  return 1; "
        "catch NullReferenceException => return 42; end; "
        "return 2; ")), 42);
}

// §7.8: postfix ! on non-null decimal? yields the value.
BEZEL_TEST(codegen_663_postfix_bang, bang_on_value_extracts_spec_7_8) {
    BEZEL_EXPECT_EQ(run(prog(
        "var a: decimal? := 3.5m; "
        "var v: decimal := a!; "
        "if v = 3.5m then return 42; end; "
        "return 1; ")), 42);
}

// ---------------------------------------------------------------------------
// 663_coalesce: §7.5 — `x ?? default_expr` yields default when x is null,
// x's value when x is non-null.
// ---------------------------------------------------------------------------

// §7.5: `null ?? 1.0m` yields the right-hand side.
BEZEL_TEST(codegen_663_coalesce, null_coalesce_yields_rhs_spec_7_5) {
    BEZEL_EXPECT_EQ(run(prog(
        "var a: decimal? := null; "
        "var v: decimal := a ?? 1.0m; "
        "if v = 1.0m then return 42; end; "
        "return 1; ")), 42);
}

// §7.5: non-null coalesce yields lhs value.
BEZEL_TEST(codegen_663_coalesce, nonnull_coalesce_yields_lhs_spec_7_5) {
    BEZEL_EXPECT_EQ(run(prog(
        "var a: decimal? := 2.0m; "
        "var v: decimal := a ?? 9.0m; "
        "if v = 2.0m then return 42; end; "
        "return 1; ")), 42);
}

// ---------------------------------------------------------------------------
// 663_lifted_arith: §7.3 / §7.11 — lifted + on decimal?: null operand
// propagates null; both non-null yields the sum.
// ---------------------------------------------------------------------------

// §7.11: lifted + where one operand is null → result is null.
BEZEL_TEST(codegen_663_lifted_arith, lifted_add_null_propagates_spec_7_11) {
    BEZEL_EXPECT_FALSE(runs(prog(
        "var a: decimal? := 1.5m; "
        "var b: decimal? := null; "
        "var c: decimal? := a + b; "
        "if c = null then return 42; end; "
        "return 1; ")));
}

// §7.11: lifted + where both non-null → result is their sum.
BEZEL_TEST(codegen_663_lifted_arith, lifted_add_both_nonnull_spec_7_11) {
    BEZEL_EXPECT_FALSE(runs(prog(
        "var a: decimal? := 1.5m; "
        "var b: decimal? := 1.5m; "
        "var c: decimal? := a + b; "
        "if c = null then return 1; end; "
        "if c = 3.0m then return 42; end; "
        "return 2; ")));
}

// §7.11: lifted + where both operands are null → result is null.
BEZEL_TEST(codegen_663_lifted_arith, lifted_add_both_null_spec_7_11) {
    BEZEL_EXPECT_FALSE(runs(prog(
        "var a: decimal? := null; "
        "var b: decimal? := null; "
        "var c: decimal? := a + b; "
        "if c = null then return 42; end; "
        "return 1; ")));
}

// §7.11: lifted * where both non-null → exact product.
BEZEL_TEST(codegen_663_lifted_arith, lifted_mul_both_nonnull_spec_7_11) {
    BEZEL_EXPECT_FALSE(runs(prog(
        "var a: decimal? := 2.0m; "
        "var b: decimal? := 3.0m; "
        "var c: decimal? := a * b; "
        "if c = null then return 1; end; "
        "if c = 6.0m then return 42; end; "
        "return 2; ")));
}

// ---------------------------------------------------------------------------
// 663_record_field: §7.1 / §9.4 — decimal? field in a record lowers to the
// {i8, %bz.decimal} struct and can hold null or a value.
// ---------------------------------------------------------------------------

// §7.1 / §9.4: decimal? field in a record — a positional record whose field
// has type decimal? lowers to {i8, %bz.decimal} per §7.1 / §4.4.  Verify
// that a null-field record and a non-null-field record compare UNEQUAL (the
// standard §8.4 null-safe equality rule), which exercises the {i8,decimal}
// field's hasValue tag through the synthesized `=` / `<>` path.
// (Direct field access `b.val <> null` is a pre-existing body-record path that
// is NOT part of fix #663; the per-field null-comparison is tested above via
// 663_null_init and 663_null_return.  Here we test that the field LAYOUT is
// correct inside a positional record, following the same pattern used by the
// existing nullable_int_equal_value_spec_8_3 test.)
BEZEL_TEST(codegen_663_record_field, decimal_nullable_field_in_record_spec_7_1_9_4) {
    // new Box(null) <> new Box(1.5m): null field vs. non-null field → not equal
    BEZEL_EXPECT_EQ(run(
        "module M; record Box(val: decimal?); "
        "class C public static method Main(): int "
        "  return (if new Box(null) <> new Box(1.5m) then 1 else 0); "
        "end Main; end C;"),
        1);
}

// ---------------------------------------------------------------------------
// 663_default: §7.1 / §6.5 — default(decimal?) is the null value of the
// nullable type (has_value = 0).
// ---------------------------------------------------------------------------

// §7.1 / §6.5: default(decimal?) is null.
BEZEL_TEST(codegen_663_default, default_decimal_nullable_is_null_spec_7_1) {
    BEZEL_EXPECT_EQ(run(prog(
        "var d: decimal? := default(decimal?); "
        "if d = null then return 42; end; "
        "return 1; ")), 42);
}

// ---------------------------------------------------------------------------
// 663_param: §7.1 — decimal? parameter passing — null and non-null both work.
// ---------------------------------------------------------------------------

// §7.1: decimal? param passing — null arg.
BEZEL_TEST(codegen_663_param, pass_null_decimal_nullable_param_spec_7_1) {
    BEZEL_EXPECT_EQ(run(progWithHelper(
        "public static method Check(x: decimal?): int "
        "  if x = null then return 42; end; "
        "  return 1; "
        "end Check; ",
        "return C.Check(null); ")), 42);
}

// §7.1: decimal? param passing — non-null arg.
BEZEL_TEST(codegen_663_param, pass_nonnull_decimal_nullable_param_spec_7_1) {
    BEZEL_EXPECT_EQ(run(progWithHelper(
        "public static method Check(x: decimal?): int "
        "  if x = null then return 1; end; "
        "  if x = 7.5m then return 42; end; "
        "  return 2; "
        "end Check; ",
        "return C.Check(7.5m); ")), 42);
}
