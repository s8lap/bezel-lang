// Codegen — issue #1525 piece (1) / spec §30.7 "Token routing in generated
// async iterators" (v0.228):
//
//   "For a compiler-generated async iterator, the enumerator returned by the
//   generated GetAsyncEnumerator(token) captures the token, and every
//   MoveNextAsync() performs an implicit token.ThrowIfCancellationRequested()
//   — on entry, and at each resume from suspension within the advance — so a
//   requested cancellation faults the in-flight MoveNextAsync task with
//   OperationCanceledException at the next suspension boundary."
//
// Before the fix, `GetAsyncEnumerator(token)` (vtable slot 4) took no token
// argument at all: `await for … with token` evaluated the token only to check
// it in the CONSUMER's own loop (§30.8 / #790) before each MoveNextAsync call,
// and a direct `src.GetAsyncEnumerator(token)` call evaluated the token purely
// for side effects/ownership and discarded it (LlvmBackend_Calls2.cpp comment,
// pre-fix: "The token argument is evaluated for its side-effects/ownership
// then discarded"). A consumer driving the manual `IAsyncEnumerator<T>`
// protocol (`await using var en := src.GetAsyncEnumerator(token); await
// en.MoveNextAsync();` — the same manual shape #759 established) therefore
// never observed cancellation at all.
//
// FIX: the async-iterator frame grows a TOKEN_FIELD; GetAsyncEnumerator(token)
// (vtable slot 4) now takes the token as a second argument and stores it into
// the returned enumerator (both the clone path and the affine
// hasOwnedParamCapture self-return path, §29.8). The generated Resume function
// — the single trampoline both MoveNextAsync's direct call ("on entry") and
// every internal `await`'s bz_task_then completion callback ("at each resume
// from suspension within the advance") funnel through — checks the token
// before dispatching to the current state; on cancellation it completes the
// in-flight per-advance task with a fresh OperationCanceledException and
// leaves STATE untouched, so a subsequent Dispose() still runs the pending
// using-var/finally chain for wherever the frame was parked (§29.7 — the same
// abandonment path a `break`/exception exit already takes).
//
// FAILS WITHOUT / PASSES WITH: before the fix, test 1 below returns 1 (the
// stale value was read, cancellation ignored); with the fix it returns 0.

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
int runFull(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §30.7 (v0.228): a token already cancelled BEFORE the first MoveNextAsync
// call on a manually-obtained enumerator (via a direct `GetAsyncEnumerator
// (token)` call — no `await for` involved) faults that first advance with
// OperationCanceledException. Before the fix, m1 read true (value 1, stale)
// and the method returned 1; with the fix the catch arm returns 0.
BEZEL_TEST(async_iter_token_routing_1525,
           manual_getasyncenumerator_cancelled_before_first_advance_faults) {
    auto r = runAotChecked(
        "module M; import System.Threading; "
        "class C "
        "  public static async method Gen(): IAsyncEnumerable<int> "
        "    yield return 1; yield return 2; yield return 3; "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    using var cts := new CancellationTokenSource(); "
        "    cts.Cancel(); "
        "    await using var en := C.Gen().GetAsyncEnumerator(cts.Token); "
        "    try "
        "      var m1 := await en.MoveNextAsync(); "
        "      if m1 then return 1; else return 2; end; "
        "    catch var ex: OperationCanceledException => do return 0; end "
        "    end; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);        // ASan/LSan-clean: en is still disposed once
    BEZEL_EXPECT_EQ(r.exitCode, 0);  // cancellation observed on manual drive
}

// Control: CancellationToken.None (the interface's default) never cancels —
// the manual protocol reads the first two values normally.
BEZEL_TEST(async_iter_token_routing_1525,
           manual_getasyncenumerator_none_reads_values_normally) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; import System.Threading; "
        "class C "
        "  public static async method Gen(): IAsyncEnumerable<int> "
        "    yield return 42; yield return 99; "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    await using var en := C.Gen().GetAsyncEnumerator(CancellationToken.None); "
        "    var m1 := await en.MoveNextAsync(); "
        "    if not m1 then return 1; end; "
        "    if en.Current <> 42 then return 2; end; "
        "    var m2 := await en.MoveNextAsync(); "
        "    if not m2 then return 3; end; "
        "    if en.Current <> 99 then return 4; end; "
        "    return 0; "
        "  end Main; "
        "end C;"), 0);
}

