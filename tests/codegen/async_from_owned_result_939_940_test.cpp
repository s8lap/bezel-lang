// Codegen — issues #939 / #940 (discovery sweep #10): `Task.FromOwnedResult<T>`
// (§30.5 Pattern B / stdlib §6.7) had no sema result type and no codegen, so an
// `OwnedTask<T>` value produced by it (a `using var task := Task.FromOwnedResult
// <Cell>(...)` local, or the inline `await Task.FromOwnedResult<...>()`) fell to
// the i32 placeholder storage. The await lowering then called the task runtime
// helpers with the wrong argument type:
//   #939 (IR_FAIL): `call i1 @bz_task_is_done(i32 ...)` — the verifier rejected
//                   the malformed IR.
//   #940 (SIGSEGV): the same mismatch surfacing at runtime (compiles, crashes).
// The fix types `Task.FromOwnedResult<T>(...)` as `OwnedTask<T>` (so the local /
// await-result lowers to a ptr task handle) AND lowers it to an already-completed
// task carrying the owned class-pointer result (bz_task_from_result, ptr-width).
//
// SOUNDNESS (§30.5 Pattern B / §28.3): awaiting an `OwnedTask<Cell>` yields an
// OWNED Cell; `using var result := await ...` must dispose that Cell EXACTLY ONCE
// at scope end (net Cell.n == 0). The awaited task itself is disposed exactly once
// too. The Cell static counter (ctor n++ / dtor n--) is the leak / double-free
// detector: a count read after the result scope closes must be 0.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}
const char* kCell =
    "class Cell public static n: int := 0; "
    "  public constructor() Cell.n:=Cell.n+1; end constructor; "
    "  destructor Cell.n:=Cell.n-1; end destructor; end Cell; ";
}  // namespace

// #939: `using var task := Task.FromOwnedResult<Cell>(new Cell());`
//       `using var result := await task;` — must compile (no IR_FAIL) and run.
// The awaited owned Cell is alive while `result` is in scope (Cell.n == 1).
BEZEL_TEST(async_from_owned_result_939_940, from_owned_result_bound_local_alive_in_scope) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class C "
        "  public static async method Consume(): Task<int> "
        "    using var task := Task.FromOwnedResult<Cell>(new Cell()); "
        "    using var result := await task; "
        "    return Cell.n; "  // 1 — the owned result is still alive in scope
        "  end Consume; "
        "  public static async method Main(): Task<int> "
        "    return await C.Consume(); "
        "  end Main; "
        "end C;"), 1);
}

// #939 dispose-exactly-once: after the `using var result` scope closes, the owned
// Cell (and the awaited task) are disposed exactly once → net Cell.n == 0.
BEZEL_TEST(async_from_owned_result_939_940, from_owned_result_bound_local_disposed_once) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class C "
        "  public static async method Consume(): Task<int> "
        "    do "
        "      using var task := Task.FromOwnedResult<Cell>(new Cell()); "
        "      using var result := await task; "
        "    end; "
        "    return Cell.n; "  // 0 — result + task disposed exactly once
        "  end Consume; "
        "  public static async method Main(): Task<int> "
        "    return await C.Consume(); "
        "  end Main; "
        "end C;"), 0);
}

// #940: canonical Pattern B `using var result := await Producer()`, where the
// producer is an async method returning OwnedTask<Cell>. Must compile and run
// WITHOUT crashing; the owned result is alive while in scope (Cell.n == 1).
BEZEL_TEST(async_from_owned_result_939_940, await_producer_inline_alive_in_scope) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class C "
        "  public static async method ProduceAsync(): OwnedTask<Cell> "
        "    return new Cell(); "
        "  end ProduceAsync; "
        "  public static async method ConsumeAsync(): Task<int> "
        "    using var result := await C.ProduceAsync(); "
        "    return Cell.n; "  // 1 — owned result alive in scope
        "  end ConsumeAsync; "
        "  public static async method Main(): Task<int> "
        "    return await C.ConsumeAsync(); "
        "  end Main; "
        "end C;"), 1);
}

// #940 dispose-exactly-once: after the `using var result` scope closes, net
// Cell.n == 0 — the awaited owned Cell is disposed exactly once (no leak, no
// double-free).
BEZEL_TEST(async_from_owned_result_939_940, await_producer_inline_disposed_once) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class C "
        "  public static async method ProduceAsync(): OwnedTask<Cell> "
        "    return new Cell(); "
        "  end ProduceAsync; "
        "  public static async method ConsumeAsync(): Task<int> "
        "    do using var result := await C.ProduceAsync(); end; "
        "    return Cell.n; "  // 0 — disposed exactly once
        "  end ConsumeAsync; "
        "  public static async method Main(): Task<int> "
        "    return await C.ConsumeAsync(); "
        "  end Main; "
        "end C;"), 0);
}

// #940 via the FromOwnedResult factory inline: `await Task.FromOwnedResult<...>()`
// directly in an owned-consuming `using var` position (§30.5 Pattern B). Disposed
// exactly once after the scope closes.
BEZEL_TEST(async_from_owned_result_939_940, await_from_owned_result_inline_disposed_once) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class C "
        "  public static async method ConsumeAsync(): Task<int> "
        "    do using var result := await Task.FromOwnedResult<Cell>(new Cell()); end; "
        "    return Cell.n; "  // 0 — disposed exactly once
        "  end ConsumeAsync; "
        "  public static async method Main(): Task<int> "
        "    return await C.ConsumeAsync(); "
        "  end Main; "
        "end C;"), 0);
}

// Control / no regression: a non-owned Task<int> value-result await still works.
BEZEL_TEST(async_from_owned_result_939_940, task_value_result_await_unchanged) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class C "
        "  public static async method GetNum(): Task<int> return 42; end GetNum; "
        "  public static async method Main(): Task<int> "
        "    var x: int := await C.GetNum(); "
        "    return x + 1; "  // 43
        "  end Main; "
        "end C;"), 43);
}
