// Scope-mapping guard for issue #2999 (stdlib §4.4 / §30.5, v0.240).
//
// THE FILED CLAIM. `Array<Task>.Add(...)` "is a SILENT NO-OP — the element is
// never appended, Count stays 0, no diagnostic at any severity"; the issue
// hypothesised the ELEMENT TYPE was the axis (Array<int>.Add worked, Array<Task>
// did not) and asked for a scope map: which Task-family element types
// (Task<T>/OwningTask<T>/...), which members (beyond Add), and which sibling
// collections (Set/Stack/Queue/LinkedList) are also affected.
//
// THE REFUTATION (measured on HEAD, 8eab3f8). The element type is NOT the axis.
// `Array<Task>.Add` APPENDS a valid Task producer correctly (Count becomes 1).
// The "silent no-op" the issue observed was the ARGUMENT, not the element type:
// the canary that exposed it used `a.Add(new Task<int>())`, and `new Task<int>()`
// is an INVALID construction — Task is a factory-only type (stdlib §6.7: it is
// obtained from `Task.FromResult` / `Task.Delay` / async methods, never `new`).
// Pre-fix that argument silently lowered to nothing, so Add's `lowerExpr(arg)`
// returned null and the whole Add returned null with no store and no diagnostic
// (Count stayed 0). Two earlier fixes, both ANCESTORS of the issue-filing
// commit, eliminated the silence:
//   * #2974 — reject a `new` on a built-in that produced no value where one is
//     demanded ("`new Task(…)` produced no value … `Task` may not be instantiated
//     at all (new on a static class is a compile error, language spec §11.7)").
//   * #2966 — reject an unlowered member call on a built-in receiver at the call
//     ("`Add` on built-in type `Array` produced no value, and no dispatch arm
//     emitted any code for this call"). This is the cascade from the argument's
//     null: the Add arm (`if (!v) return nullptr;`, LlvmBackend_Calls.cpp) bails
//     when its argument failed to lower, so no arm emitted code.
// On HEAD the issue's exact repro `a.Add(new Task<int>())` is therefore a HARD
// COMPILE ERROR (no binary, two located diagnostics) — NOT a silent return of 0.
//
// THE SCOPE MAP (element-type x member x collection, valid producers). Every cell
// was measured by compiling + running the program and reading `Count` through the
// exit code; every cell APPENDS (Count == expected):
//   element: int            / Array<int>.Add(42)             -> 1
//   element: Task           / Array<Task>.Add(Task.Delay(1)) -> 1
//   element: Task<int>      / Array<Task<int>>.Add(FromResult) -> 1
//   element: OwningTask<C>  / Array<OwningTask<C>>.Add(FromOwnedResult) -> 1
//   member:  Insert         / Array<Task>.Insert(0, t)       -> 1
//   collect: Stack<Task>    / Stack<Task>.Push(t)            -> 1
//   collect: Queue<Task>    / Queue<Task>.Enqueue(t)         -> 1
//   collect: LinkedList<Task>/ LinkedList<Task>.AddLast(t)   -> 1
// The Add/Push/Enqueue arm (LlvmBackend_Calls.cpp) emits the `bz_array_add` /
// `bz_stack_push` / `bz_queue_enqueue` store UNCONDITIONALLY once the argument
// lowers; there is NO element-type gate on the store (boxForTarget is a no-op for
// a class element like Task). So no Task-family element type, member, or sibling
// collection is affected.
//
// WHAT THIS FILE PINS. Issue #2999 described a defect class (silent collection
// no-op for Task-family elements) that #2966/#2974 have already closed. These
// cases lock that closure so the class cannot regress: the POSITIVE cases assert
// that Add/Push/Enqueue APPEND a valid Task-family producer (Count == expected),
// and the NEGATIVE case asserts the issue's exact invalid repro is REJECTED with a
// diagnostic (no binary, i.e. NOT a silent return of 0). The existing #1504
// canary covers only the ASYNC suspend-crossing shape; this file covers the SYNC
// shape the issue's repro used.

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

// per stdlib §4.4 (v0.240), #2999. THE issue's exact scenario with a VALID
// producer: `var a := new Array<Task>(); a.Add(<valid Task>); return a.Count;`.
// `Task.Delay(1)` mints a caller-owned `Task` (§6.7). Pre-#2966 this returned 0
// when the argument was the invalid `new Task<int>()`; with a valid producer it
// has always appended. Pins Count == 1.
BEZEL_TEST(collection_task_add_appends_2999, array_of_task_add_appends) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class App "
        "  public static method Main(): int "
        "    using var t := Task.Delay(1); "
        "    using var a := new Array<Task>(); "
        "    a.Add(t); "
        "    return a.Count; "
        "  end Main; "
        "end App;"), 1);
}

