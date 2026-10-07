// Codegen — issue #1127 (§30.3 / §30.5): `OwningTask<OwningArray<T>>.Result`
// yielded an empty array instead of the real heap array.
//
// Root cause: sema rejected `OwningTask<T>.Result` entirely with the error
// "OwningTask<T> does not expose a Result property (§30.3)"; this left the
// expression untyped, so codegen materialised a zero-initialised placeholder
// (an empty array) rather than the actual async result.
//
// The fix extends the `Task<T>.Result` sema path (ExprTypecheck_Calls.cpp,
// §30.3) to also accept `OwningTask<T>.Result`, typing it as T.  Codegen for
// `.Result` already used `isTaskTypeName` (which fires for both Task and
// OwningTask), so no codegen change was needed beyond adding `bz_task_free`
// for anonymous producers on the ok and fault paths to avoid a task-struct
// leak (§30.5).
//
// Spec derivation:
//   §30.3 "Result is the synchronous escape hatch: blocks the calling thread
//          until the task completes, then materialises and returns T."
//   §30.5 Pattern B: `OwningTask<T>` transfers ownership of T to the consumer.
// v0.154.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
// Res: a plain class whose field value is readable via indexer.
const char* kImports =
    "import System.Collections.Generic; ";
const char* kRes =
    "class Res public n: int; "
    "  public constructor(v: int) n := v; end constructor; "
    "end Res; ";
// Cell: a class with a static counter (ctor ++, dtor --) for leak detection.
const char* kCell =
    "class Cell public static n: int := 0; "
    "  public constructor() Cell.n:=Cell.n+1; end constructor; "
    "  destructor Cell.n:=Cell.n-1; end destructor; end Cell; ";
}  // namespace

// ---------------------------------------------------------------------------
// §30.3 / §30.5 #1127 — primary repro:
// `OwningTask<OwningArray<Res>>.Result` in a non-async `Main`.
// Pre-fix this compiled to a zero-element array (sema left the expression
// untyped so codegen emitted a zero-initialised placeholder); post-fix the
// real array is returned and arr[0].n + arr[1].n == 7.
// ---------------------------------------------------------------------------
BEZEL_TEST(async_owned_task_result_1127, owned_task_owned_array_result_value) {
    BEZEL_EXPECT_EQ(run(
        "module M; " + std::string(kImports) + kRes +
        "class C "
        "  public static async method LoadAll(): OwningTask<OwningArray<Res>> "
        "    var arr := new OwningArray<Res>(); "
        "    arr.Add(new Res(3)); arr.Add(new Res(4)); "
        "    return arr; "
        "  end LoadAll; "
        "  public static method Main(): int "
        "    using var arr := LoadAll().Result; "
        "    return arr[0].n + arr[1].n; "   // expect 7
        "  end Main; "
        "end C;"), 7);
}

// ---------------------------------------------------------------------------
// §30.5 Pattern B — await form of the same OwningTask<OwningArray<Res>>.
// `using var arr := await C.LoadAll()` inside an async method must also
// yield the real array (regression guard for the await path).
// ---------------------------------------------------------------------------
BEZEL_TEST(async_owned_task_result_1127, owned_task_owned_array_await_value) {
    BEZEL_EXPECT_EQ(run(
        "module M; " + std::string(kImports) + kRes +
        "class C "
        "  public static async method LoadAll(): OwningTask<OwningArray<Res>> "
        "    var arr := new OwningArray<Res>(); "
        "    arr.Add(new Res(3)); arr.Add(new Res(4)); "
        "    return arr; "
        "  end LoadAll; "
        "  public static async method GetCount(): Task<int> "
        "    using var arr := await C.LoadAll(); "
        "    return arr.Count; "          // 2 elements
        "  end GetCount; "
        "  public static async method Main(): Task<int> "
        "    return await C.GetCount(); "
        "  end Main; "
        "end C;"), 2);
}

