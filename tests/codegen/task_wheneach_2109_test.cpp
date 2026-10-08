// Ownership-pass — Task.WhenEachOwned named-local owned form
// (`await for var tk in Task.WhenEachOwned(owned tasks) do …`).
// Issues #2109 / stdlib §6.7 v0.212 example.
//
// Follow-up to #2095/#2096 — the runtime memory model for WhenEachOwned is
// sound (#2096 closure: disjoint-ownership between the enumerator's input
// array and the ready queue). The front-end, however, never recorded the
// move of a named owned local passed as the for-loop iterable's owned-sink
// argument (`Task.WhenEachOwned(owned tasks)`), because the iterable
// expression lives in the pre-loop block and was never walked by
// `handleDischarge` / `recordMovesIn` (the issue pin). Post-fix, the
// for-loop iterable position's owned-sink arguments route through
// `recordMovesIn` exactly as any other call site — so the named-local form
// is accepted, the loop and Dispose both stay leak-clean, and the
// `using var`-rejection (§28.3 scope-bound) stays in force.
//
// These tests run under runAotChecked — the AOT ASan/LSan oracle — so a
// leak / double-free / use-after-free flips r.ran to false (-999), distinct
// from any legal exit value. The Cell.n == 0 oracle pins the exactly-once
// disposal invariant.

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

// #2109 — documented named-local form is accepted. Producer returns an
// `owned` OwningArray<OwningTask<Cell>> which is bound to a named local
// `tasks`, then moved into the for-loop iterable's owned sink via
// `Task.WhenEachOwned(owned tasks)`. Pre-fix the ownership pass did not
// record this transfer (the iterable is evaluated in the pre-loop block,
// which `handleDischarge` / `recordMovesIn` never visited), so `tasks`
// remained Owned at method-scope end and the §28.3 "would be dropped here
// without disposal" rejection fired. Post-fix `recordMovesIn` runs on the
// iterable and the rejection goes away. The loop yields every
// OwningTask<Cell>, the body disposes each Cell via `using var x`, so
// Cell.n == 0 — pinning exact disposal.
BEZEL_TEST(task_wheneach_2109, owned_named_local_iterable_accepted) {
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
        "    var tasks := C.Make(); "      // owned local binds the producer
        "    var count := 0; "
        "    await for var tk in Task.WhenEachOwned(move tasks) do "  // documented form
        "      using var x := await tk; "
        "      count := count + 1; "
        "    end; "
        "    if count = 3 and Cell.n = 0 then return 0; else return 1; end; "
        "  end Main; end C;"), 0);
}

// #2109 — named-local form with break mid-stream. Pre-fix the ownership pass
// left `tasks` Owned, so the rejection fired; post-fix the move is recorded
// and the enumerator's disposal disposes the array container exactly once.
// `break` mid-loop leaves one unconsumed OwningTask<Cell> in the ready
// queue; the enumerator's Dispose (per the #2096 disjoint-ownership model)
// disposes its Cell via the per-T result disposer. Cell.n == 0 — every
// Cell disposed exactly once.
BEZEL_TEST(task_wheneach_2109, owned_named_local_iterable_break_dispose_once) {
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
        "    var tasks := C.Make(); "
        "    var count := 0; "
        "    await for var tk in Task.WhenEachOwned(move tasks) do "
        "      using var x := await tk; "
        "      count := count + 1; "
        "      break; "  // dispose with two Cells left in the array
        "    end; "
        "    return Cell.n; "  // 0: every Cell disposed exactly once
        "  end Main; end C;"), 0);
}
