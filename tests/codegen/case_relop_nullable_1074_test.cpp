// Regression tests for #1074 (§19.3): a relational pattern (`> 3`, `<= n`, …)
// in a `case` over a NULLABLE value-type scrutinee. The nullable-scrutinee
// lowering in patternMatches (LlvmBackend_Exprs.cpp) handled null / constant /
// named / binding / range / destructuring patterns but had no RelOpPattern
// case, so a relational arm fell through to the deferred `return nullptr` —
// lowerCaseExpr then bailed leaving `case.end` with no terminator, and LLVM IR
// verification aborted the compile. The fix runs the relational comparison on
// the unwrapped value and ANDs it with hasValue: a null scrutinee never matches
// a relational pattern; a present value matches iff the comparison holds.
//
// runJit returns the raw exit code (0..255).

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}
std::string caseProg(const std::string& init, const std::string& arm) {
    return std::string("module M; class C public static method Main(): int "
                       "  var n: int? := ") + init + "; "
           "  var r := case n " + arm + ", _ => 2 end; "
           "  return r; end Main; end C;";
}
}  // namespace

// The #1074 repro: a present value satisfying the relational pattern matches.
BEZEL_TEST(case_relop_nullable_1074, present_value_matches_gt) {
    BEZEL_EXPECT_EQ(run(caseProg("5", "> 3 => 1")), 1);   // 5 > 3 → arm 1
}

// A null scrutinee never matches a relational pattern → falls to `_`.
BEZEL_TEST(case_relop_nullable_1074, null_scrutinee_falls_through) {
    BEZEL_EXPECT_EQ(run(caseProg("null", "> 3 => 1")), 2);  // null → `_`
}

// Boundary: 3 is not > 3 → `_`.
BEZEL_TEST(case_relop_nullable_1074, present_value_below_boundary_gt) {
    BEZEL_EXPECT_EQ(run(caseProg("3", "> 3 => 1")), 2);
}

// `<=` matches at the boundary.
BEZEL_TEST(case_relop_nullable_1074, present_value_matches_le_boundary) {
    BEZEL_EXPECT_EQ(run(caseProg("3", "<= 3 => 8")), 8);
}

// `<` and `>=` lower correctly too.
BEZEL_TEST(case_relop_nullable_1074, lt_and_ge_forms) {
    BEZEL_EXPECT_EQ(run(caseProg("2", "< 5 => 4")), 4);    // 2 < 5
    BEZEL_EXPECT_EQ(run(caseProg("7", ">= 7 => 6")), 6);   // 7 >= 7
}

// A relational arm first, then a constant arm on the same nullable scrutinee —
// both pattern kinds coexist and lower without leaving an unterminated block.
BEZEL_TEST(case_relop_nullable_1074, relational_then_constant_arm) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var n: int? := 10; "
        "  var r := case n > 100 => 1, 10 => 5, _ => 2 end; "
        "  return r; end Main; end C;"), 5);  // 10 not >100; matches constant 10
}

// ---------------------------------------------------------------------------
// #1113 (§7.11 / §8.4): a NARROW nullable (`byte?`/`short?`/`sbyte?`) compared
// to an int LITERAL (which lowers at i32) crashed the compiler — the §8.4
// null-safe equality ICmp'd the i8/i16 payload against the i32 literal (an LLVM
// same-type assert). The fix widens the narrower operand to the wider (each by
// its own signedness), mirroring the non-nullable comparison — NOT truncating
// the literal (which would wrap an out-of-range value into the payload range).
// ---------------------------------------------------------------------------

// byte? = <int-literal>: present value equal (was a compiler crash).
BEZEL_TEST(case_relop_nullable_1074, narrow_nullable_eq_int_literal_1113) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: byte? := 5; return (1 if a = 5 else 0); end Main; end C;"), 1);
}

// byte? <> <int-literal>: present value not-equal (was a compiler crash).
BEZEL_TEST(case_relop_nullable_1074, narrow_nullable_ne_int_literal_1113) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: byte? := 5; return (1 if a <> 6 else 0); end Main; end C;"), 1);
}

// Out-of-range literal: `byte?(5) = 261` — 261 & 0xFF == 5, so a TRUNCATING fix
// would wrongly report equal. Widening compares 5 vs 261 → NOT equal.
BEZEL_TEST(case_relop_nullable_1074, narrow_nullable_eq_out_of_range_literal_1113) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: byte? := 5; return (1 if a = 261 else 0); end Main; end C;"), 0);
}

// sbyte? signed: the payload sign-extends so a negative literal compares by value.
BEZEL_TEST(case_relop_nullable_1074, narrow_signed_nullable_eq_negative_literal_1113) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: sbyte? := -5; return (1 if a = -5 else 0); end Main; end C;"), 1);
}

// short? = <int-literal> with the literal LHS (operand order swapped).
BEZEL_TEST(case_relop_nullable_1074, narrow_nullable_eq_literal_lhs_1113) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: short? := 300; return (1 if 300 = a else 0); end Main; end C;"), 1);
}

// null narrow-nullable = literal → not equal (null is never equal to a value).
BEZEL_TEST(case_relop_nullable_1074, null_narrow_nullable_eq_literal_1113) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: byte? := null; return (1 if a = 5 else 0); end Main; end C;"), 0);
}
