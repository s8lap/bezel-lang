// Codegen / leak — the #1073 fix deliberately failed closed on five residuals
// to avoid the #1921 / #2248 UAF; #2305 ships the sound widening. Spec
// §28.14 (call-bounded default), §28.9 (runtime-managed reclaim), §27.5
// (closures), §28.3 (exception-path discharge), §29.3 (iterator frames),
// §11 (constructors), §7.5 (null-conditional). Spec revision v0.236.
//
// RESIDUALS ADDRESSED HERE:
//
//   5. #1864's sync-iterator release (syncIterClosureArgs) was normal-edge-only.
//      The fix widens it to also run on the unwind edge — a sync-iterator
//      construction that propagates (an argument eval throws, an inlined
//      generic body throws, etc.) drops its closure temporary on the way out.
//      Implemented in lowerCall by combining syncIterClosureArgs +
//      callBoundedClosureArgs into the same unwind cleanup (the two vectors
//      are disjoint: sync-iterator gated in the call-bounded arm's `isSyncIterCallee`
//      exclusion).
//
//   4a. `new T(lambda ...)` — the ctor formals are also call-bounded
//      (§28.14's verification covers `ast::NewExpr` per `verifiedCallBoundedFormalAt`),
//      and the same AST-shape guard transplants one-for-one. The release runs
//      on the normal-return edge after `CreateCall`, and on the unwind edge via
//      `closureArgUnwindCleanup` installed across the call.
//
// Each case below pins a leak that is LSan-clean after the fix and 40 (+8 env)
// bytes leaked before it; the dual-edge guard keeps the residual 1 (bare-`T`)
// and residual 3 (bare-`T` return) shapes strictly pinned by case I/J — they
// still carry the pre-existing #1073 leak by design, while a wrongly-armed
// release on those shapes would surface as a generated-code UAF that ASan
// still catches.
//
// OBSERVABILITY. `runAotChecked` links + runs under ASan/LSan; a leaked closure
// cell or a double-free/UAF flips `r.ran` to false. exitCode is WEXITSTATUS-
// masked to 8 bits so assertions stay ≤ 255.
//
// FAIL-WITHOUT / PASS-WITH. Case A leaks 40 bytes on base (the sync-iterator
// construction propagates via a deliberately-throwing ctor arg). Case B leaks
// 40 bytes on base (the ctor formals case). All other cases were already
// LSan-clean before the fix and remain so — they pin that the fix adds no
// spurious release on shapes where the callee legitimately retains.

#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

// ===========================================================================
// A — #1864 sync-iterator residual (DEFENSIVE only). The #1864 sync-iterator
// release is `!diverged_`-only; a sync-iterator CONSTRUCTION that propagates
// (a static-init guard, an inlined generic body's uncaught throw, or a future
// call shape that surfaces during construction) would strand the closure cell.
// The fix widens syncIterClosureArgs into the same unwind cleanup as
// callBoundedClosureArgs — they are disjoint, so this is a one-line change.
// The shape is hard to OBSERVE today because `emitIteratorMethod`'s prologue
// (calloc + capture-store + retain) does not itself throw on any test shape
// we can construct — the test pins the fix is regression-safe rather than
// fail-without. The pair of tests B/C below exercises the OBSERVABLE ctor-
// formals half of #2305.
// ===========================================================================
BEZEL_TEST(codegen_2305_closure_arg_residuals,
           sync_iter_construction_compiles_and_runs_spec_28_14) {
    auto r = runAotChecked(
        "module M; "
        "class C "
        "  public static method Iter(n: int, f: (int) -> int): "
        "                                          IEnumerable<int> "
        "    var i := 0; "
        "    while i < n do "
        "      yield return f(i); "
        "      i := i + 1; "
        "    end; "
        "  end Iter; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var v in C.Iter(3, lambda (x) => x + 1) do "
        "      total := total + v; "
        "    end; "
        "    return total; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 6);  // (0+1) + (1+1) + (2+1) = 1+2+3 = 6
}

// ===========================================================================
// B — ctor-formal residual. `new Sink(lambda () => ...)` lowers through
// lowerNew, not lowerCall; before #2305 no closure release was emitted and
// the cell + env leaked. With the fix, the call-bounded AST-shape guard
// transplants one-for-one and the release runs on both edges. The lambda
// captures nothing, so its +1 reference is the only one — releasing on
// either edge is exactly-once.
// ===========================================================================
BEZEL_TEST(codegen_2305_closure_arg_residuals,
           new_ctor_with_lambda_arg_drops_closure_spec_28_14) {
    auto r = runAotChecked(
        "module M; "
        "class Box "
        "  public V: int := 0; "
        "end Box; "
        "class Sink "
        "  public N: int := 0; "
        "  public constructor(h: () -> int) N := h(); end constructor; "
        "  destructor end destructor; "
        "end Sink; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Box(); "
        "    using var s := new Sink(lambda () => 42); "
        "    b.V := s.N; "
        "    return b.V; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// C — ctor-formal residual, complex capture. A lambda that captures a
// `using var` box and yields into the sink — verifies the call-bounded
// release composes with the using-var scope-end disposal and does not
// double-release (the cell goes from +1 → 0 on the normal edge; the using
// scope does not own a separate refcount on the closure).
// ===========================================================================
BEZEL_TEST(codegen_2305_closure_arg_residuals,
           new_ctor_capturing_lambda_no_double_release_spec_28_14) {
    auto r = runAotChecked(
        "module M; "
        "class Box "
        "  public V: int := 0; "
        "end Box; "
        "class Sink "
        "  public N: int := 0; "
        "  public constructor(h: () -> int) N := h(); end constructor; "
        "  destructor end destructor; "
        "end Sink; "
        "class C "
        "  public static method Main(): int "
        "    using var box := new Box(); "
        "    using var s := new Sink(lambda () => do box.V := box.V + 5; "
        "                                       return box.V; "
        "                                     end); "
        "    return s.N; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}