// Codegen slice CG4 — comparisons (= <> < > <= >=), bool literals,
// logical and/or/not, and the conditional expression `X if C else Y`
// (§20, §17.1). Observable via the int exit code: the conditional
// selects an int branch based on a bool condition.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runJit;

namespace {

int run(const std::string& expr) {
    auto r = runJit(std::string("module M; class C "
        "public static method Main(): int => ") + expr + "; end C;");
    return r.ran ? r.exitCode : -999;
}

int runProg(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}

} // namespace

BEZEL_TEST(codegen_cg4, less_than_true_branch) {
    BEZEL_EXPECT_EQ(run("5 if 1 < 2 else 9"), 5);
}

BEZEL_TEST(codegen_cg4, less_than_false_branch) {
    BEZEL_EXPECT_EQ(run("5 if 2 < 1 else 9"), 9);
}

BEZEL_TEST(codegen_cg4, equality) {
    BEZEL_EXPECT_EQ(run("1 if 3 = 3 else 0"), 1);
}

BEZEL_TEST(codegen_cg4, inequality) {
    BEZEL_EXPECT_EQ(run("1 if 3 <> 3 else 0"), 0);
}

BEZEL_TEST(codegen_cg4, ge_le) {
    BEZEL_EXPECT_EQ(run("7 if (4 >= 4) else 8"), 7);
}

BEZEL_TEST(codegen_cg4, bool_literal_condition) {
    BEZEL_EXPECT_EQ(run("1 if true else 2"), 1);
    BEZEL_EXPECT_EQ(run("1 if false else 2"), 2);
}

BEZEL_TEST(codegen_cg4, logical_and) {
    BEZEL_EXPECT_EQ(run("1 if (1 < 2) and (3 < 4) else 0"), 1);
    BEZEL_EXPECT_EQ(run("1 if (1 < 2) and (4 < 3) else 0"), 0);
}

BEZEL_TEST(codegen_cg4, logical_or) {
    BEZEL_EXPECT_EQ(run("1 if (2 < 1) or (3 < 4) else 0"), 1);
}

BEZEL_TEST(codegen_cg4, logical_not) {
    BEZEL_EXPECT_EQ(run("1 if not (2 < 1) else 0"), 1);
}

BEZEL_TEST(codegen_cg4, nested_conditional) {
    BEZEL_EXPECT_EQ(run("1 if 1 > 2 else (2 if 3 > 4 else 3)"), 3);
}

BEZEL_TEST(codegen_cg4, conditional_branches_compute) {
    BEZEL_EXPECT_EQ(run("(10 * 2) if 1 < 2 else (10 + 2)"), 20);
}

// --- §20 + §17.12: a conditional with a diverging (`throw`) branch. ---
// `x if c else throw E` parses with the throw as the else branch (the
// throw's own operand is `E`; §17.12). The branch is bottom-typed, so
// the conditional's type is the non-diverging branch's type and codegen
// must not include the diverging branch in the join PHI.

// True direction: the value branch is selected; no exception is raised.
BEZEL_TEST(codegen_cg4, conditional_throw_else_arm_value_path) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; class C "
        "  public static method Main(): int => "
        "    42 if 1 < 2 else throw new Exception(\"boom\"); "
        "end C;"), 42);
}

// False direction: the else branch's `throw` fires; a caller's try/catch
// catches it. Exercises the diverging-branch lowering at run time.
BEZEL_TEST(codegen_cg4, conditional_throw_else_arm_throw_path) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; class C "
        "  public static method Helper(c: bool): int => "
        "    42 if c else throw new Exception(\"boom\"); "
        "  public static method Main(): int "
        "    try return Helper(false); catch Exception => return 7; end; "
        "    return 0; "
        "  end Main; "
        "end C;"), 7);
}

// The `return (x if c else throw E)` form (throw in the else branch of a
// returned conditional), value path.
BEZEL_TEST(codegen_cg4, return_conditional_throw_else_value_path) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; class C "
        "  public static method Main(): int "
        "    return 5 if 2 > 1 else throw new Exception(\"boom\"); "
        "  end Main; "
        "end C;"), 5);
}

// Mirror: the THEN branch diverges (parenthesized so the throw doesn't
// swallow the whole conditional). Value comes from the else branch.
BEZEL_TEST(codegen_cg4, conditional_throw_then_arm_value_path) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; class C "
        "  public static method Main(): int => "
        "    (throw new Exception(\"boom\")) if 1 > 2 else 9; "
        "end C;"), 9);
}

// --- §20: branches of differing-but-common numeric type. ---
// `5 if c else 9000000000L` is an int branch and a long branch; the §20
// common type is `long`. Codegen must coerce the int branch to i64 before
// the join PHI (else an i32/i64 PHI mismatch crashes LLVM). Verify the
// long value survives (a truncating i32 PHI would lose the high bits).
BEZEL_TEST(codegen_cg4, conditional_numeric_widening_to_long) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; class C "
        "  public static method Main(): int "
        "    var hi: long := 5 if 1 > 2 else 9000000000L; "
        "    return 1 if hi = 9000000000L else 0; "
        "  end Main; "
        "end C;"), 1);
}

// The selected-int direction of the same widened conditional still yields
// the int value (widened to long).
BEZEL_TEST(codegen_cg4, conditional_numeric_widening_int_branch) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; class C "
        "  public static method Main(): int "
        "    var v: long := 5 if 1 < 2 else 9000000000L; "
        "    return 1 if v = 5L else 0; "
        "  end Main; "
        "end C;"), 1);
}

// --- §20: branches with no common type are rejected (clean diagnostic,
// not a codegen crash). This is the `throw E if c else x` shape: it
// parses as `throw (E if c else x)` (the throw's operand is the whole
// conditional; §17.12, Appendix A), whose branches are an Exception value
// and an int — no common type exists. Sema must reject it. ---
BEZEL_TEST(codegen_cg4, throw_of_incompatible_conditional_is_type_error) {
    JitResult r = runJit(
        "module M; class C "
        "  public static method Main(): int => "
        "    throw new Exception(\"e\") if 1 < 2 else 5; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(r.ran);
}

// Plain incompatible branches (no throw involved) are likewise rejected.
BEZEL_TEST(codegen_cg4, incompatible_branches_is_type_error) {
    JitResult r = runJit(
        "module M; class N end N; class C "
        "  public static method Main(): int "
        "    var x := new N(); "
        "    var y := 5 if 1 < 2 else x; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(r.ran);
}
