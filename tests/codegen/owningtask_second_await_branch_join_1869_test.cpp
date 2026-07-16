// §30.5 "Result lifetime and task disposal" (Pattern B) / Appendix C.6 —
// issue #1869 (SB01-C2, residual of #1786).
//
// The branch-join slice of the OwningTask single-consumer rule. A plain owned
// `OwningTask<T>` local awaited on only SOME arm of a branch is MOVED-FROM on
// that arm (§30.5 line ~7399: "a second await is rejected at compile time as a
// moved-from read"); the join leaves it MAYBE-consumed, so a later await/move
// is a compile error (pinned by the two lit reject tests
// tests/lit/ownership/owningtask_second_await_branch_join_1869__*.bzl).
//
// These are the RUNTIME POSITIVE CONTROLS: the fix must not over-reject or
// disturb codegen for the VALID shapes, where the task is discharged on EVERY
// arm and the produced result is disposed exactly once. Both are run under the
// AOT ASan/LSan net (runAotChecked): a leak or double-free on either arm fails.
//
// The fix is ownership-pass-only (an await-SPEND marker upgraded to
// maybe-consumed at a branch join); these tests confirm the marker leaves the
// valid conditional-await disposal path byte-for-byte unchanged. // per §30.5

#include "support/aot_support.hpp"   // runAotChecked — AOT ASan/LSan memory net
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
int runChecked(const std::string& program) {
    auto r = runAotChecked(program);
    return r.ran ? r.exitCode : -999;
}

// Cell: a class with a static live-instance counter (ctor ++, dtor --) so a
// leak / double-free is observable as a nonzero final count.
const char* kCell =
    "class Cell public static n: int := 0; "
    "  public constructor() Cell.n:=Cell.n+1; end constructor; "
    "  destructor Cell.n:=Cell.n-1; end destructor; end Cell; ";
}  // namespace

// -----------------------------------------------------------------------------
// Positive control 1 — await on BOTH arms. The task is spent (and its Cell
// consumed into a `using var`, disposed at the arm's block end) on whichever arm
// runs. After RunTest returns, Cell.n is back to 0 — the result was disposed
// exactly once. The method compiles (no over-rejection: both arms discharge the
// task, so there is no maybe-consumed disagreement at the join). Exercised on
// BOTH arms (flag true and false). Return `r*10 + Cell.n` so the assertion pins
// both the taken arm (r) and exactly-once disposal (Cell.n == 0). // per §30.5
// -----------------------------------------------------------------------------
BEZEL_TEST(owningtask_second_await_branch_join_1869,
           await_both_arms_disposes_once_true_30_5) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; " + std::string(kCell) +
        "class C "
        "  public static async method MakeAsync(): OwningTask<Cell> "
        "    return new Cell(); "
        "  end MakeAsync; "
        "  public static async method RunTest(flag: bool): Task<int> "
        "    var t := C.MakeAsync(); "
        "    var r: int := 0; "
        "    if flag then "
        "      using var g1 := await t; r := 1; "
        "    else "
        "      using var g2 := await t; r := 2; "
        "    end; "
        "    return r * 10 + Cell.n; "        // taken arm * 10, + live-count 0
        "  end RunTest; "
        "  public static async method Main(): Task<int> "
        "    return await C.RunTest(true); "  // 10: arm 1 taken, Cell disposed
        "  end Main; end C;"),
        10);
}

BEZEL_TEST(owningtask_second_await_branch_join_1869,
           await_both_arms_disposes_once_false_30_5) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; " + std::string(kCell) +
        "class C "
        "  public static async method MakeAsync(): OwningTask<Cell> "
        "    return new Cell(); "
        "  end MakeAsync; "
        "  public static async method RunTest(flag: bool): Task<int> "
        "    var t := C.MakeAsync(); "
        "    var r: int := 0; "
        "    if flag then "
        "      using var g1 := await t; r := 1; "
        "    else "
        "      using var g2 := await t; r := 2; "
        "    end; "
        "    return r * 10 + Cell.n; "
        "  end RunTest; "
        "  public static async method Main(): Task<int> "
        "    return await C.RunTest(false); " // 20: arm 2 taken, Cell disposed
        "  end Main; end C;"),
        20);
}

// -----------------------------------------------------------------------------
// Positive control 2 — a SINGLE straight-line await of a plain owned task local
// still compiles and disposes the result exactly once. The #1869 fix must not
// perturb the everyday one-await shape (the await-spend marker only matters at a
// branch join). Cell.n is 0 after the `using var g` scope ends. // per §30.5
// -----------------------------------------------------------------------------
BEZEL_TEST(owningtask_second_await_branch_join_1869,
           single_await_still_disposes_once_30_5) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; " + std::string(kCell) +
        "class C "
        "  public static async method MakeAsync(): OwningTask<Cell> "
        "    return new Cell(); "
        "  end MakeAsync; "
        "  public static async method RunTest(): Task<int> "
        "    var t := C.MakeAsync(); "
        "    do using var g := await t; end; " // consume + dispose the Cell
        "    return Cell.n; "                   // 0: disposed exactly once
        "  end RunTest; "
        "  public static async method Main(): Task<int> "
        "    return await C.RunTest(); "
        "  end Main; end C;"),
        0);
}