// Control: an un-cancelled CancellationTokenSource's token also iterates
// normally through the manual protocol (the routed token is read, not merely
// stored-and-ignored).
BEZEL_TEST(async_iter_token_routing_1525,
           manual_getasyncenumerator_uncancelled_cts_reads_values) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; import System.Threading; "
        "class C "
        "  public static async method Gen(): IAsyncEnumerable<int> "
        "    yield return 7; "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    using var cts := new CancellationTokenSource(); "
        "    await using var en := C.Gen().GetAsyncEnumerator(cts.Token); "
        "    var m1 := await en.MoveNextAsync(); "
        "    if not m1 then return 1; end; "
        "    if en.Current <> 7 then return 2; end; "
        "    return 0; "
        "  end Main; "
        "end C;"), 0);
}

// §29.8 / §30.7: the affine owned-parameter shape (GetAsyncEnumerator's slot 4
// returns `self`, not a clone — see emitAsyncIteratorMethod's
// hasOwnedParamCapture branch) must ALSO route the token onto the returned
// (self) enumerator, not silently drop it because it took the "return self
// unchanged" path. The receiver is an UNBOUND temporary
// (`P.UseCell(new Cell())`, not bound to a separate named local) — binding it
// to a plain `var` first hits a DIFFERENT, pre-existing double-free (filed
// separately, see the commit message) unrelated to token routing: the
// ownership tracker does not know GetAsyncEnumerator's return aliases such a
// binding's own frame, so both the binding's scope-end auto-drop and the
// returned enumerator's `await using` disposal free the same object. That gap
// is independent of this fix (reproduces with zero args, no cancellation, and
// even pre-dates the token parameter existing at all) and is out of this
// issue's scope (§29.8 aliasing, not §30.7 token routing).
BEZEL_TEST(async_iter_token_routing_1525,
           owned_param_iterator_cancelled_token_faults_via_self_path) {
    auto r = runAotChecked(
        "module M; import System.Threading; "
        "class Cell public V: int := 7; "
        "  public constructor() end constructor; "
        "end Cell; "
        "class P "
        "  public static async method UseCell(owned c: Cell): IAsyncEnumerable<int> "
        "    yield return c.V; "
        "  end UseCell; "
        "  public static async method Main(): Task<int> "
        "    using var cts := new CancellationTokenSource(); "
        "    cts.Cancel(); "
        "    await using var en := P.UseCell(new Cell()).GetAsyncEnumerator(cts.Token); "
        "    try "
        "      var m1 := await en.MoveNextAsync(); "
        "      if m1 then return 1; else return 2; end; "
        "    catch var ex: OperationCanceledException => do return 0; end "
        "    end; "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// Regression: `await for … with token` (§30.8 / #765/#790) still behaves
// identically after the token is ALSO routed into GetAsyncEnumerator — the
// consumer-side per-iteration check and the generated iterator's own
// Resume-entry check are redundant on this path (same condition, the
// consumer's check fires first in program order), so the observable timing is
// unchanged: cancelling before the loop still raises before any iteration.
BEZEL_TEST(async_iter_token_routing_1525,
           await_for_with_token_cancelled_before_loop_still_raises) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; import System.Threading; "
        "class C "
        "  public static async method Gen(): IAsyncEnumerable<int> "
        "    yield return 1; yield return 2; yield return 3; "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    using var cts := new CancellationTokenSource(); cts.Cancel(); "
        "    var sum := 0; "
        "    try "
        "      await for var x in Gen() with cts.Token do sum := sum + x; end; "
        "    catch var ex: OperationCanceledException => do return 42; end "
        "    end; "
        "    return sum; "
        "  end Main; end C;"), 42);
}
