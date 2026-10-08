// Codegen/runtime — Task.WhenEach + Task.WhenEachOwned streaming fan-in
// (stdlib §6.7 v0.212 / proposal 0023 D1-D7). Issues #2082 / #2095 / #2096.
//
// These tests derive from Bezel_Standard_Library.md §6.7 v0.212 and the
// graduated proposal 0023 (D1-D7). They run under runAotChecked — the AOT
// ASan/LSan oracle — so a leak / double-free / use-after-free flips r.ran to
// false (-999), distinct from any legal exit value. Beyond the spec's happy
// paths, they pin the audit-point-4 blocking-defect closures from #2095/#2096:
//
//   (a) Dispose-before-completion must not UAF (#2095 defect 1 — the
//       refcounted-context lifetime; enumerator survives until observers drain).
//   (b) Already-completed inputs must each be yielded EXACTLY ONCE (#2095
//       defect 2 — the per-index `observed[]` once-flag; no duplicate enqueue).
//   (c) Streaming many completions must not double-count (defect 2, larger N).
//   (d) Owning-variant Dispose must free each unconsumed OwningTask<T>'s class
//       result EXACTLY ONCE (#2096 — the bz_we_dispose disjoint-ownership model:
//       never sweep input-array elements AND drain the ready queue, which double-
//       freed; plus the codegen-threaded per-T result disposer so a
//       Task.FromOwnedResult task's result does not leak on Dispose).
//
// The #1813 input seam (stdlib §6.7): a borrowed `Array<Task<T>>` slot takes no
// ownership, so `tasks.Add(freshCall())` abandons the freshly-minted task. All
// BORROWING tests here build the array over NAMED task bindings. The OWNING
// variant takes ownership, so most of its tests build the OwningArray inside a
// producer method and pass it as a FRESH-PRODUCER owned-sink argument —
// `Task.WhenEachOwned(C.Make())`, written WITHOUT the call-site marker, because
// §22.3 (v0.169) reserves `owned` for a *named owned binding* (the argument form
// the call leaves moved-from) and makes it an error on a fresh producer: "A fresh
// producer — a `new` expression, a call returning `owned T`, an operator result,
// or a `move obj._field` — stays unmarked: no live binding is invalidated, so
// there is nothing to announce." `C.Make()` is declared `owned OwningArray<…>`,
// i.e. exactly that second form. (These calls carried a spurious `owned` marker
// until #2278: the for-loop ITERABLE expression was not walked by the ownership
// pass at all, so no §22.3 check reached them; #2109 added the walk and the
// marker was correctly rejected. The marker is dropped here rather than admitted
// — the spec rule is unambiguous.) The documented named-local owned-move form is
// covered by owned_named_local_marker_transfers below.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// A leak / double-free / use-after-free flips r.ran to false -> -999, distinct
// from any legal exit value.
int run(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

// A class with a static live-count: ctor ++, dtor --. Net 0 after a scope means
// every instance was disposed exactly once (>0 leak, <0 double-free), giving a
// value oracle that complements the ASan/LSan sanitizer oracle.
const char* kCell =
    "class Cell public static n: int := 0; "
    "  public constructor() Cell.n := Cell.n + 1; end constructor; "
    "  destructor Cell.n := Cell.n - 1; end destructor; end Cell; ";

}  // namespace

// ============================================================
// Borrowing: Task.WhenEach<T>(tasks: Array<Task<T>>): IAsyncEnumerable<Task<T>>
// stdlib §6.7 v0.212 / proposal 0023 D1, D2, D5, D6.
// ============================================================

// §6.7 v0.212 / 0023 D1: WhenEach over an array of already-completed tasks,
// built from NAMED bindings (#1813 seam). The loop yields 3 elements and sums
// them (6). The borrowed `tasks` array is held alive by the §28.3 deferred-
// activation frame carriage of the returned IAsyncEnumerable<Task<int>>.
BEZEL_TEST(task_wheneach_2082, borrow_typed_already_complete_sums) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    using var t1 := Task.FromResult(1); "
        "    using var t2 := Task.FromResult(2); "
        "    using var t3 := Task.FromResult(3); "
        "    using var tasks: Array<Task<int>> := [t1, t2, t3]; "
        "    var sum := 0; "
        "    await for var tk in Task.WhenEach(tasks) do "
        "      var x := await tk; "
        "      sum := sum + x; "
        "    end; "
        "    return sum; "
        "  end Main; end C;"), 6);
}