// ---------------------------------------------------------------------------
// Empty OwningArray — `OwningTask<OwningArray<Res>>.Result` with zero elements
// must succeed without crash and return Count == 0.
// ---------------------------------------------------------------------------
BEZEL_TEST(async_owned_task_result_1127, owned_task_empty_owned_array_result) {
    BEZEL_EXPECT_EQ(run(
        "module M; " + std::string(kImports) + kRes +
        "class C "
        "  public static async method EmptyArr(): OwningTask<OwningArray<Res>> "
        "    var arr := new OwningArray<Res>(); "
        "    return arr; "
        "  end EmptyArr; "
        "  public static method Main(): int "
        "    using var arr := EmptyArr().Result; "
        "    return arr.Count; "          // 0
        "  end Main; "
        "end C;"), 0);
}

// ---------------------------------------------------------------------------
// §30.3: owned elements disposed exactly once — Cell static counter must be
// zero after the `using var arr` scope closes (no double-free, no leak).
// Uses .Result in the non-async form (#1127 repro path).
// ---------------------------------------------------------------------------
BEZEL_TEST(async_owned_task_result_1127, owned_task_owned_array_result_disposal) {
    BEZEL_EXPECT_EQ(run(
        "module M; " + std::string(kImports) + kCell +
        "class C "
        "  public static async method Make(): OwningTask<OwningArray<Cell>> "
        "    var a := new OwningArray<Cell>(); "
        "    a.Add(new Cell()); a.Add(new Cell()); "  // Cell.n → 2
        "    return a; "
        "  end Make; "
        "  public static method Main(): int "
        "    using var arr := Make().Result; "
        "    var alive := Cell.n; "  // 2 while arr is in scope
        "    return alive; "         // expect 2; arr+cells disposed after return
        "  end Main; "
        "end C;"), 2);
}

// ---------------------------------------------------------------------------
// §30.3 regression: `Task<int>.Result` (non-owning) still works after the fix.
// ---------------------------------------------------------------------------
BEZEL_TEST(async_owned_task_result_1127, task_int_result_regression) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Threading.Tasks; "
        "class C "
        "  public static async method GetNum(): Task<int> "
        "    return 42; "
        "  end GetNum; "
        "  public static method Main(): int "
        "    return GetNum().Result; "   // expect 42
        "  end Main; "
        "end C;"), 42);
}

// ---------------------------------------------------------------------------
// §30.5 / #1258: an OwningTask<T:class> DROPPED at scope end (using var, never
// awaited) must dispose its produced owned class result — bz_task_dispose_owned_result
// runs the result's destructor before the task is freed. Cell.n must be 0.
// ---------------------------------------------------------------------------
BEZEL_TEST(async_owned_task_result_1127, owned_task_dropped_disposes_result_1258) {
    BEZEL_EXPECT_EQ(run(
        "module M; " + std::string(kCell) +
        "class C "
        "  public static async method ProduceCell(): OwningTask<Cell> "
        "    return new Cell(); "
        "  end ProduceCell; "
        "  public static method Main(): int "
        "    do using var t := ProduceCell(); end; "  // dropped, never awaited
        "    return Cell.n; "                          // 0 (result disposed)
        "end Main; end C;"), 0);
}

// §30.5 / #1258: when the OwningTask result IS consumed by an await, the task's
// scope-end disposal must NOT double-dispose it (bz_task_take_result zeroed the
// result on consume; the disposer sees null and skips). The awaited `c` disposes
// the Cell exactly once. Cell.n must be 0 — a double-free would crash/underflow.
BEZEL_TEST(async_owned_task_result_1127, owned_task_awaited_result_single_dispose_1258) {
    BEZEL_EXPECT_EQ(run(
        "module M; " + std::string(kCell) +
        "class C "
        "  public static async method ProduceCell(): OwningTask<Cell> "
        "    return new Cell(); "
        "  end ProduceCell; "
        "  public static async method Consume(): Task<int> "
        "    using var t := ProduceCell(); "
        "    using var c := await t; "   // consume — transfers the Cell into c
        "    return 7; "
        "  end Consume; "
        "  public static method Main(): int "
        "    do using var rt := Consume(); end; "  // drives Consume synchronously
        "    return Cell.n; "                       // 0 (disposed once via c)
        "end Main; end C;"), 0);
}
