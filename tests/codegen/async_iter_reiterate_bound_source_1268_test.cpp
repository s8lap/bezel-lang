// fix #1268 / §29.3 + §29.11 + §30.7: a second `await for` over the SAME bound
// `IAsyncEnumerable<T>` local must replay the iterator body from the start.
//
// Spec authority (§29.3, v0.226): "An async iterator method … has return type
// `IAsyncEnumerable<T>` (re-iterable) or `IAsyncEnumerator<T>` (single-use)",
// where re-iterable is defined one sentence earlier as "each call to
// `GetEnumerator()` runs the body again from the start"; §29.11 adds "Each
// `GetEnumerator()` call produces an independent enumerator." §29.11's
// single-consumption rule is scoped to a LAZY QUERY ("a value produced by a
// deferred LINQ operator"), NOT to an iterator-method result — so the second
// loop below is well-formed Bezel and must run, not be rejected.
//
// Before the fix the `await for` desugaring treated the source value AS the
// enumerator (the async iterator's vtable had no GetAsyncEnumerator slot), so
// the first loop's exit disposed and freed the very frame the second loop then
// re-drove: ASan reported `heap-use-after-free … in bz$async$Main$Resume`,
// freed by `bz$asynciter$Gen$Dispose`, allocated by `bz$C$Gen` — a SIGSEGV
// (rc 139) without the sanitizer. The fix gives the async-iterator vtable a
// slot-4 `GetAsyncEnumerator` that hands out an independent byte-copy clone
// reset to the start (the async twin of the synchronous fix #943), and moves
// the SOURCE's disposal to its own binder's scope end.
//
// Every cell here uses runAotChecked: the defect was a lifetime defect, and the
// fix moves a disposal — so the oracle must reject a leak or a double free just
// as loudly as the original use-after-free.

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {
int run(const std::string& src) {
    JitResult r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §29.3: the issue's exact repro. Was SIGSEGV (rc 139) / ASan
// heap-use-after-free during the SECOND `await for`; now rc 0.
BEZEL_TEST(async_iter_reiterate_bound_source_1268, repro_two_await_for_one_local) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static async method Gen(): IAsyncEnumerable<int> "
        "    yield return 7; "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    var src := Gen(); "
        "    var s1: int := 0; "
        "    await for var x in src do s1 := x; end; "
        "    var s2: int := 0; "
        "    await for var x in src do s2 := x; end; "
        "    if s1 <> 7 then return 1; end; "
        "    if s2 <> 7 then return 2; end; "
        "    return 0; "
        "  end Main; "
        "end C;"), 0);
}

// §29.3 "runs the body again from the start": re-iteration replays the WHOLE
// sequence, not just the first element — both loops sum 1+2+3 = 6, so the
// two sums are equal and the total is 12. A stale/exhausted enumerator would
// make the second loop contribute 0.
BEZEL_TEST(async_iter_reiterate_bound_source_1268, replays_full_sequence) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static async method Gen(): IAsyncEnumerable<int> "
        "    yield return 1; "
        "    yield return 2; "
        "    yield return 3; "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    var src := Gen(); "
        "    var a: int := 0; "
        "    await for var x in src do a := a + x; end; "
        "    var b: int := 0; "
        "    await for var x in src do b := b + x; end; "
        "    if a <> 6 then return 1; end; "
        "    if b <> 6 then return 2; end; "
        "    return a + b; "  // 12
        "  end Main; "
        "end C;"), 12);
}

// §29.11 "Each GetEnumerator() call produces an independent enumerator": three
// consumptions of one binding are independent, and an iterator body that also
// SUSPENDS (an `await` between yields) replays across the suspend too — the
// per-advance task / spill-area frame fields must be reset in each clone, not
// inherited from the source.
BEZEL_TEST(async_iter_reiterate_bound_source_1268, three_loops_with_await_in_body) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Threading; "
        "class C "
        "  public static async method Gen(): IAsyncEnumerable<int> "
        "    yield return 4; "
        "    await Task.Delay(0); "
        "    yield return 5; "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    var src := Gen(); "
        "    var total: int := 0; "
        "    await for var x in src do total := total + x; end; "
        "    await for var x in src do total := total + x; end; "
        "    await for var x in src do total := total + x; end; "
        "    return total; "  // 3 * 9
        "  end Main; "
        "end C;"), 27);
}