// §6.7 v0.212 / 0023 D1: WhenEach over a no-result `Array<Task>`. The loop
// iterates the completed tasks; the counter records how many ran (3).
BEZEL_TEST(task_wheneach_2082, borrow_no_result_completes) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    using var t1 := Task.FromResult(1); "
        "    using var t2 := Task.FromResult(2); "
        "    using var t3 := Task.FromResult(3); "
        "    using var tasks: Array<Task> := [t1, t2, t3]; "
        "    var count := 0; "
        "    await for var tk in Task.WhenEach(tasks) do "
        "      await tk; "
        "      count := count + 1; "
        "    end; "
        "    return count; "
        "  end Main; end C;"), 3);
}

// §6.7 v0.212 / 0023 D5: an EMPTY input completes immediately — MoveNextAsync
// returns false on the first call, the body never runs, count stays 0.
BEZEL_TEST(task_wheneach_2082, borrow_empty_completes_immediately) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    using var tasks := new Array<Task<int>>(); "
        "    var count := 0; "
        "    await for var tk in Task.WhenEach(tasks) do "
        "      var x := await tk; "
        "      count := count + x; "
        "    end; "
        "    return count; "  // 0
        "  end Main; end C;"), 0);
}

// §6.7 v0.212 / 0023 D6: a faulted task is yielded as a normal element; the
// fault surfaces when the caller awaits it (caught in the loop body). The
// iteration continues past the fault (no short-circuit): 10 + 0 + 30 = 40.
BEZEL_TEST(task_wheneach_2082, borrow_faulted_task_yielded_caught) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static async method Boom(): Task<int> "
        "    throw new InvalidOperationException(\"f\"); end Boom; "
        "  public static async method Main(): Task<int> "
        "    using var t1 := Task.FromResult(10); "
        "    using var t2 := Boom(); "
        "    using var t3 := Task.FromResult(30); "
        "    using var tasks: Array<Task<int>> := [t1, t2, t3]; "
        "    var sum := 0; "
        "    await for var tk in Task.WhenEach(tasks) do "
        "      var r := 0; "
        "      try r := await tk; catch Exception => r := 0; end; "
        "      sum := sum + r; "
        "    end; "
        "    return sum; "  // 10 + 0 + 30 = 40
        "  end Main; end C;"), 40);
}

// AUDIT POINT 4(a) — #2095 defect 1 (UAF after enumerator disposal). Breaking
// out of the loop disposes the enumerator MID-STREAM, while the ready queue
// still holds un-yielded completions and observer registrations are still live.
// Pre-fix, bz_task_free deleted the enumerator context out from under the
// registered continuations (no refcount) — a later deref was a UAF, and the
// unfreed context/queue leaked. Post-fix the refcounted context (every observer
// holds a ref; last drain frees) makes Dispose-before-completion memory-clean.
// Oracle: returns sum after the first element (1); runAotChecked catches any
// UAF/leak on the dispose path (-> -999).
BEZEL_TEST(task_wheneach_2082, borrow_dispose_before_completion_no_uaf) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    using var t1 := Task.FromResult(1); "
        "    using var t2 := Task.FromResult(2); "
        "    using var t3 := Task.FromResult(3); "
        "    using var tasks: Array<Task<int>> := [t1, t2, t3]; "
        "    var sum := 0; "
        "    await for var tk in Task.WhenEach(tasks) do "
        "      var x := await tk; "
        "      sum := sum + x; "
        "      break; "  // dispose the enumerator with t2,t3 still queued
        "    end; "
        "    return sum; "  // 1 (first completion), and no UAF/leak on Dispose
        "  end Main; end C;"), 1);
}

// AUDIT POINT 4(b) — #2095 defect 2 (duplicate enqueue). Five already-completed
// inputs interleaved must each be yielded EXACTLY ONCE, in completion order.
// Pre-fix, bz_task_then's already-done inline fast path plus re-registration
// could push the same input index twice; here that would over-count (count > 5)
// or double a distinct value. The per-index `observed[]` once-flag guarantees
// at-most-once enqueue. Distinct power-of-two values (1,2,4,8,16) make any
// duplicate visible in the sum. Oracle: count == 5 AND sum == 31 -> 0.
BEZEL_TEST(task_wheneach_2082, borrow_interleaved_each_yielded_once) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    using var t1 := Task.FromResult(1); "
        "    using var t2 := Task.FromResult(2); "
        "    using var t3 := Task.FromResult(4); "
        "    using var t4 := Task.FromResult(8); "
        "    using var t5 := Task.FromResult(16); "
        "    using var tasks: Array<Task<int>> := [t1, t2, t3, t4, t5]; "
        "    var count := 0; "
        "    var sum := 0; "
        "    await for var tk in Task.WhenEach(tasks) do "
        "      var x := await tk; "
        "      count := count + 1; "
        "      sum := sum + x; "
        "    end; "
        "    if count = 5 and sum = 31 then return 0; else return 1; end; "
        "  end Main; end C;"), 0);
}

