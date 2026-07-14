// §30.5 / Appendix C.6 (v0.193; second-await split) — issue #1785.
//
// `OwningTask<T>` for class T is single-consumer (Pattern B). The second-await
// obligation splits by how the task is bound (spec §30.5 "Consumer-side rules",
// the OwningTask paragraph, v0.190):
//
//   - A **plain owned local** `var t := Producer();` is movable, so its read is
//     a CONSUME: `await t` moves it out, and a straight-line second `await t` is
//     a COMPILE-TIME moved-from error. (Pinned by the lit test
//     tests/lit/ownership/owningtask_builtin_class_second_await_1785__*.bzl.)
//
//   - A `using var`-bound task is §28.3-IMMOVABLE, so its read is a BORROW:
//     `await t` is borrow-awaited — it neither consumes the operand nor spends a
//     static single-consumer use. A second `await t` therefore COMPILES; the
//     single-consumer guarantee is enforced at RUNTIME — the second await raises
//     `InvalidOperationException` because the result was already moved out
//     (spec §30.5 "the second await raises InvalidOperationException"; codegen
//     `markSpentAfter`).
//
// The bug: for a BUILT-IN compiler-known class result — `Mutex.AcquireAsync()`
// returns `OwningTask<Lock>` (stdlib §6.2), synthesized with a NULL declaration
// — the ownership pass's await-kind classifier (`taskResultArgIsClass`) failed
// to recognize `Lock` as a class, so `awaitTaskKind` reported NotTask and the
// Pattern B split never fired. A plain-local second await was then mis-diagnosed
// as "dropped without disposal", and (once the classifier is fixed) a naive fix
// would wrongly reject the using-var second await at compile time. Both facets
// are fixed together: the classifier recognizes the built-in class heads, and
// the operand consume is skipped for `using var` bindings (borrow-await).
//
// These tests exercise the RUNTIME half (using-var → compiles, second await
// raises IOE) under the ASan/LSan net (runAotChecked): the abandoned-task path
// and the g1 result must be disposed exactly once, with no leak. A second test
// uses a user class with a disposal counter so the exactly-once disposal is
// asserted directly; on the pre-fix compiler that program did not even compile
// (the using-var second await was rejected), so it is a strict regression pin.

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
// Direction (1), built-in class result: a `using var`-bound `OwningTask<Lock>`
// (from `Mutex.AcquireAsync()`) COMPILES with two awaits; the first await
// consumes the produced Lock (bound to `g1`, released at its scope end), and the
// SECOND await raises `InvalidOperationException` at runtime — caught here and
// turned into the exit code 2. No leak / double-free on any path (runAotChecked).
// Pre-fix: `OwningTask<Lock>` was NotTask, so `g1` was bound as a borrow and the
// held Lock was never released — an ASan/LSan leak. // per §30.5, stdlib §6.2
// -----------------------------------------------------------------------------
BEZEL_TEST(owningtask_builtin_class_second_await_1785,
           builtin_lock_usingvar_second_await_raises_ioe_30_5) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; import System.Threading; "
        "class C "
        "  public static async method RunTest(): Task<int> "
        "    using var m := new Mutex(); "
        "    using var t := m.AcquireAsync(); "        // OwningTask<Lock>, using-var bound
        "    var r: int := 0; "
        "    do using var g1 := await t; r := 1; end; " // first await consumes the Lock
        "    try "
        "      using var g2 := await t; "               // second await — spent → IOE
        "      r := 9; "
        "    catch InvalidOperationException => r := 2; "
        "    end; "
        "    return r; "                                // 2: second await raised IOE
        "  end RunTest; "
        "  public static async method Main(): Task<int> "
        "    return await C.RunTest(); "
        "  end Main; end C;"),
        2);
}

// -----------------------------------------------------------------------------
// Same shape with a USER class carrying a disposal counter, so the exactly-once
// disposal is asserted directly (not only via ASan): after the first await's
// `g1` scope ends the Cell is disposed (Cell.n back to 0), the second await
// raises IOE (r=2), and the return `r + Cell.n` is `2 + 0 == 2`. This program
// did NOT compile before the fix — the using-var second await was wrongly
// rejected as a moved-from consume — so it fails on the pre-fix compiler and
// passes with it. // per §30.5 (v0.190 second-await split)
// -----------------------------------------------------------------------------
BEZEL_TEST(owningtask_builtin_class_second_await_1785,
           usingvar_second_await_compiles_and_disposes_once_30_5) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; import System.Threading; " + std::string(kCell) +
        "class C "
        "  public static async method MakeAsync(): OwningTask<Cell> "
        "    return new Cell(); "
        "  end MakeAsync; "
        "  public static async method RunTest(): Task<int> "
        "    using var t := C.MakeAsync(); "
        "    var r: int := 0; "
        "    do using var g1 := await t; r := 1; end; " // g1 owns the Cell; disposed at block end
        "    try "
        "      using var g2 := await t; "               // second await — spent → IOE
        "      r := 9; "
        "    catch InvalidOperationException => r := 2; "
        "    end; "
        "    return r + Cell.n; "                       // 2 + 0: IOE caught, Cell disposed once
        "  end RunTest; "
        "  public static async method Main(): Task<int> "
        "    return await C.RunTest(); "
        "  end Main; end C;"),
        2);
}

// -----------------------------------------------------------------------------
// Positive control: a SINGLE (correct) await of a built-in `OwningTask<Lock>`
// bound to a PLAIN owned local compiles and runs to completion — the fix must
// not over-consume or spuriously reject the ordinary one-await shape. The Lock
// (`g`) and the task are each disposed exactly once (runAotChecked).
// // per §30.5, stdlib §6.2
// -----------------------------------------------------------------------------
BEZEL_TEST(owningtask_builtin_class_second_await_1785,
           builtin_lock_plain_local_single_await_ok_30_5) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; import System.Threading; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    using var m := new Mutex(); "
        "    var t := m.AcquireAsync(); "               // plain owned local OwningTask<Lock>
        "    using var g := await t; "                  // single await consumes t
        "    return 0; "
        "  end Main; end C;"),
        0);
}
