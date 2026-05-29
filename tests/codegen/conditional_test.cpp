// Codegen slice CG4 — comparisons (= <> < > <= >=), bool literals,
// logical and/or/not, and the conditional expression `X if C else Y`
// (§20, §17.1). Observable via the int exit code: the conditional
// selects an int branch based on a bool condition.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& expr) {
    auto r = runJit(std::string("module M; class C "
        "public static method Main(): int => ") + expr + "; end C;");
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