// §29.7 / §30.7: a loop ABANDONED early (`break`) disposes only its own
// enumerator — the source binding survives and a later loop replays in full.
// The iterator's `finally` runs once per abandoned/exhausted enumerator, so the
// counter also proves each loop drove a DISTINCT frame (3 cleanups, not 1).
BEZEL_TEST(async_iter_reiterate_bound_source_1268, break_then_reiterate) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class T public static cleaned: int := 0; end T; "
        "class C "
        "  public static async method Gen(): IAsyncEnumerable<int> "
        "    try "
        "      yield return 1; "
        "      yield return 2; "
        "      yield return 3; "
        "    finally "
        "      T.cleaned := T.cleaned + 1; "
        "    end; "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    var src := Gen(); "
        "    var first: int := 0; "
        "    await for var x in src do first := x; break; end; "
        "    var full: int := 0; "
        "    await for var x in src do full := full + x; end; "
        "    var again: int := 0; "
        "    await for var x in src do again := again + x; end; "
        "    if first <> 1 then return 1; end; "
        "    if full <> 6 then return 2; end; "
        "    if again <> 6 then return 3; end; "
        "    return T.cleaned; "  // one per enumerator: 3
        "  end Main; "
        "end C;"), 3);
}

// §29.11: the pre-existing shapes must be unaffected by the ownership move.
// An UNBOUND temporary source (`await for … in Gen()`) has no binder, so the
// loop still owns and discharges BOTH the source enumerable and the enumerator
// it handed out — a leak or a double free here would fail this cell.
BEZEL_TEST(async_iter_reiterate_bound_source_1268, temporary_source_still_discharged) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static async method Gen(): IAsyncEnumerable<int> "
        "    yield return 7; "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    var s1: int := 0; "
        "    await for var x in Gen() do s1 := x; end; "
        "    var s2: int := 0; "
        "    await for var x in Gen() do s2 := x; end; "
        "    if s1 <> 7 then return 1; end; "
        "    if s2 <> 7 then return 2; end; "
        "    return 0; "
        "  end Main; "
        "end C;"), 0);
}

// §29.8 (#973b) + §29.11: the deliberate CARVE. An async iterator with an
// `owned` PARAMETER carries an affine obligation in its frame, so slot 4 returns
// `self` rather than a byte-copy that would give two frames the same owned
// pointer. The loop then leaves a NAMED source alone (its binder disposes it at
// scope end) — the same late timing the synchronous #943 path already has for
// `var src := UseCell(new Cell()); for var x in src do … end`. Cell.n back to 0
// after the source's scope means the owned param was disposed EXACTLY once.
BEZEL_TEST(async_iter_reiterate_bound_source_1268, owned_param_iterator_self_enumerator) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Cell public static n: int := 0; public V: int := 7; "
        "  public constructor() Cell.n := Cell.n + 1; end constructor; "
        "  destructor Cell.n := Cell.n - 1; end destructor; end Cell; "
        "class P "
        "  public static async method UseCell(owned c: Cell): IAsyncEnumerable<int> "
        "    yield return c.V; "
        "  end UseCell; "
        "  public static async method Main(): Task<int> "
        "    var got: int := 0; "
        "    do "
        "      var src := UseCell(new Cell()); "
        "      await for var x in src do got := x; end; "
        "    end; "
        "    return got * 10 + Cell.n; "  // 70: yielded 7, Cell disposed once
        "  end Main; end P;"), 70);
}

// §30.7: the manual protocol face of the same rule — `GetAsyncEnumerator()`
// declares `owned IAsyncEnumerator<T>`, so two calls on one bound source give
// two independent enumerators, each driven and disposed on its own. Before the
// fix the call returned the receiver, so the first `await using` scope end
// freed the source out from under the second.
BEZEL_TEST(async_iter_reiterate_bound_source_1268, two_get_async_enumerators) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Threading; "
        "class C "
        "  public static async method Gen(): IAsyncEnumerable<int> "
        "    yield return 6; "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    var src := Gen(); "
        "    var a: int := 0; "
        "    do "
        "      await using var e1 := src.GetAsyncEnumerator(CancellationToken.None); "
        "      if await e1.MoveNextAsync() then a := e1.Current; end; "
        "    end; "
        "    var b: int := 0; "
        "    do "
        "      await using var e2 := src.GetAsyncEnumerator(CancellationToken.None); "
        "      if await e2.MoveNextAsync() then b := e2.Current; end; "
        "    end; "
        "    if a <> 6 then return 1; end; "
        "    if b <> 6 then return 2; end; "
        "    return 0; "
        "  end Main; "
        "end C;"), 0);
}