// AUDIT POINT 4(c) — #2095 defect 2, larger N (no double-enqueue under repeated
// MoveNextAsync drain). Eight distinct completions drained one-per-MoveNextAsync
// must sum to exactly 1+..+8 = 36 across exactly 8 iterations; a re-entry that
// re-enqueued an index would change the count or the sum. Oracle: 0.
BEZEL_TEST(task_wheneach_2082, borrow_reentry_no_double_enqueue) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    using var a := Task.FromResult(1); "
        "    using var b := Task.FromResult(2); "
        "    using var c := Task.FromResult(3); "
        "    using var d := Task.FromResult(4); "
        "    using var e := Task.FromResult(5); "
        "    using var f := Task.FromResult(6); "
        "    using var g := Task.FromResult(7); "
        "    using var h := Task.FromResult(8); "
        "    using var tasks: Array<Task<int>> := [a, b, c, d, e, f, g, h]; "
        "    var count := 0; "
        "    var sum := 0; "
        "    await for var tk in Task.WhenEach(tasks) do "
        "      var x := await tk; "
        "      count := count + 1; "
        "      sum := sum + x; "
        "    end; "
        "    if count = 8 and sum = 36 then return 0; else return 1; end; "
        "  end Main; end C;"), 0);
}

// ============================================================
// Owning: Task.WhenEachOwned<T>(owned tasks: OwningArray<OwningTask<T>>)
// stdlib §6.7 v0.212 / proposal 0023 D7.
// ============================================================

// AUDIT POINT 4(d) — #2096 owning-variant Dispose exactly-once free. Break after
// consuming the FIRST completion, leaving TWO OwningTask<Cell> in the ready
// queue when the enumerator is disposed. This is the exact #2096 shape: pre-fix
// bz_we_dispose swept every non-null input-array slot AND drained the ready
// queue, and a completed task lived in BOTH (the observer never nulled the input
// slot) -> the task struct was freed TWICE (ASan double-free). Post-fix
// bz_we_dispose frees only what the enumerator still owns (ready + current), and
// the codegen-threaded per-T result disposer disposes each unconsumed Cell.
// Oracle: Cell.n == 0 — all three Cells disposed exactly once (one by the loop
// body's `using var x`, two by Dispose).
BEZEL_TEST(task_wheneach_2082, owned_break_unconsumed_disposed_once) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class C "
        "  public static method Make(): owned OwningArray<OwningTask<Cell>> "
        "    var arr := new OwningArray<OwningTask<Cell>>(); "
        "    arr.Add(Task.FromOwnedResult<Cell>(new Cell())); "
        "    arr.Add(Task.FromOwnedResult<Cell>(new Cell())); "
        "    arr.Add(Task.FromOwnedResult<Cell>(new Cell())); "
        "    return arr; "
        "  end Make; "
        "  public static async method Main(): Task<int> "
        "    var count := 0; "
        "    await for var tk in Task.WhenEachOwned(C.Make()) do "
        "      using var x := await tk; "
        "      count := count + 1; "
        "      break; "  // dispose with two OwningTask<Cell> still queued
        "    end; "
        "    return Cell.n; "  // 0 => every Cell disposed exactly once
        "  end Main; end C;"), 0);
}

