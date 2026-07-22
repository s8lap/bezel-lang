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
// variant takes ownership, so its tests build the OwningArray inside a producer
// method and pass it as a fresh-producer owned-sink argument
// (`Task.WhenEachOwned(owned C.Make())`); the documented named-local owned-move
// form (`... owned tasks`) needs a separate ownership-pass fix for the for-loop
// iterable position (filed follow-up) and is out of scope for this runtime lane.

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
        "    await for var tk in Task.WhenEachOwned(owned C.Make()) do "
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
        "    await for var tk in Task.WhenEachOwned(owned C.Make()) do "
        "      using var x := await tk; "
        "      count := count + 1; "
        "    end; "
        "    return Cell.n; "  // 0 => every Cell disposed exactly once
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
        "    await for var tk in Task.WhenEachOwned(owned C.Make()) do "
        "      using var x := await tk; "
        "      count := count + 1; "
        "    end; "
        "    return Cell.n + count; "  // 0
        "  end Main; end C;"), 0);
}
