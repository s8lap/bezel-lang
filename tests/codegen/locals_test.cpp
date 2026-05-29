// Codegen slice CG3 — local variables and block-bodied methods. Lowers
// `var`/`const` declarations (entry-block alloca + store), assignment
// (store), local reads (load), and `return` in a statement body.

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

BEZEL_TEST(codegen_cg3, single_local_returned) {
    BEZEL_EXPECT_EQ(run("var x := 42; return x;"), 42);
}

BEZEL_TEST(codegen_cg3, two_locals_summed) {
    BEZEL_EXPECT_EQ(run("var x := 40; var y := 2; return x + y;"), 42);
}

BEZEL_TEST(codegen_cg3, local_in_expression) {
    BEZEL_EXPECT_EQ(run("var n := 5; return n * n + 1;"), 26);
}

BEZEL_TEST(codegen_cg3, assignment_updates_local) {
    BEZEL_EXPECT_EQ(run("var x := 1; x := x + 9; return x;"), 10);
}

BEZEL_TEST(codegen_cg3, reassign_from_other_local) {
    BEZEL_EXPECT_EQ(run("var a := 3; var b := 4; a := b; return a;"), 4);
}

BEZEL_TEST(codegen_cg3, const_local) {
    BEZEL_EXPECT_EQ(run("const k := 7; return k * 2;"), 14);
}

BEZEL_TEST(codegen_cg3, typed_local) {
    BEZEL_EXPECT_EQ(run("var x: int := 100; return x - 1;"), 99);
}

BEZEL_TEST(codegen_cg3, early_return_wins) {
    // The second return is dead; lowering stops at the first terminator.
    BEZEL_EXPECT_EQ(run("return 11; return 22;"), 11);
}
