// Scope-expansion guard for issue #2999 (stdlib §4.4/§4.5/§4.6/§4.7/§4.10/§30.5,
// v0.240) — the COLLECTION/MEMBER axis the issue did NOT measure. Pairs with
// collection_task_add_appends_2999_test.cpp, which covers the SYNC Array/Stack/
// Queue shapes the issue's repro used; THIS file covers the shapes the lane
// brief explicitly flagged as NOT measured at issue-filing time:
//
//   - LinkedList<Task>.AddLast (§4.6 — borrow-list node insertion)
//   - OwningArray<OwningTask<C>>.Add (§4.5 — owned-class transfer into
//     the owning array; a fresh `new C()` producer transferred via
//     Task.FromOwnedResult is the per-stdlib-§6.7 spelling)
//   - Set<Task>.Add (§4.10 — borrowing hash set; element identity by §8.3
//     reference equality, so a Task is just a Task here)
//   - Array<Task>[i] := t (indexer-set on a borrowing array of class
//     elements — the residual write form `Assign` lowered; the issue noted
//     it was not measured)
//
// On HEAD every cell APPENDS (Count == expected). The Add/Push/Enqueue arm
// (LlvmBackend_Calls.cpp) emits `bz_array_add` / `bz_stack_push` /
// `bz_queue_enqueue` / `bz_llist_add_last` / `bz_set_add` UNCONDITIONALLY
// once the argument lowers; there is no element-type gate (Task is a class,
// elementPolicyFor falls into the ClassBorrow row, no special-case).
//
// The canary already exists (collection_task_add_appends_2999) and pins the
// core fix. This file pins the SCOPE EXPANSION so the residual shapes
// cannot regress independently.

#include <string>

#include "support/aot_support.hpp"   // runAot — AOT, the strict oracle
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// Run a program that returns a Count through the exit code; -999 flags a
// compile/link failure so a silent no-op (Count 0) is never confused with one.
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// stdlib §4.6, #2999. `LinkedList<Task>.AddLast(t)` appends a Task element.
// `LinkedList<T>` is bz_llist_t-backed (NOT bz_array_t), so this exercises a
// different runtime store path than the Array/Stack/Queue cases. Pins
// Count == 1.
BEZEL_TEST(collection_task_add_scope_2999, linkedlist_of_task_addlast_appends) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class App "
        "  public static method Main(): int "
        "    using var t := Task.Delay(1); "
        "    using var l := new LinkedList<Task>(); "
        "    l.AddLast(t); "
        "    return l.Count; "
        "  end Main; "
        "end App;"), 1);
}

// stdlib §4.5 / §6.7, #2999. `OwningArray<OwningTask<C>>.Add(...)` transfers
// the owned OwningTask (an owned-class element). The valid producer is
// `Task.FromOwnedResult<C>(owned new C())` (§6.7 — mints a fresh Pattern-B
// task). The owning family takes over disposal of the element; the array
// then frees the task in its own element-disposer at scope end. Pins
// Count == 1.
BEZEL_TEST(collection_task_add_scope_2999, owningarray_of_owningtask_add_appends) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "end C; "
        "class App "
        "  public static method Main(): int "
        "    using var oa := new OwningArray<OwningTask<C>>(); "
        "    oa.Add(Task.FromOwnedResult<C>(new C())); "
        "    return oa.Count; "
        "  end Main; "
        "end App;"), 1);
}

// stdlib §4.10, #2999. `Set<Task>.Add(t)` appends a Task element. `Set<T>` is
// bz_dict_t-backed (NOT bz_array_t), so this exercises yet another runtime
// store path; Task elements compare by §8.3 reference identity, so the
// dict-backed hash-insert just records the pointer. Pins Count == 1.
BEZEL_TEST(collection_task_add_scope_2999, set_of_task_add_appends) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class App "
        "  public static method Main(): int "
        "    using var t := Task.Delay(1); "
        "    using var s := new Set<Task>(); "
        "    s.Add(t); "
        "    return s.Count; "
        "  end Main; "
        "end App;"), 1);
}

// stdlib §4.4, #2999 — INDEXER axis. `a[i] := v` on an `Array<Task>` is a
// write form the issue did not measure. After Add + indexer-set, Count is
// unchanged (1) and the slot holds a valid Task pointer (no silent no-op on
// the write). Pins Count == 1.
BEZEL_TEST(collection_task_add_scope_2999, array_of_task_indexer_set_writes) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class App "
        "  public static method Main(): int "
        "    using var t := Task.Delay(1); "
        "    using var u := Task.Delay(2); "
        "    using var a := new Array<Task>(); "
        "    a.Add(t); "
        "    a[0] := u; "
        "    return a.Count; "
        "  end Main; "
        "end App;"), 1);
}