// §6.7 v0.212 / 0023 D7: owning full consume — every OwningTask<Cell> yielded
// and its Cell result taken by the loop body (`using var x := await tk`), then
// disposed exactly once at end of each iteration. Dispose at exhaustion frees
// the array container only (ready empty, nothing taken outstanding). Oracle:
// Cell.n == 0.
BEZEL_TEST(task_wheneach_2082, owned_full_consume_disposed_once) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class C "
        "  public static method Make(): owned OwningArray<OwningTask<Cell>> "
        "    var arr := new OwningArray<OwningTask<Cell>>(); "
        "    arr.Add(Task.FromOwnedResult<Cell>(new Cell())); "
        "    arr.Add(Task.FromOwnedResult<Cell>(new Cell())); "
        "    arr.Add(Task.FromOwnedResult<Cell>(new Cell())); "
        "    return arr; "
        "  end Make; "
        "  public static async method Main(): Task<int> "
        "    var count := 0; "
        "    await for var tk in Task.WhenEachOwned(C.Make()) do "
        "      using var x := await tk; "
        "      count := count + 1; "
        "    end; "
        "    return Cell.n; "  // 0 => every Cell disposed exactly once
        "  end Main; end C;"), 0);
}

// #2096 follow-up — owning Dispose must dispose an UNCONSUMED FAULTED input's
// owned exception object, not just bz_task_free it (bz_task_free never touches
// error_obj, §30.9). One input faults (MakeBoom throws before producing a Cell);
// the loop consumes the first ok element and breaks, leaving the faulted task
// AND an ok task in the ready queue at Dispose. bz_we_dispose frees both via the
// fault-disposing helper: the faulted task's InvalidOperationException is
// disposed (else it leaks — 40 bytes under LSan), and the ok task's Cell is
// disposed via its result disposer. Oracle: Cell.n == 0 (the 2 ok Cells disposed
// exactly once) AND leak-clean (the faulted exception object disposed).
BEZEL_TEST(task_wheneach_2082, owned_faulted_input_unconsumed_disposed_no_leak) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class C "
        "  public static async method MakeOk(): OwningTask<Cell> "
        "    return new Cell(); end MakeOk; "
        "  public static async method MakeBoom(): OwningTask<Cell> "
        "    throw new InvalidOperationException(\"boom\"); end MakeBoom; "
        "  public static method Make(): owned OwningArray<OwningTask<Cell>> "
        "    var arr := new OwningArray<OwningTask<Cell>>(); "
        "    arr.Add(C.MakeOk()); "
        "    arr.Add(C.MakeBoom()); "  // faulted, left unconsumed at Dispose
        "    arr.Add(C.MakeOk()); "
        "    return arr; "
        "  end Make; "
        "  public static async method Main(): Task<int> "
        "    var count := 0; "
        "    await for var tk in Task.WhenEachOwned(C.Make()) do "
        "      using var x := await tk; "
        "      count := count + 1; "
        "      break; "  // dispose with a faulted + an ok task still queued
        "    end; "
        "    return Cell.n; "  // 0, and the faulted exception object disposed
        "  end Main; end C;"), 0);
}

// §6.7 v0.212 / 0023 D5 + D7: owning EMPTY input completes immediately (no
// iterations); Dispose frees the (empty) array container. Oracle: 0.
BEZEL_TEST(task_wheneach_2082, owned_empty_completes_immediately) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class C "
        "  public static method Make(): owned OwningArray<OwningTask<Cell>> "
        "    return new OwningArray<OwningTask<Cell>>(); "
        "  end Make; "
        "  public static async method Main(): Task<int> "
        "    var count := 0; "
        "    await for var tk in Task.WhenEachOwned(C.Make()) do "
        "      using var x := await tk; "
        "      count := count + 1; "
        "    end; "
        "    return Cell.n + count; "  // 0
        "  end Main; end C;"), 0);
}

// ============================================================
// STILL-RUNNING inputs (#2112): every test above feeds already-completed
// inputs (Task.FromResult / Task.FromOwnedResult), so at init each observer
// fires INLINE and MoveNextAsync always takes the synchronous `ready` path —
// the parked `pending_advance` delivery, the disposed->observer-frees
// (`free_completed`) arm, and late-observer refcount survival (the #2095
// closures) are never executed. The tests below feed inputs that are genuinely
// STILL RUNNING at enumeration start, built with the sanctioned Task.Delay
// still-running idiom (async producer awaits Task.Delay before producing —
// stdlib §6.7 Delay; the same idiom as async_451 / taskscope_asan_808). With
// no input completable in the microseconds before the first MoveNextAsync runs,
// that call is forced down the park path (pending_advance), and the completion
// is delivered by the per-input observer that fires later on the timer/pool
// thread — the parked-advance protocol. Instrumented probe evidence (temporary
// counters, #2112 report): owned_still_running_parks_then_delivers observes
// park=3/parked_deliver=3; owned_dispose_in_flight_observer_frees observes
// park=1/parked_deliver=1/free_completed=2.
//
// These use the OWNING variant deliberately: the borrowing variant additionally
// requires the caller to hold the `Array<Task<T>>` local across the loop's
// suspend, which trips a separate general async frame-spill defect (an
// `Array<T>` local is corrupted across an `await`; `OwningArray<T>` is not —
// filed separately from this lane). The park/deliver runtime code
// (bz_we_move_next / bz_we_input_completed) is shared by both variants — only
// the `is_owning` free arm differs — so the owning tests exercise the identical
// parked-advance protocol without depending on that unrelated defect.

