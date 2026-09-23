// Codegen — issue #1525 piece (2) / spec §29.7 "Disposal of partially-consumed
// iterators" + §28.12 "Asynchronous disposal" (v0.228):
//
//   §29.7: "When a consumer stops iterating early — break, exception, or
//   using var ending before exhaustion — the iterator is disposed. ... Runs
//   the pending cleanup actions active at the point of suspension — using var
//   disposals (including, via §28.12's synchronous-fallback path, await using
//   var disposals) and finally clauses ... in reverse order of establishment
//   ... Transitions to a completed state; subsequent MoveNext() returns
//   false; second Dispose() is a no-op."
//
//   §28.12: "Implicit discharge is synchronous. ... Asynchronous cleanup runs
//   only where the program wrote await using ... with §29.7's synchronous
//   fallback on early iterator abandonment." And: "If a class implements
//   IAsyncDisposable but has no destructor, synchronous Dispose() performs
//   only physical reclamation ... any cleanup that DisposeAsync would have
//   performed is skipped."
//
// VERIFY-FIRST finding: the compiler's disposal machinery for this — the
// generated async-iterator Dispose() function's `smIterCleanupAtState_`
// establishment-ordered cleanup stack (fix #1598, LlvmBackend_Async.cpp) and
// `emitAsyncDisposeAwaitAndFree`'s defensive `inAsyncSM` check
// (LlvmBackend_Async.cpp) — was ALREADY CORRECT for the core §29.7 sync-
// fallback behavior before this change: an `await using var` active at a
// yield's suspension point, when the enumerator is abandoned (break,
// exception, or its own using-var scope ending early), is disposed via the
// synchronous destructor path, never DisposeAsync. These tests pin that
// behavior with a spec-cited regression suite (it was previously untested for
// the async-iterator case specifically) and add the one genuine gap the
// adversarial self-pass found: a plain (non-`await`) `using var src :=
// AsyncIteratorMethod();` binding — the correct spelling for a NAMED
// `IAsyncEnumerable<T>`/`IOwningAsyncEnumerable<T>` local, since that
// interface does not itself extend IAsyncDisposable — registered NO
// disposal at all (registerUsingDisposal's `isSyncIterEnum` name check,
// LlvmBackend_Setup.cpp, matched only the four SYNC iterator-interface names),
// leaking the source frame on every exit including full, uneventful
// consumption. Fixed by adding "IAsyncEnumerable"/"IOwningAsyncEnumerable" to
// that name list — disposal there is always synchronous regardless (the
// iterator-format vtable's slot-1 Dispose), matching the sync counterpart
// exactly.
//
// FAILS WITHOUT / PASSES WITH (the source-leak fix): test 6 below
// (named_source_using_var_disposed_exactly_once) leaks 64 bytes under LSan
// without the LlvmBackend_Setup.cpp fix (confirmed via scratch-copy revert);
// r.ran flips false → true.

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
// AsyncRes: implements IAsyncDisposable AND declares a destructor — the
// §28.12 canonical shape with BOTH cleanup paths distinguishable by which
// counter increment ran (destructor +1, DisposeAsync +10). Mirrors
// async_auto_drop_sync_dispose_1663_test.cpp's kAsyncRes.
const char* kAsyncRes =
    "class AsyncRes: IAsyncDisposable "
    "  public static disposed: int := 0; "
    "  public async method DisposeAsync(): Task "
    "    AsyncRes.disposed := AsyncRes.disposed + 10; "
    "  end DisposeAsync; "
    "  destructor AsyncRes.disposed := AsyncRes.disposed + 1; end destructor; "
    "end AsyncRes; ";
const char* kGen3 =
    "class C "
    "  public static async method Gen(): IAsyncEnumerable<int> "
    "    await using var r := new AsyncRes(); "
    "    yield return 1; yield return 2; yield return 3; "
    "  end Gen; "
    "end C; ";
}  // namespace

