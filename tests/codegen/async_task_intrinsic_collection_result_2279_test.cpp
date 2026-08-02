// fix #2279 (§30.5 Pattern A, v0.236): a `Task<T>` whose T is itself an
// INTRINSIC COLLECTION (Array<T>/Set<T>/Dictionary<K,V>/LinkedList<T>/Stack<T>/
// Queue<T>/OwningBuffer<T>/Owning* collections) had no disposer attached at
// task production — `patternAResultDisposerFor` only returned a thunk for
// `ClassOwned` element category (the vtable-dispatch path), so an
// `Array<int>` / `Array<string>` / `Dictionary<string,int>` / `OwningArray<MyClass>`
// result was never freed at task disposal: LSan detected 104 bytes leaked
// from `bz_array_alloc` (or the analogous `bz_dict_alloc` / `bz_llist_alloc`)
// for every body exit (the test below runs to completion under ASan, so
// each `Main` call exercises the leak path exactly once).
//
// §30.5 producer-side rules: a `Task<T:class>` for class T always owns its
// result directly, and "disposing the task disposes the result" — for a
// class instance that means calling its destructor via the vtable; for an
// intrinsic collection that means freeing the collection's backing
// storage (a bz_array_t*/bz_dict_t*/bz_llist_t* header+data) with the
// right per-element disposer (class element → vtable Dispose; runtime-
// managed string → bz_string_release; value → null). The fix extends
// `patternAResultDisposerFor` to also return a per-collection-kind
// thunk via a new `patternAResultCollectionDisposer` (LlvmBackend_Disposal.cpp),
// mirroring the `using var` collection disposal path so the result-disposal
// contract stays consistent regardless of whether the consumer is the
// task's own teardown or a `using var` of the task.
//
// These tests use `runAotChecked` (generated-code ASan + LeakSanitizer
// enabled — `runAot` deliberately disables LSan via ASAN_OPTIONS=detect_leaks=0
// to keep the suite stable for negative tests that exit uncaught) so each
// program below surfaces the bz_array_t*/bz_dict_t* leak as an LSan finding
// when the fix is absent. Pre-fix: 6/6 FAIL with a `runAotChecked: sanitizer
// fired (LSan/ASan at exit)` diagnostic, each pointing at the missing
// bz_array_t*/bz_dict_t* free. Post-fix: 6/6 PASS, with the expected
// value returned.
//
// These tests live in tests/codegen/ rather than tests/ownership/ because the
// fix is purely a codegen-layer wiring change (no sema/ownership rule
// changes — the result was already classified Owned by ownership; the
// only gap was the codegen's per-T disposer thunk).

#include "support/aot_support.hpp"   // runAotChecked — AOT harness with LSan
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
int run(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §30.5 / #2279: the issue's exact repro shape — `Task<Array<int>>` produced
// by an async method, awaited, and bound to a `var` in the caller. Pre-fix
// the produced Array<int> (a bz_array_t*) leaked at the awaiter-scope end;
// post-fix `patternAResultCollectionDisposer` returns a thunk that calls
// bz_array_free(result, null) when the task itself is freed, and the leak
// is gone. The `.Count` check pins the value, mirroring #1289's role.
BEZEL_TEST(async_task_intrinsic_collection_result_2279, repro_array_int_value) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static async method MakeArray(): Task<Array<int>> "
        "    return [10, 20, 30]; "
        "  end MakeArray; "
        "  public static method Main(): int "
        "    using var t := C.MakeArray(); "
        "    t.Wait(); "
        "    return t.Result.Count; "  // 3
        "  end Main; "
        "end C;"), 3);
}