// #2112 — park-then-complete (pending_advance delivery). Three OwningTask<Cell>
// inputs are all still running (each awaits Task.Delay before producing its
// Cell) when the `await for` begins, so the first MoveNextAsync finds `ready`
// empty and parks a `pending_advance` task; the per-input observer that fires
// when a Delay elapses delivers the completion to that parked advance (the
// `else if (e->pending_advance)` arm of bz_we_input_completed). Full consume:
// every OwningTask is yielded and its Cell taken by `using var x := await tk`
// and disposed at end of iteration. Oracle: count == 3 (all yielded through the
// parked path) AND Cell.n == 0 (each Cell disposed exactly once) -> 0; any
// leak / double-free / UAF flips runAotChecked's oracle to -999.
BEZEL_TEST(task_wheneach_2082, owned_still_running_parks_then_delivers) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class C "
        "  public static async method MakeCell(ms: int): OwningTask<Cell> "
        "    await Task.Delay(ms); "
        "    return new Cell(); "
        "  end MakeCell; "
        "  public static method Make(): owned OwningArray<OwningTask<Cell>> "
        "    var arr := new OwningArray<OwningTask<Cell>>(); "
        "    arr.Add(C.MakeCell(30)); "
        "    arr.Add(C.MakeCell(30)); "
        "    arr.Add(C.MakeCell(30)); "
        "    return arr; "
        "  end Make; "
        "  public static async method Main(): Task<int> "
        "    var count := 0; "
        "    await for var tk in Task.WhenEachOwned(C.Make()) do "
        "      using var x := await tk; "
        "      count := count + 1; "
        "    end; "
        "    if count = 3 and Cell.n = 0 then return 0; else return 1; end; "
        "  end Main; end C;"), 0);
}

// #2112 — dispose-with-in-flight-inputs: the disposed->observer-frees arm
// (`free_completed`) AND late-observer refcount survival (the #2095 closures).
// The first input has a short delay and completes first; the loop consumes it
// (using var x) and BREAKS, disposing the enumerator while the other two inputs
// are still running (long delay, not yet completed). bz_we_dispose finds `ready`
// empty and `current_yielded` already taken, so it frees no input here — the two
// in-flight tasks are still owned by their registered observers, which each hold
// a refcount on the enumerator context (so it is NOT deleted at Dispose:
// refcount survival). When those Delays later elapse (flushed at scheduler
// shutdown, timer drain), each observer runs bz_we_input_completed, sees
// `disposed` + `is_owning`, and discharges its task through bz_we_free_owned_task
// (`free_completed = true`) — disposing the produced Cell exactly once; the last
// observer's release deletes the context. Oracle: count == 1 (one consumed
// before break); the two in-flight Cells + task structs must be freed exactly
// once by the deferred observers — a leak (observer never freed) or double-free
// (dispose also swept the input) flips runAotChecked's oracle to -999.
BEZEL_TEST(task_wheneach_2082, owned_dispose_in_flight_observer_frees) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class C "
        "  public static async method MakeCell(ms: int): OwningTask<Cell> "
        "    await Task.Delay(ms); "
        "    return new Cell(); "
        "  end MakeCell; "
        "  public static method Make(): owned OwningArray<OwningTask<Cell>> "
        "    var arr := new OwningArray<OwningTask<Cell>>(); "
        "    arr.Add(C.MakeCell(5)); "    // completes first -> consumed
        "    arr.Add(C.MakeCell(60)); "   // in-flight at Dispose -> free_completed
        "    arr.Add(C.MakeCell(60)); "   // in-flight at Dispose -> free_completed
        "    return arr; "
        "  end Make; "
        "  public static async method Main(): Task<int> "
        "    var count := 0; "
        "    await for var tk in Task.WhenEachOwned(C.Make()) do "
        "      using var x := await tk; "
        "      count := count + 1; "
        "      break; "  // dispose with two OwningTask<Cell> still running
        "    end; "
        "    return count; "  // 1; deferred observers free the two in-flight Cells
        "  end Main; end C;"), 1);
}

