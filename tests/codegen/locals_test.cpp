// Codegen slice CG3 — local variables and block-bodied methods. Lowers
// `var`/`const` declarations (entry-block alloca + store), assignment
// (store), local reads (load), and `return` in a statement body.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& body) {
    auto r = runAot(std::string("module M; class C "
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

// #1619 / §4.7: a unit-typed local (`()` — "the type with exactly one
// value") crashed the compiler with SIGILL. `lowerTypeRef` maps
// `TypeRef::Kind::Unit` to LLVM `void` (correct for a function RETURN
// type), but that `void` reached `entryAlloca`/`CreateAlloca` unguarded for
// a LOCAL declaration — `IRBuilder::CreateAlloca(voidTy)` is invalid IR
// construction, and this libLLVM build compiles the resulting internal
// `llvm_unreachable` to a hardware trap instead of an assertion, so the
// crash was silent (no diagnostic, exit 132). Covers both the explicit-type
// and inferred-type declaration forms — the inferred-type path re-derives
// `void` from the initializer's sema type via a separate branch
// (LlvmBackend_Setup.cpp lowerLocalDecl) that hit the same bug.
BEZEL_TEST(codegen_cg3, unit_typed_local_explicit_type_1619) {
    BEZEL_EXPECT_EQ(run("var u: () := (); return 0;"), 0);
}

BEZEL_TEST(codegen_cg3, unit_typed_local_inferred_type_1619) {
    BEZEL_EXPECT_EQ(run("var u := (); return 0;"), 0);
}
