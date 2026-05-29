// Codegen slice CG5 — if / elif / else and while statements (§21.2,
// §21.3). Basic-block control flow over CG4's bool conditions and CG3's
// locals; observable via the int exit code.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& body) {
    auto r = runJit(std::string("module M; class C "
        "public static method Main(): int ") + body + " end Main; end C;");
    return r.ran ? r.exitCode : -999;
}

} // namespace

BEZEL_TEST(codegen_cg5, if_then_taken) {
    BEZEL_EXPECT_EQ(run(
        "var x := 0; if 1 < 2 then x := 5; end; return x;"), 5);
}

BEZEL_TEST(codegen_cg5, if_then_not_taken) {
    BEZEL_EXPECT_EQ(run(
        "var x := 0; if 2 < 1 then x := 5; end; return x;"), 0);
}

BEZEL_TEST(codegen_cg5, if_else) {
    BEZEL_EXPECT_EQ(run(
        "var x := 0; if 2 < 1 then x := 5; else x := 9; end; return x;"), 9);
}

BEZEL_TEST(codegen_cg5, elif_chain) {
    BEZEL_EXPECT_EQ(run(
        "var n := 2; var r := 0; "
        "if n = 1 then r := 10; "
        "elif n = 2 then r := 20; "
        "elif n = 3 then r := 30; "
        "else r := 40; end; "
        "return r;"), 20);
}

BEZEL_TEST(codegen_cg5, elif_else_fallthrough) {
    BEZEL_EXPECT_EQ(run(
        "var n := 99; var r := 0; "
        "if n = 1 then r := 10; elif n = 2 then r := 20; else r := 40; end; "
        "return r;"), 40);
}

BEZEL_TEST(codegen_cg5, return_inside_if) {
    BEZEL_EXPECT_EQ(run(
        "if 1 < 2 then return 7; end; return 3;"), 7);
}

BEZEL_TEST(codegen_cg5, both_branches_return) {
    BEZEL_EXPECT_EQ(run(
        "if 5 > 3 then return 1; else return 2; end;"), 1);
}

BEZEL_TEST(codegen_cg5, while_sum) {
    // sum of 0..4 = 10
    BEZEL_EXPECT_EQ(run(
        "var i := 0; var sum := 0; "
        "while i < 5 do sum := sum + i; i := i + 1; end; "
        "return sum;"), 10);
}

BEZEL_TEST(codegen_cg5, while_zero_iterations) {
    BEZEL_EXPECT_EQ(run(
        "var i := 10; var c := 0; "
        "while i < 5 do c := c + 1; i := i + 1; end; "
        "return c;"), 0);
}

BEZEL_TEST(codegen_cg5, nested_if_in_while) {
    // count evens in 0..5 → 0,2,4 = 3
    BEZEL_EXPECT_EQ(run(
        "var i := 0; var evens := 0; "
        "while i < 6 do "
        "  if (i mod 2) = 0 then evens := evens + 1; end; "
        "  i := i + 1; "
        "end; "
        "return evens;"), 3);
}
