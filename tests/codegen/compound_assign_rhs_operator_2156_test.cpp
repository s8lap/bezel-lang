// Codegen fix #2156 — compound assignment `target OP= value` where the
// underlying binary operator is declared on the VALUE's (RHS) type, with a
// primitive LHS (e.g. `n *= s` resolving through `Scale.operator *(int, Scale)`).
//
// Per §25.2 (v0.226): the candidate set for `a <op> b` is "the union of the
// `operator <op>` overloads declared on the static types of BOTH operands,"
// matched in order (no commutation). §6.6 makes `x op= e` equivalent to
// `x := x op e`, so the compound form is well-typed exactly when the resolved
// overload's result stores back. The former compound-assignment codegen
// searched ONLY the target's type; a primitive LHS found no overload and fell
// through to emitCompoundOp, which fed the record struct to
// llvm.smul.with.overflow.i32 → malformed IR (IR verification failed). The
// binary form (`n * s`) already worked because sema's findBinaryOperator
// searches both operand types.
//
// Spec authority: §6.6 (compound assignments), §25.1 (desugar rule),
// §25.2 (restrictions and resolution — both-operand candidate set, in-order).

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}
} // namespace

// §25.2 / §6.6 / #2156: exact issue repro — `n *= s` resolves through
// `Scale.operator *(int, Scale): int` declared on the RHS type. Without the
// fix this aborted codegen ("IR verification failed"); with the fix n becomes
// 42 (6 * 7).
BEZEL_TEST(codegen_fix2156, compound_assign_rhs_operator_2156_times_int_lhs) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Scale(F: int) "
        "  public static method operator *(n: int, s: Scale): int => "
        "    n * s.F; "
        "end Scale; "
        "class Program "
        "  public static method Main(): int "
        "    var n: int := 6; "
        "    var s: Scale := new Scale(7); "
        "    n *= s; "
        "    return n; "
        "  end Main; "
        "end Program;"),
        42);
}

// §25.2 / §6.6 / #2156: the binary form `n * s` and the compound form `n *= s`
// must agree (both resolve the same RHS-declared overload). Pins that the
// compound dispatch produces the same value as the (already-working) binary
// dispatch — the regression that #2156 reports is "binary form works, compound
// form aborts."
BEZEL_TEST(codegen_fix2156, compound_assign_rhs_operator_2156_matches_binary) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Scale(F: int) "
        "  public static method operator *(n: int, s: Scale): int => "
        "    n * s.F; "
        "end Scale; "
        "class Program "
        "  public static method Main(): int "
        "    var n: int := 6; "
        "    var s: Scale := new Scale(7); "
        "    var viaBinary: int := n * s; "
        "    n *= s; "
        "    return viaBinary + n; "
        "  end Main; "
        "end Program;"),
        84);
}

// §25.2 / §6.6 / #2156: `-=` with the operator on the RHS type (breadth across
// the compound family — the same dispatch path serves every token).
BEZEL_TEST(codegen_fix2156, compound_assign_rhs_operator_2156_minus_int_lhs) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Scale(F: int) "
        "  public static method operator -(n: int, s: Scale): int => "
        "    n - s.F; "
        "end Scale; "
        "class Program "
        "  public static method Main(): int "
        "    var n: int := 50; "
        "    var s: Scale := new Scale(8); "
        "    n -= s; "
        "    return n; "
        "  end Main; "
        "end Program;"),
        42);
}

// §25.2 / §6.6 / #2156: a record RHS that overloads BOTH `(int, Scale)` and
// `(Scale, Scale)` — the in-order param match must pick the `(int, Scale)`
// overload for `n *= s` (primitive LHS), not whichever happens to be first in
// the member list. Result is 6 * 7 = 42, NOT the Scale*Scale path.
BEZEL_TEST(codegen_fix2156, compound_assign_rhs_operator_2156_disambiguates_overloads) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Scale(F: int) "
        "  public static method operator *(a: Scale, b: Scale): Scale => "
        "    new Scale(a.F * b.F); "
        "  public static method operator *(n: int, s: Scale): int => "
        "    n * s.F; "
        "end Scale; "
        "class Program "
        "  public static method Main(): int "
        "    var n: int := 6; "
        "    var s: Scale := new Scale(7); "
        "    n *= s; "
        "    return n; "
        "  end Main; "
        "end Program;"),
        42);
}

// §6.6 / regression: primitive-LHS / primitive-RHS `+=` must keep using the
// built-in overflow-checked intrinsic (the user-overload path must not
// intercept primitives). Mirrors the #610 int-regression guard for this file.
BEZEL_TEST(codegen_fix2156, compound_assign_rhs_operator_2156_int_regression) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var x := 40; x += 2; return x; "
        "end Main; end C;"),
        42);
}