// per stdlib §4.4 (v0.240), #2999. The TYPED task element `Task<int>` — the
// issue's `Array<Task<int>>` shape — appends a `Task<int>` producer
// (`Task.FromResult<int>(42)`, §6.7) the same way. Pins Count == 1.
BEZEL_TEST(collection_task_add_appends_2999, array_of_taskint_add_appends) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class App "
        "  public static method Main(): int "
        "    using var t := Task.FromResult<int>(42); "
        "    using var a := new Array<Task<int>>(); "
        "    a.Add(t); "
        "    return a.Count; "
        "  end Main; "
        "end App;"), 1);
}

// per stdlib §4.4 (v0.240), #2999. Two appends ACCUMULATE (the issue noted
// "Count becomes 2" for two `Array<int>` adds); the Task element accumulates
// identically. Pins Count == 2 (guards against an Add that only ever held 1).
BEZEL_TEST(collection_task_add_appends_2999, two_task_adds_accumulate) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class App "
        "  public static method Main(): int "
        "    using var t1 := Task.Delay(1); "
        "    using var t2 := Task.Delay(2); "
        "    using var a := new Array<Task>(); "
        "    a.Add(t1); a.Add(t2); "
        "    return a.Count; "
        "  end Main; "
        "end App;"), 2);
}

// per stdlib §4.4 (v0.240), #2999 — MEMBER axis. `Insert(0, item)` (not Add)
// appends a Task element the same way. Pins Count == 1.
BEZEL_TEST(collection_task_add_appends_2999, array_of_task_insert_appends) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class App "
        "  public static method Main(): int "
        "    using var t := Task.Delay(1); "
        "    using var a := new Array<Task>(); "
        "    a.Insert(0, t); "
        "    return a.Count; "
        "  end Main; "
        "end App;"), 1);
}

// per stdlib §4.6 (v0.240), #2999 — COLLECTION axis. `Stack<Task>.Push` shares
// the Add/Push/Enqueue lowering arm with Array<T>.Add; it appends a Task element.
// Pins Count == 1.
BEZEL_TEST(collection_task_add_appends_2999, stack_of_task_push_appends) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class App "
        "  public static method Main(): int "
        "    using var t := Task.Delay(1); "
        "    using var s := new Stack<Task>(); "
        "    s.Push(t); "
        "    return s.Count; "
        "  end Main; "
        "end App;"), 1);
}

// per stdlib §4.7 (v0.240), #2999 — COLLECTION axis. `Queue<Task>.Enqueue`
// shares the same lowering arm; it appends a Task element. Pins Count == 1.
BEZEL_TEST(collection_task_add_appends_2999, queue_of_task_enqueue_appends) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class App "
        "  public static method Main(): int "
        "    using var t := Task.Delay(1); "
        "    using var q := new Queue<Task>(); "
        "    q.Enqueue(t); "
        "    return q.Count; "
        "  end Main; "
        "end App;"), 1);
}

// per stdlib §4.4/§6.7 (v0.240), #2999 — NEGATIVE: no silence. The issue's EXACT
// invalid repro `a.Add(new Task<int>())` must NOT silently run and return 0: it
// must be REJECTED with a diagnostic (Task is factory-only, so `new Task<int>()`
// is an invalid construction per §6.7/§11.7). This is the property #2966/#2974
// added and that this case locks: a program that does not compile (ran == false,
// hasErrors) rather than a binary that prints 0.
BEZEL_TEST(collection_task_add_appends_2999, invalid_new_task_rejected_not_silent) {
    auto r = runAot(
        "module M; "
        "class App "
        "  public static method Main(): int "
        "    using var a := new Array<Task<int>>(); "
        "    a.Add(new Task<int>()); "
        "    return a.Count; "
        "  end Main; "
        "end App;");
    BEZEL_EXPECT_FALSE(r.ran);            // no binary produced — NOT a silent run
    bool reported = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error) { reported = true; break; }
    }
    BEZEL_EXPECT_TRUE(reported);          // a diagnostic IS emitted (no silence)
}
