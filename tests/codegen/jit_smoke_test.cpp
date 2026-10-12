// Codegen slice CG0 — toolchain smoke test. Proves the LLVM wiring: the
// front-end runs, LlvmBackend emits a module, and the ORC JIT executes its
// `main`. CG0's emitter always returns 0, so any well-formed program's `main`
// exits 0 for now; real lowering arrives in CG1+.

#include "support/aot_support.hpp"   // runAot — strict AOT oracle
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

BEZEL_TEST(codegen_cg0, empty_main_runs_and_returns_zero) {
    auto r = runAot(
        "module M; class C public static method Main(): int => 0; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

BEZEL_TEST(codegen_cg0, jit_runs_for_a_trivial_class) {
    // End-to-end lowering + JIT execution. (As of CG1 the body is actually
    // lowered, so this returns 7 rather than the CG0 placeholder 0.)
    auto r = runAot(
        "module M; class C public static method Main(): int => 7; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}
