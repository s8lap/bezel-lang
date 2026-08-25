// fix #2627 — spec §29.1 (v0.236) on the ASYNC head: an IOwningAsyncEnumerator<T>
// / IOwningAsyncEnumerable<T> driven to exhaustion must null `current_val` on
// every exhaustion transition (the resume-dispatch "Done" lambda, the fault
// path, AND the body-runs-off-its-end path), so a subsequent TakeCurrent() sees
// null and raises InvalidOperationException per the §29.1 rule.
//
// The sync head (LlvmBackend_Iterators.cpp) implemented this since #1349 (and
// hardened under #2616): the exhaustion transition in MoveNext disposes-then-
// nulls `current_val`, so after MoveNext returns false a later TakeCurrent
// raises IOE. The async head had no equivalent — the "Done" lambda
// (LlvmBackend_Async.cpp) and the fault path set state -1 but never touched
// `current_val`. A stale value remained and TakeCurrent handed it back, which
// per #2616's analysis is also a disposal defect: the value is still owned by
// the enumerator (ownership transfers only at TakeCurrent), so leaking it via
// a plain null would also be a leak. The fix mirrors the sync pattern AND the
// async Dispose path (the #2591 disposeYieldedVal at
// LlvmBackend_Async.cpp:2903-2920): dispose-then-null under the same null guard,
// gated on isOwningIter && elemTy->isPointerTy() (the async frame carries no
// separate current_val_owned flag — #2591 already collapsed the two sync
// branches into the single owning-form dispose). Exactly-once: the
// abandon-then-Dispose pass at #2591 and the new exhaustion passes are both
// dispose-then-null under the same guard, so whichever runs first leaves the
// slot null for the other.
//
// ORACLE. Two assertions:
//   1. Stale-value raise: MoveNextAsync → true → TakeCurrent → MoveNextAsync →
//      false → TakeCurrent must raise InvalidOperationException. A stale read
//      would return the previous (now-untaken) value instead. The enc
//      `caught` flips 0 → 1 only on the IOE catch.
//   2. Disposal: a class-typed yielded value's destructor counter must
//      increment exactly once per yield on the exhaustion path. With the fix
//      the abandoned last value is disposed at the exhaustion transition; the
//      counter agrees with the symmetric #2616 sync test (every yielded R is
//      disposed exactly once). Without the fix it is either leaked (counter
//      under-counts → LSan fails runAotChecked) or — for the take-then-exhaust
//      shape — disposed twice (counter over-counts → ASan fails runAotChecked).
//
// The `mutable static field _n` declaration-site warning (§22.12 rule 2 form
// (iii)) is expected and is a warning, not an error; the counter is written and
// read from one domain only. Same idiom as #2616 / #1349.

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// ASan + LeakSanitizer oracle — the point of every test in this file.
int runChecked(const std::string& program) {
    auto r = runAotChecked(program);
    return r.ran ? r.exitCode : -999;
}

// Live-object counter: R's destructor increments the static `_n`, so `R._n`
// after the enumerator is disposed is the number of R instances destroyed.
const std::string kCountR =
    "class R "
    "  public v: int := 0; "
    "  public static _n: int := 0; "
    "  public constructor(n: int) v := n; end constructor; "
    "  destructor R._n := R._n + 1; end destructor; "
    "end R; ";

}  // namespace

// §29.1 (v0.236) on the ASYNC head — the headline defect: after MoveNextAsync
// returns false, TakeCurrent must raise InvalidOperationException, NOT hand back
// the stale last value. The sync head has done this since #1349; the async head
// (until this fix) returned the stale value.
//
// Shape: do NOT TakeCurrent the first value (so current_val is left set with
// Box(7)). Then MoveNextAsync(false) transitions to exhausted. WITHOUT the
// fix current_val still holds Box(7) at this point, so the second TakeCurrent
// hands back the stale Box(7) — caught stays 0, the program exits 0 (the test
// asserts 1 → FAIL). WITH the fix the exhaustion transition nulls current_val,
// so TakeCurrent raises IOE, caught flips to 1, the program exits 1 (PASS).
BEZEL_TEST(async_enumerator_take_after_exhaustion_2627,
           take_after_exhaustion_raises_ioe_spec_29_1) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class Box "
        "  public v: int := 0; "
        "  public constructor(n: int) v := n; end constructor; "
        "end Box; "
        "class C "
        "  public static async method Gen(): IOwningAsyncEnumerator<Box> "
        "    yield return new Box(7); "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    await using var en := Gen(); "
        "    var b1 := await en.MoveNextAsync(); "
        "    if not b1 then return -1; end; "
        "    var b2 := await en.MoveNextAsync(); "
        "    if b2 then return -2; end; "
        "    var caught := 0; "
        "    try "
        "      using var y := en.TakeCurrent(); "
        "      caught := 0; "
        "    catch InvalidOperationException => caught := 1; end; "
        "    return caught; "
        "  end Main; end C;"), 1);
}

