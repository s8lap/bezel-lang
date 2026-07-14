// Codegen / ownership — issue #1663 (spec §28.12 "Implicit discharge is
// synchronous", v0.184; §28.3 auto-drop; §30.1 async methods).
//
// DEFECT: inside an `async` method body, a plain-`var` owned local that is never
// discharged (no `using`, no `move`, no `.Dispose()`, no `return`) and never
// crosses an `await` was met with a HARD §28.3 ERROR at scope end, instead of the
// affine auto-drop (must-use Warning + synchronous scope-end Dispose) that the
// SYNCHRONOUS case gets. §28.12 (v0.184) settles that the implicit disposal paths
// — auto-drop among them — "always take the synchronous Dispose() path, on every
// edge and in every context including `async` bodies: an implicit disposal point
// never awaits." Such a local never reaches a suspension point, so it is created-
// and-disposed within a single resume activation and cannot be caught live by task
// abandonment; the auto-drop is exactly-once-safe with a plain synchronous Dispose.
//
// FIX: the ownership pass (Ownership.cpp endState auto-drop) now flags such a local
// `autoDropAtScopeEnd` in an async NON-ITERATOR body, gated on it NOT being a
// suspend-local (annotateSuspendLocals — a sound over-approximation of "crosses an
// await"). Codegen already routes `autoDropAtScopeEnd` through registerUsingDisposal
// (plain `var` ⇒ never `await using` ⇒ synchronous Dispose, never DisposeAsync).
//
// FAILS WITHOUT / PASSES WITH: without the fix the R-11 programs below are REJECTED
// (hard §28.3 error) → runAotChecked returns ran=false / non-matching exit → the
// EXPECTs fail. With the fix they compile (Warning only), auto-drop via the
// SYNCHRONOUS destructor, and are ASan/LSan-clean.

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
// Cell: static counter, ctor +1 / dtor -1 — a leak / double-free detector
// (net 0 ⇒ constructed instances were each disposed exactly once).
const char* kCell =
    "class Cell public static n: int := 0; "
    "  public constructor() Cell.n:=Cell.n+1; end constructor; "
    "  destructor Cell.n:=Cell.n-1; end destructor; end Cell; ";

// AsyncRes: the R-11 shape. Implements IAsyncDisposable (DisposeAsync ⇒ +10) AND
// declares a destructor for the synchronous fallback (⇒ +1). Which path ran is
// read off the static counter, so the observable distinguishes sync Dispose
// (destructor, +1) from async Dispose (DisposeAsync, +10).
const char* kAsyncRes =
    "class AsyncRes: IAsyncDisposable "
    "  public static disposed: int := 0; "
    "  public async method DisposeAsync(): Task "
    "    AsyncRes.disposed := AsyncRes.disposed + 10; "
    "  end DisposeAsync; "
    "  destructor AsyncRes.disposed := AsyncRes.disposed + 1; end destructor; "
    "end AsyncRes; ";
}  // namespace

// §28.12 / §28.3 (#1663): a plain `var r := new AsyncRes();` (NO `using`) in an
// async body with NO intervening await auto-drops at scope end and must take the
// SYNCHRONOUS Dispose path — the destructor (+1) runs, NOT DisposeAsync (+10).
// disposed == 1 proves the sync path ran; ASan/LSan-clean proves exactly-once.
BEZEL_TEST(async_auto_drop_sync_dispose_1663, plain_var_auto_drop_runs_destructor_not_disposeasync) {
    auto r = runAotChecked(
        std::string("module M; ") + kAsyncRes +
        "class C "
        "  public static async method Work(): Task "
        "    var r := new AsyncRes(); "     // owned, never discharged, no await
        "  end Work; "
        "  public static async method Main(): Task<int> "
        "    await C.Work(); "
        "    return AsyncRes.disposed; "     // 1 = destructor ran; 10 = DisposeAsync
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);               // ASan/LSan-clean (no leak, no double-free)
    BEZEL_EXPECT_EQ(r.exitCode, 1);         // synchronous destructor, NOT DisposeAsync
}

// §28.3 / §30.1 (#1663): the auto-dropped owned local is disposed EXACTLY ONCE.
// A Cell auto-dropped in an async body leaves the static counter net 0 after the
// task settles (ctor +1, dtor -1). Under runAotChecked a leak or double-free would
// flip r.ran to false.
BEZEL_TEST(async_auto_drop_sync_dispose_1663, plain_var_auto_drop_disposed_exactly_once) {
    auto r = runAotChecked(
        std::string("module M; ") + kCell +
        "class C "
        "  public static async method Work(): Task "
        "    var c := new Cell(); "
        "  end Work; "
        "  public static async method Main(): Task<int> "
        "    await C.Work(); "
        "    return Cell.n; "                // 0 = disposed exactly once
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// §28.12 / §28.3 (#1663): auto-drop is also correct when the local is declared
// AFTER an if-statement that itself follows the last await — the local then lives
// in a CFG block strictly after every await site and is not a suspend-local.
BEZEL_TEST(async_auto_drop_sync_dispose_1663, plain_var_after_post_await_branch_auto_drops) {
    auto r = runAotChecked(
        std::string("module M; ") + kCell +
        "class C "
        "  public static async method Work(): Task "
        "    var x: int := await Task.FromResult(1); "
        "    if x > 0 then x := x + 1; end; "   // control-flow split after the await
        "    var c := new Cell(); "             // block after the await → auto-drops
        "  end Work; "
        "  public static async method Main(): Task<int> "
        "    await C.Work(); "
        "    return Cell.n; "                    // 0 = disposed exactly once
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// CONTROL (§30.1 / §30.5, #1663): an owned local that DOES cross an await stays on
// the state-machine disposal path and is disposed EXACTLY ONCE — the fix must not
// perturb it. A `using var` live across an await is frame-spilled and disposed on
// the final fall-off; net Cell.n == 0, ASan/LSan-clean. (Works with and without the
// fix — a guard that the auto-drop change is confined to non-suspend locals.)
BEZEL_TEST(async_auto_drop_sync_dispose_1663, owned_local_crossing_await_still_disposed_once) {
    auto r = runAotChecked(
        std::string("module M; ") + kCell +
        "class C "
        "  public static async method Work(): Task "
        "    using var c := new Cell(); "
        "    var x: int := await Task.FromResult(1); "   // c is live across this await
        "  end Work; "
        "  public static async method Main(): Task<int> "
        "    await C.Work(); "
        "    return Cell.n; "                    // 0 = disposed exactly once
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// SOUNDNESS BOUNDARY (§28.3, #1663): a plain undischarged owned local that DOES
// cross an await is NOT auto-dropped — it stays the hard §28.3 discharge error (its
// frame-teardown disposal on the §30.5 abandonment edges is a separate piece;
// auto-dropping it would leak on abandonment). The fix must keep this reject.
BEZEL_TEST(async_auto_drop_sync_dispose_1663, undischarged_local_crossing_await_still_rejected) {
    auto r = runAot(
        std::string("module M; ") + kCell +
        "class C "
        "  public static async method Work(): Task "
        "    var c := new Cell(); "                       // owned, undischarged ...
        "    var x: int := await Task.FromResult(1); "    // ... and live across await
        "  end Work; "
        "  public static async method Main(): Task<int> "
        "    await C.Work(); "
        "    return Cell.n; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