// Control: full consumption takes the NORMAL exit path — the implicit finally
// of `await using var` awaits DisposeAsync() under the ordinary async driver
// (§28.12, §30.7's own worked example is exactly this shape). disposed == 10
// (DisposeAsync ran), not 1 (the destructor did NOT also run — §28.12's "the
// destructor body ... is not also run, because DisposeAsync supersedes it").
BEZEL_TEST(async_iter_early_abandonment_dispose_1525,
           full_consumption_uses_disposeasync_on_normal_exit) {
    auto r = runAotChecked(
        std::string("module M; ") + kAsyncRes + kGen3 +
        "class C2 "
        "  public static async method Main(): Task<int> "
        "    var sum := 0; "
        "    await for var x in C.Gen() do sum := sum + x; end; "
        "    if sum <> 6 then return 1; end; "
        "    return AsyncRes.disposed; "
        "  end Main; "
        "end C2;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 10);
}

// §29.7 (v0.228): a `break` out of `await for` abandons the iterator while
// suspended at the yield. The pending `await using var r` — active at that
// suspension point — is disposed via §28.12's synchronous-fallback path: the
// destructor runs (+1), DisposeAsync is SKIPPED entirely.
BEZEL_TEST(async_iter_early_abandonment_dispose_1525,
           early_break_uses_synchronous_fallback_destructor) {
    auto r = runAotChecked(
        std::string("module M; ") + kAsyncRes + kGen3 +
        "class C2 "
        "  public static async method Main(): Task<int> "
        "    var sum := 0; "
        "    await for var x in C.Gen() do "
        "      sum := sum + x; "
        "      if x = 1 then break; end; "
        "    end; "
        "    if sum <> 1 then return 100; end; "
        "    return AsyncRes.disposed; "
        "  end Main; "
        "end C2;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);  // sync destructor only, never DisposeAsync
}

// §29.7 (v0.228): an exception propagating out of the loop body abandons the
// iterator identically to `break` — same synchronous-fallback disposal — AND
// the original exception is NOT lost/swallowed by the disposal (the outer
// catch still observes it).
BEZEL_TEST(async_iter_early_abandonment_dispose_1525,
           exception_from_loop_body_disposes_and_preserves_the_exception) {
    auto r = runAotChecked(
        std::string("module M; ") + kAsyncRes + kGen3 +
        "class Boom: Exception "
        "  public constructor() base(\"boom\"); end constructor; "
        "end Boom; "
        "class C2 "
        "  public static async method Main(): Task<int> "
        "    var sum := 0; "
        "    try "
        "      await for var x in C.Gen() do "
        "        sum := sum + x; "
        "        if x = 1 then throw new Boom(); end; "
        "      end; "
        "      return 999; "  // unreachable
        "    catch var ex: Boom => do "
        "      if sum <> 1 then return 100; end; "
        "      if AsyncRes.disposed <> 1 then return 101; end; "
        "      return 0; "
        "    end "
        "    end; "
        "  end Main; "
        "end C2;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// Adversarial self-pass (a): double-dispose when the iterator is abandoned
// AND its frame teardown also disposes. An iterator with BOTH an `owned`
// parameter (§29.8's frame-teardown auto-dispose) and an `await using var`
// (§29.7's abandonment sync-fallback) abandoned together via `break` — both
// disposed EXACTLY once, in the correct combination, with no double-free
// (ASan/LSan-clean via runAotChecked).
BEZEL_TEST(async_iter_early_abandonment_dispose_1525,
           owned_param_and_await_using_both_disposed_exactly_once_on_break) {
    auto r = runAotChecked(
        std::string("module M; ") + kAsyncRes +
        "class Cell public static n: int := 0; public V: int := 5; "
        "  public constructor() Cell.n := Cell.n + 1; end constructor; "
        "  destructor Cell.n := Cell.n - 1; end destructor; "
        "end Cell; "
        "class D "
        "  public static async method Gen(owned c: Cell): IAsyncEnumerable<int> "
        "    await using var r := new AsyncRes(); "
        "    yield return c.V; yield return c.V + 1; "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    var sum := 0; "
        "    await for var x in D.Gen(new Cell()) do sum := sum + x; break; end; "
        "    if sum <> 5 then return 1; end; "
        "    if AsyncRes.disposed <> 1 then return 2; end; "  // sync fallback only
        "    if Cell.n <> 0 then return 3; end; "              // owned param, once
        "    return 0; "
        "  end Main; "
        "end D;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// Adversarial self-pass (d): re-iteration after abandonment. §29.3 marks
// `IAsyncEnumerable<T>` re-iterable ("each call to GetAsyncEnumerator() runs
// the body again from the start"). A first `await for` over a `using var`-
// bound source abandons its enumerator mid-stream (break after the first
// value, sync-fallback disposal, +1); a SECOND `await for` over the SAME
// bound source gets a FRESH, independent enumerator (fix #1268) that runs to
// full completion and takes the normal DisposeAsync exit (+10) — the two
// enumerators' AsyncRes instances are disposed independently and exactly
// once each (disposed == 11 total). This also exercises the source-leak fix:
// `using var src := Gen();` must itself be disposed exactly once at Main's
// scope end without leaking or double-freeing.
BEZEL_TEST(async_iter_early_abandonment_dispose_1525,
           reiteration_after_abandonment_independent_disposal) {
    auto r = runAotChecked(
        std::string("module M; ") + kAsyncRes + kGen3 +
        "class C2 "
        "  public static async method Main(): Task<int> "
        "    using var src := C.Gen(); "
        "    var sum1 := 0; "
        "    await for var x in src do "
        "      sum1 := sum1 + x; "
        "      if x = 1 then break; end; "
        "    end; "
        "    if sum1 <> 1 then return 1; end; "
        "    if AsyncRes.disposed <> 1 then return 2; end; "
        "    var sum2 := 0; "
        "    await for var y in src do sum2 := sum2 + y; end; "
        "    if sum2 <> 6 then return 3; end; "
        "    if AsyncRes.disposed <> 11 then return 4; end; "
        "    return 0; "
        "  end Main; "
        "end C2;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// fix #1525 (piece 2 discovery) / §29.7/§29.11: standalone, minimal regression
// for the source-frame leak — a `using var src := Gen();` binding (the correct
// non-`await` spelling for a named IAsyncEnumerable<T> local, since that
// interface does not extend IAsyncDisposable) disposed exactly once at scope
// end after a SINGLE, uneventful `await for` loop (no break, no re-iteration
// — isolates the fix from the other self-pass scenarios above). Before the
// fix this leaked the 64-byte source frame every time (registerUsingDisposal
// registered no disposal for it at all); LSan under runAotChecked catches it.
BEZEL_TEST(async_iter_early_abandonment_dispose_1525,
           named_source_using_var_disposed_exactly_once_single_loop) {
    auto r = runAotChecked(
        std::string("module M; ") + kAsyncRes + kGen3 +
        "class C2 "
        "  public static async method Main(): Task<int> "
        "    using var src := C.Gen(); "
        "    var sum := 0; "
        "    await for var x in src do sum := sum + x; end; "
        "    return sum; "
        "  end Main; "
        "end C2;");
    BEZEL_EXPECT_TRUE(r.ran);  // ASan/LSan-clean: src itself is not leaked
    BEZEL_EXPECT_EQ(r.exitCode, 6);
}

// Regression / confirms an EXISTING static check this piece's spec reading
// leans on (not a new fix — verified still enforced, since §30.7's own text
// says a class with no synchronous fallback "may not be live across a yield
// return ... a compile error"): TaskScope (stdlib §6.10) implements
// IAsyncDisposable with NO destructor by design (no synchronous escape is
// sound, stdlib §6.10's own prose). Held live across a `yield return` inside
// an async iterator, it must be rejected at compile time rather than silently
// taking a no-op sync fallback that would void the scope's quiescence
// guarantee. `Gen` never even runs (compile error), so runAot's `ran` is
// false and no exit code is produced.
BEZEL_TEST(async_iter_early_abandonment_dispose_1525,
           no_destructor_await_using_live_across_yield_is_compile_error) {
    auto r = runAot(
        "module M; import System.Threading; "
        "class C "
        "  public static async method Gen(): IAsyncEnumerable<int> "
        "    await using var scope := new TaskScope(); "
        "    yield return 1; "
        "    await scope.Join(); "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    var sum := 0; "
        "    await for var x in Gen() do sum := sum + x; end; "
        "    return sum; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);  // rejected at compile time, §30.7
}