// §29.1: IOwningAsyncEnumerable<T> variant — the same head as the sync #1349
// test, but on the async side. Reach exhaustion, then TakeCurrent must IOE.
//
// NOTE: this test would route through IOwningAsyncEnumerable<T>'s enumerator
// factory, but `GetOwningAsyncEnumerator` on IOwningAsyncEnumerable is
// documented but not yet implemented in the compiler (#2457). Once #2457 is
// shipped, this test should be expanded to cover that route; until then the
// IOwningAsyncEnumerator<Box> test above (test 1) covers the codegen head.
#if 0
BEZEL_TEST(async_enumerator_take_after_exhaustion_2627,
           take_after_exhaustion_raises_ioe_enumerable_spec_29_1) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class Box "
        "  public v: int := 0; "
        "  public constructor(n: int) v := n; end constructor; "
        "end Box; "
        "class C "
        "  public static async method Gen(): IOwningAsyncEnumerable<Box> "
        "    yield return new Box(7); "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    await using var src := Gen(); "
        "    await using var en := src.GetOwningAsyncEnumerator(); "
        "    var b1 := await en.MoveNextAsync(); "
        "    if not b1 then return -1; end; "
        "    using var x := en.TakeCurrent(); "
        "    if x.v <> 7 then return -2; end; "
        "    var b2 := await en.MoveNextAsync(); "
        "    if b2 then return -3; end; "
        "    var caught := 0; "
        "    try "
        "      using var y := en.TakeCurrent(); "
        "      caught := 0; "
        "    catch InvalidOperationException => caught := 1; end; "
        "    return caught; "
        "  end Main; end C;"), 1);
}
#endif

// §29.1: ABANDONED value at exhaustion must be DISPOSED (per #2616's rule, not
// merely nulled). Manual protocol: take R(1), MoveNextAsync to make R(2)
// pending, ABANDON it, drive to exhaustion. Read R._n IMMEDIATELY after
// exhaustion (before the `await using var en` scope-end Dispose fires) — at
// that point only the new exhaustion dispose has had a chance to run.
//
// Without the fix: R(2) is still alive at the MoveNextAsync#3 boundary (no
// exhaustion dispose) → R._n == 0 → assertion of 10 fails.
// With the fix: R(2) disposed at the natural-completion dispose → R._n == 1 →
// return 10. R(1) is still owned by `x` here, so _n == 1 (one disposed).
BEZEL_TEST(async_enumerator_take_after_exhaustion_2627,
           abandoned_value_disposed_on_exhaustion_spec_29_1) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; " + kCountR +
        "class C "
        "  public static async method Gen(): IOwningAsyncEnumerator<R> "
        "    yield return new R(1); "
        "    yield return new R(2); "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    await using var en := Gen(); "
        "    var b1 := await en.MoveNextAsync(); "
        "    if not b1 then return -1; end; "
        "    using var x := en.TakeCurrent(); "
        "    if x.v <> 1 then return -2; end; "
        "    var b2 := await en.MoveNextAsync(); "
        "    if not b2 then return -3; end; "
        "    var b3 := await en.MoveNextAsync(); "
        "    if b3 then return -4; end; "
        "    var n := R._n; "
        "    return n * 10; "
        "  end Main; end C;"), 10);
}

// §29.1 / #2616 control: NO DOUBLE-FREE on the dominant `await for` path. Each
// yielded R is taken and disposed by the `using var` inside the loop; at the
// exhaustion transition `current_val` is null (TakeCurrent already nulled it)
// and the new dispose is a no-op. Counter must equal the number of yields (3).
// A double dispose would push _n to 6 (and ASan would fire); a leak would drop
// it to 2 (LSan would fire).
BEZEL_TEST(async_enumerator_take_after_exhaustion_2627,
           await_for_owning_disposes_each_exactly_once_no_double_free) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; " + kCountR +
        "class C "
        "  public static async method Gen(): IOwningAsyncEnumerable<R> "
        "    yield return new R(1); "
        "    yield return new R(2); "
        "    yield return new R(3); "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    await for var r in Gen() do "
        "      using var u := r; "
        "    end; "
        "    return R._n * 10 + 1; "
        "  end Main; end C;"), 31);
}

// §29.1: TakeCurrent then MoveNextAsync-true then MoveNextAsync-false — the
// exhausted-on-second-call path — must also raise IOE on a final TakeCurrent.
// This exercises the resume-dispatch "Done" lambda, which fires on any
// MoveNextAsync once state is -1.
//
// Shape: take Box(7), then call MoveNextAsync(false) which loads the second
// value Box(8) into current_val, then move to exhaustion WITHOUT taking.
// WITHOUT the fix current_val still holds Box(8) at this point; the final
// TakeCurrent hands back the stale Box(8), caught stays 0 → FAIL. WITH the
// fix the exhaustion transition nulls current_val so TakeCurrent raises IOE,
// caught flips to 1 → PASS.
BEZEL_TEST(async_enumerator_take_after_exhaustion_2627,
           take_after_subsequent_exhaustion_raises_ioe_spec_29_1) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class Box "
        "  public v: int := 0; "
        "  public constructor(n: int) v := n; end constructor; "
        "end Box; "
        "class C "
        "  public static async method Gen(): IOwningAsyncEnumerator<Box> "
        "    yield return new Box(7); "
        "    yield return new Box(8); "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    await using var en := Gen(); "
        "    var b1 := await en.MoveNextAsync(); "
        "    if not b1 then return -1; end; "
        "    using var x := en.TakeCurrent(); "
        "    if x.v <> 7 then return -2; end; "
        "    var b2 := await en.MoveNextAsync(); "
        "    if not b2 then return -3; end; "
        "    /* Do NOT TakeCurrent b2 — leave current_val set to Box(8). */ "
        "    var b3 := await en.MoveNextAsync(); "
        "    if b3 then return -4; end; "
        "    var caught := 0; "
        "    try "
        "      using var y := en.TakeCurrent(); "
        "      caught := 0; "
        "    catch InvalidOperationException => caught := 1; end; "
        "    return caught; "
        "  end Main; end C;"), 1);
}