// #2112 follow-up — a FAULTED in-flight input freed by the disposed observer
// must dispose its owned exception object too (bz_we_free_owned_task disposes
// error_obj; bz_task_free alone never touches it, §30.9). Extends the
// already-completed owned_faulted_input_unconsumed_disposed_no_leak case to the
// DEFERRED path: the faulting input is still running at Dispose and its observer
// runs free_completed at shutdown. One input completes first (consumed + break);
// a still-running faulting input and a still-running ok input remain in flight
// at Dispose. Oracle: count == 1; the faulting input's InvalidOperationException
// (40 bytes) and the ok input's Cell must both be disposed by the deferred
// observers — a leak flips runAotChecked's oracle to -999.
BEZEL_TEST(task_wheneach_2082, owned_faulted_in_flight_dispose_no_leak) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class C "
        "  public static async method MakeCell(ms: int): OwningTask<Cell> "
        "    await Task.Delay(ms); "
        "    return new Cell(); "
        "  end MakeCell; "
        "  public static async method BoomCell(ms: int): OwningTask<Cell> "
        "    await Task.Delay(ms); "
        "    throw new InvalidOperationException(\"boom\"); "
        "  end BoomCell; "
        "  public static method Make(): owned OwningArray<OwningTask<Cell>> "
        "    var arr := new OwningArray<OwningTask<Cell>>(); "
        "    arr.Add(C.MakeCell(5)); "    // completes first -> consumed
        "    arr.Add(C.BoomCell(60)); "   // in-flight faulted -> free_completed disposes exc
        "    arr.Add(C.MakeCell(60)); "   // in-flight ok -> free_completed disposes Cell
        "    return arr; "
        "  end Make; "
        "  public static async method Main(): Task<int> "
        "    var count := 0; "
        "    await for var tk in Task.WhenEachOwned(C.Make()) do "
        "      using var x := await tk; "
        "      count := count + 1; "
        "      break; "  // dispose with a faulted + an ok task still running
        "    end; "
        "    return count; "  // 1; deferred observers free the exc object + the Cell
        "  end Main; end C;"), 1);
}

// ============================================================
// §22.3 (v0.169) call-site marker in the for-loop ITERABLE position — the
// documented proposal 0023 D7 form `Task.WhenEachOwned(owned tasks)` over a
// NAMED owned local. This is the marker's *valid* half, the exact complement of
// the fresh-producer rejection the tests above now honor: `tasks` is a named
// owned binding, so the call leaves it moved-from and §22.3 REQUIRES the marker.
//
// Fails-without / passes-with: the for-loop iterable expression sits in the
// pre-loop CFG block and was not walked by the ownership pass until #2109's
// `forOrigins` walk. Without that walk the transfer is never recorded, `tasks`
// stays Owned at method-scope end, and §28.3's "would be dropped here without
// disposal" rejects the program (r.ran == false -> -999). With the walk, the
// §22.3 marker consumes `tasks`, the enumerator owns the array, and the whole
// thing is ASan/LSan clean. Oracle: count == 2 and Cell.n == 0 -> 20 + 2 = 22;
// a leak / double-free / UAF flips runAotChecked's oracle to -999.
// per stdlib §6.7 v0.212, spec §22.3 v0.169, §28.3
BEZEL_TEST(task_wheneach_2082, owned_named_local_marker_transfers) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class C "
        "  public static method Make(): owned OwningArray<OwningTask<Cell>> "
        "    var arr := new OwningArray<OwningTask<Cell>>(); "
        "    arr.Add(Task.FromOwnedResult<Cell>(new Cell())); "
        "    arr.Add(Task.FromOwnedResult<Cell>(new Cell())); "
        "    return arr; "
        "  end Make; "
        "  public static async method Main(): Task<int> "
        "    var tasks := C.Make(); "      // named owned local
        "    var count := 0; "
        "    await for var tk in Task.WhenEachOwned(move tasks) do "
        "      using var x := await tk; "
        "      count := count + 1; "
        "    end; "
        "    return 20 - Cell.n * 10 + count; "  // 22 iff every Cell disposed once
        "  end Main; end C;"), 22);
}
