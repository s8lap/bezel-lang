// Codegen — issue #910 (discovery sweep #7): `using var c := await <OwningTask<T>>`
// inside an async method did not dispose the owned T at scope end (a leak).
// usingVarOwnsClassObjectDynamically only inspected a CallExpr initializer, so
// the `await` UnaryExpr fell through and no disposal was registered. The
// `await using var` (async-dispose) and explicit `c.Dispose()` forms both worked,
// confirming the object and runtime paths are sound — only the synchronous
// `using var` discharge of an awaited owned result was missing. §30.5 Pattern B /
// §28.10.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
const char* kCell =
    "class Cell public static n: int := 0; "
    "  public constructor() Cell.n:=Cell.n+1; end constructor; "
    "  destructor Cell.n:=Cell.n-1; end destructor; end Cell; ";
}  // namespace

// `using var c := await Make()` (Make returns OwningTask<Cell>) disposes c.
BEZEL_TEST(async_using_var_await_dispose_910, using_var_awaited_owned_task_disposed) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class C "
        "  public static async method Make(): OwningTask<Cell> return new Cell(); end Make; "
        "  public static async method Run(): Task<int> "
        "    using var c := await C.Make(); "
        "    return 0; "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    var r: int := await C.Run(); "
        "    return Cell.n; "  // 0
        "  end Main; "
        "end C;"), 0);
}

// Control: the `await using var` (async-dispose) form still disposes.
BEZEL_TEST(async_using_var_await_dispose_910, await_using_var_still_disposes) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class C "
        "  public static async method Make(): OwningTask<Cell> return new Cell(); end Make; "
        "  public static async method Run(): Task<int> "
        "    await using var c := await C.Make(); "
        "    return 0; "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    var r: int := await C.Run(); "
        "    return Cell.n; "  // 0
        "  end Main; "
        "end C;"), 0);
}

// §28.3 / §30.1 (#938): a `using var` at the METHOD-LEVEL scope of an async body
// (no nested block, no await, no explicit return) is disposed on normal fall-off.
// Previously the async body's top-level dispose scope was popped without emitting
// its disposals, so this leaked (only nested `do…end` scopes were freed).
BEZEL_TEST(async_using_var_await_dispose_910, method_level_using_var_disposed_938) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class C "
        "  public static async method LeakAsync(): Task "
        "    using var c := new Cell(); "
        "  end LeakAsync; "
        "  public static async method Main(): Task<int> "
        "    await C.LeakAsync(); "
        "    return Cell.n; "  // 0 — c disposed at method-level scope end
        "  end Main; "
        "end C;"), 0);
}

// §28.3 / §30.1 (#938): two method-level `using var`s both disposed on fall-off.
BEZEL_TEST(async_using_var_await_dispose_910, method_level_two_using_vars_disposed_938) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class C "
        "  public static async method M2(): Task "
        "    using var a := new Cell(); using var b := new Cell(); "
        "  end M2; "
        "  public static async method Main(): Task<int> "
        "    await C.M2(); "
        "    return Cell.n; "  // 0
        "  end Main; "
        "end C;"), 0);
}

// §28.3 / §30.1 (#938): a method-level `using var` that CROSSES an await is
// disposed exactly once on the final fall-off (the frame-spilled value).
BEZEL_TEST(async_using_var_await_dispose_910, method_level_using_var_across_await_938) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class C "
        "  public static async method M3(): Task "
        "    using var a := new Cell(); "
        "    var r: int := await Task.FromResult(1); "
        "  end M3; "
        "  public static async method Main(): Task<int> "
        "    await C.M3(); "
        "    return Cell.n; "  // 0
        "  end Main; "
        "end C;"), 0);
}

// #1122 / §30.5: awaiting a BOUND-LOCAL OwningTask whose result is a built-in
// OWNING collection (`OwningArray<T>` — the result type of Task.WhenAllOwned)
// must discharge the bound operand. Before the fix awaitTaskKind's resultIsClass
// only recognized a result type with a user CLASS declaration, so an
// OwningTask<OwningArray<Res>> (a built-in, declaration-less Named) was classified
// NotTask and `var ot := …; await ot;` spuriously reported `ot` as "dropped
// without disposal". OwningArray<T> is a heap/reference type (stdlib §3.1), so
// this is a valid Pattern B — the await consumes `ot` and the result is bound.
BEZEL_TEST(async_using_var_await_dispose_910, await_bound_owntask_owning_collection_1122) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; "
        "class Res public n: int; public constructor(v: int) n := v; end constructor; end Res; "
        "class C "
        "  public static async method MakeArr(): OwningTask<OwningArray<Res>> "
        "    var a := new OwningArray<Res>(); a.Add(new Res(7)); return a; "
        "  end MakeArr; "
        "  public static async method Run(): Task<int> "
        "    var ot := MakeArr(); "
        "    using var arr := await ot; "          // ot consumed by the await
        "    return arr[0].n; "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    return await Run(); "
        "  end Main; "
        "end C;"), 7);
}
