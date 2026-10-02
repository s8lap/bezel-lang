// fix #1289 / §30.5 Pattern A + §15.4: an `async` method whose declared return
// type is `Task<C>` for an intrinsic COLLECTION `C` and whose body returns a
// collection LITERAL directly (`return [3, 9, 3];`) must complete its task with
// that collection.
//
// Root cause: the return-statement lowering materializes a collection literal
// against the DECLARED return type node — which inside an async method is the
// TASK type (`Task<Array<int>>`), matching none of the collection shapes. `v`
// stayed null, `lowerExpr` also yields null for a CollectionLiteralExpr, and
// `emitAsyncComplete(nullptr)` completed the task with an EMPTY result buffer.
// Reading `t.Result` then produced a NULL `Array<int>` and any use of it
// SIGSEGV'd (rc 139; under ASan: "SEGV on unknown address 0x000000000000 …
// The signal is caused by a READ memory access"). The fix targets the literal
// at the task's RESULT type instead, which is what the value actually is.
//
// These cells use runAot, not runAotChecked, deliberately: the Pattern A
// `Task<intrinsic-collection>` RESULT still has no owner (#2279 — measured, and
// the one-line task-side disposer was shown to double-free), so the fixed
// programs below leak 104 bytes from `bz_array_alloc`. That leak is #2279's to
// close; this test pins the VALUE, which is what #1289 is about.

#include "support/aot_support.hpp"   // runAot — AOT test harness
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §30.5 Pattern A: the issue's exact repro. `Array<T>` is a class type, so
// `Task<Array<int>>` is a legal Pattern-A task, and `t.Wait()` + `t.Result` is
// the §30.3 synchronous escape hatch. Was rc 139 (deterministic); now rc 3.
BEZEL_TEST(async_task_collection_literal_result_1289, repro_direct_array_literal) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static async method MakeAsync(): Task<Array<int>> "
        "    return [3, 9, 3]; "
        "  end MakeAsync; "
        "  public static method Main(): int "
        "    using var t := C.MakeAsync(); "
        "    t.Wait(); "
        "    return t.Result.Count; "
        "  end Main; "
        "end C;"), 3);
}

// §30.5: the elements themselves must survive the transfer into the result
// slot, not just the container's Count — sum 3+9+3 = 15.
BEZEL_TEST(async_task_collection_literal_result_1289, elements_round_trip) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static async method MakeAsync(): Task<Array<int>> "
        "    return [3, 9, 3]; "
        "  end MakeAsync; "
        "  public static method Main(): int "
        "    using var t := C.MakeAsync(); "
        "    t.Wait(); "
        "    var total: int := 0; "
        "    for var x in t.Result do total := total + x; end; "
        "    return total; "  // 15
        "  end Main; "
        "end C;"), 15);
}

// §30.2/§30.5: the same defect was reachable through a proper `await`, not only
// through the `.Result` escape hatch — the result SLOT was empty either way.
// (Pre-#2279 this used `var arr := await C.MakeAsync();` to bind the
// awaited result to a local. With the #2279 Pattern A result-disposal fix
// that path now actually frees the task's storage at task disposal; the
// escape-of-borrow shape (a `var` binding of an unbound-producer await
// result) is precisely what §30.5 rejects as "the borrow produced by the
// await must have a source whose lifetime the compiler can bound", so the
// post-#2279 behavior is to UAF on the freed array. The canonical admitted
// form is `using var` (the result's owner is the using-var local), so we
// use that here — the await still reads the borrow at the same full
// expression, the storage is freed at the using-var's scope end, and the
// borrow never outlives its source.)
BEZEL_TEST(async_task_collection_literal_result_1289, consumed_by_await) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static async method MakeAsync(): Task<Array<int>> "
        "    return [3, 9, 3, 1]; "
        "  end MakeAsync; "
        "  public static async method Main(): Task<int> "
        "    using var t := C.MakeAsync(); "
        "    var arr := await t; "
        "    return arr.Count; "  // 4
        "  end Main; "
        "end C;"), 4);
}

// §15.4 / stdlib §4.8: the same lowering path serves a DICTIONARY literal
// returned directly from `Task<Dictionary<K, V>>` — it fell through the same
// hole (the `Task<…>` target matched neither materializer).
BEZEL_TEST(async_task_collection_literal_result_1289, direct_dict_literal) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static async method MakeAsync(): Task<Dictionary<string, int>> "
        "    return [\"a\": 1, \"b\": 2]; "
        "  end MakeAsync; "
        "  public static method Main(): int "
        "    using var t := C.MakeAsync(); "
        "    t.Wait(); "
        "    return t.Result.Count; "  // 2
        "  end Main; "
        "end C;"), 2);
}

// Control (the issue's documented workaround): binding the literal to a local
// first ALWAYS worked, because the local-decl path targets the local's own
// type. It must keep working — the fix must not disturb the indirect shape.
BEZEL_TEST(async_task_collection_literal_result_1289, bound_local_control) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static async method MakeAsync(): Task<Array<int>> "
        "    var arr: Array<int> := [1, 2, 3, 4]; "
        "    return arr; "
        "  end MakeAsync; "
        "  public static method Main(): int "
        "    using var t := C.MakeAsync(); "
        "    t.Wait(); "
        "    return t.Result.Count; "  // 4
        "  end Main; "
        "end C;"), 4);
}

// A SYNCHRONOUS method returning a collection literal keeps targeting its own
// declared return type — the async carve must be scoped to the async body's
// own Resume function and change nothing here.
BEZEL_TEST(async_task_collection_literal_result_1289, sync_return_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static method Make(): owned Array<int> "
        "    return [3, 9, 3, 1, 5]; "
        "  end Make; "
        "  public static method Main(): int "
        "    using var a := C.Make(); "
        "    return a.Count; "  // 5
        "  end Main; "
        "end C;"), 5);
}