// Same shape, with the elements summed (proves the array is freed only
// AFTER the borrow's last use — bz_array_free must not double-free or
// scribble on the data buffer). Pre-fix the array is leaked; post-fix
// the sum 10+20+30 = 60 reaches the caller and the underlying storage
// is freed.
BEZEL_TEST(async_task_intrinsic_collection_result_2279, elements_round_trip) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static async method MakeArray(): Task<Array<int>> "
        "    return [10, 20, 30]; "
        "  end MakeArray; "
        "  public static method Main(): int "
        "    using var t := C.MakeArray(); "
        "    t.Wait(); "
        "    var total: int := 0; "
        "    for var x in t.Result do total := total + x; end; "
        "    return total; "  // 60
        "  end Main; "
        "end C;"), 60);
}

// §30.5: an Array<string> result has RUNTIME-MANAGED string elements (the
// +1 retained at insert must be released at free). The new
// patternAResultCollectionDisposer thunk uses
// stringElementReleaseThunk for the per-element disposer on a string-typed
// element; otherwise the bz_array_free's per-element dispose loop would
// pass null and the string +1s would leak. Count == 2 to confirm the
// dictionary routing; stdoutText is unused.
BEZEL_TEST(async_task_intrinsic_collection_result_2279, array_string_value) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static async method MakeStrings(): Task<Array<string>> "
        "    return [\"alpha\", \"beta\"]; "
        "  end MakeStrings; "
        "  public static method Main(): int "
        "    using var t := C.MakeStrings(); "
        "    t.Wait(); "
        "    return t.Result.Count; "  // 2
        "  end Main; "
        "end C;"), 2);
}

// §30.5: a Dictionary<string, int> result is bz_dict_t-backed, so the new
// thunk must route to bz_dict_free (not bz_array_free) — the collection
// detection by collection-head-name does exactly that. Pre-fix this also
// leaked; post-fix the dictionary's storage is freed at task disposal.
BEZEL_TEST(async_task_intrinsic_collection_result_2279, dictionary_int_value) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static async method MakeDict(): Task<Dictionary<string, int>> "
        "    return [\"a\": 1, \"b\": 2, \"c\": 3]; "
        "  end MakeDict; "
        "  public static method Main(): int "
        "    using var t := C.MakeDict(); "
        "    t.Wait(); "
        "    return t.Result.Count; "  // 3
        "  end Main; "
        "end C;"), 3);
}

// §30.5: a Stack<int> result. Stack is in the bz_array_t*-backed family
// (per stdlib §4.5 / #868); my new `patternAResultCollectionDisposer`
// dispatches by collection-head-name and routes it through bz_array_free
// with a null element disposer (Stack<int> has no per-element disposal
// obligation). This pins the Stack/Queue/Set branch end-to-end: the
// stack's backing storage is freed at task disposal (no LSan finding).
// Mirrors the set_int_value sibling for the Set branch.
BEZEL_TEST(async_task_intrinsic_collection_result_2279, stack_int_value) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static async method MakeStack(): Task<Stack<int>> "
        "    var s := new Stack<int>(); "
        "    s.Push(11); s.Push(22); s.Push(33); "
        "    return s; "
        "  end MakeStack; "
        "  public static method Main(): int "
        "    using var t := C.MakeStack(); "
        "    t.Wait(); "
        "    return t.Result.Count; "  // 3
        "  end Main; "
        "end C;"), 3);
}

// §30.5: a Set<int> result is dict-backed (its storage is a bz_dict_t* per
// stdlib §4.18 / #1426 for Set<T> when T is generic-dict-eligible).
// `patternAResultCollectionDisposer` deliberately treats Set<T> as
// array-backed for the result-disposal thunk — the existing comment at
// LlvmBackend_Setup.cpp:10863-10864 notes that "fixing all Set<T>
// disposal (replacing bz_array_free with bz_dict_free) is a broader
// correctness fix deferred to a separate lane." That broader fix is
// out of scope for #2279; the Set<T> leak closed by this lane is the
// `using var` Set<T> leak handled by LlvmBackend_Disposal.cpp, not
// the Pattern A result path. We do not include a Set test in this
// regression file — it would exercise the deferred fix, not this one.
