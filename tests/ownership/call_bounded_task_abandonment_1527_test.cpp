// Ownership tests for #1527 (§28.14 v0.179, Appendix C.6): call-bounded
// callback × TASK-ABANDONMENT escape sink.
//
// §28.14 v0.179 *Call-bounded × deferred activation*, shape (2): "A callee —
// synchronous or async — that invokes a call-bounded callback whose
// invocation returns a task must treat that returned task as inheriting the
// call bound: it must `await` it to settlement before returning. Returning
// it, storing it, passing it to `Task.FireAndForget`, or binding it and
// dropping it undisposed-to-settlement (`using var t := cb();` with no
// await) … is an escape sink, rejected by the same structural verification
// as the other sinks above. In a synchronous callee the invocation itself is
// rejected … a non-async body cannot await the returned task at all … so
// invoking a plain function-typed parameter whose invocation yields a
// Task-family value is a compile error there — declare the parameter
// `scoped` or make the callee `async`. The await-to-settlement requirement
// is per-path and admits fan-out: the returned tasks may be accumulated in a
// purely-local collection … provided every one of them is awaited to
// settlement before the callee returns on every path — awaiting an
// aggregation helper over that collection (`await Task.WhenAll(pending)`,
// [§30.10]) discharges all of its inputs, since the aggregate completes only
// after every input settles." Appendix C.6 states the matching invariant:
// "The task returned by invoking a call-bounded callback is awaited to
// settlement, on every path, before the callee returns — directly or via an
// aggregation helper over a purely-local collection; abandonment is an
// escape sink, and in a synchronous callee the invocation itself is
// rejected."
//
// This obligation was NOT previously verified: the pre-existing §28.14 sink
// checks (callBoundedScan / #1419, extended to a lambda's own parameters by
// #1432) classify what a call-bounded callback VALUE may be used for
// (stored / returned / forwarded) but had no model for what happens to the
// TASK produced by INVOKING one. checkCallBoundedTaskSettlement (new, run
// right after callBoundedScan in MethodOwnership::run()) closes that gap
// with a structural, join-aware walk (if/elif/else precise; while/for/try
// conservative — a discharge reachable only inside a maybe-zero-trip loop
// body, or only on some exception-unwind path, is not relied on) mirroring
// sema::DefiniteAssignment's own joinLiveBranches shape.
//
// Scope, per CLAUDE.md's decline-with-reasons discipline (recorded, not
// silent):
//  - Does NOT descend into a nested LambdaExpr's own body (mirrors
//    checkTaskScopeConsumption's own scope) — a lambda's async-ness may
//    differ from the enclosing callable's; extending there is left to a
//    follow-up issue if evidence demands it.
//  - Recognizes the invocation only at a fixed set of STATEMENT-level
//    positions (LocalDecl initializer, assignment RHS, return/discard
//    value, a fan-out `.Add(...)` argument, a bare expression-statement, an
//    `await` operand) — an invocation reached only through some OTHER
//    nested expression position is not specifically recognized. The same
//    "provable subset" posture v1 admission bound #3 already states for
//    this section.
//  - `throw` is not checked against this obligation (mirrors
//    sema::DefiniteAssignment's own out-param `throw` exemption).
//  - #1427 (the deferred "unbounded" composition tier) is untouched — see
//    call_bounded_2814's still-rejected `escaping_capturing_closure_rejected`
//    control.

#include <string>

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

bool diagContains(const bezel::test::OwnershipResult& r,
                  const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

}  // namespace

// ===========================================================================
// Synchronous callee: the invocation itself is rejected (§28.14 v0.179 (2)).
// ===========================================================================

// The invocation is rejected even though the resulting task is never used
// for anything — a non-async body has no consuming position for it at all.
BEZEL_TEST(call_bounded_task_abandonment_1527, sync_callee_invocation_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static method Sync(cb: () -> Task) "
        "    var t := cb(); "
        "  end Sync; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "synchronous callee"));
    BEZEL_EXPECT_TRUE(diagContains(r, "`cb` `scoped`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§28.14, v0.179"));
}

// The diagnostic fires at the invocation site regardless of the surrounding
// syntax — a bare, unbound invocation is the same rejection.
BEZEL_TEST(call_bounded_task_abandonment_1527, sync_callee_bare_invocation_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static method Sync(cb: () -> Task) "
        "    cb(); "
        "  end Sync; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "synchronous callee"));
}

// A sync callee whose callback returns a NON-task type is the pre-existing
// #1419/#1432 regime, entirely unaffected (regression control — mirrors
// call_bounded_2814's own bind_to_local_and_invoke_clean).
BEZEL_TEST(call_bounded_task_abandonment_1527,
           sync_callee_non_task_callback_unaffected) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static method Sync(cb: () -> int): int "
        "    var t := cb(); "
        "    return t; "
        "  end Sync; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ===========================================================================
// Async callee, ACCEPTED shapes.
// ===========================================================================

// Bind then await — the canonical direct idiom.
BEZEL_TEST(call_bounded_task_abandonment_1527, async_bind_then_await_clean) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static async method Ok(cb: () -> Task): Task "
        "    var t := cb(); "
        "    await t; "
        "  end Ok; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Inline `await cb()` — never bound to a local at all.
BEZEL_TEST(call_bounded_task_abandonment_1527, async_inline_await_clean) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static async method Ok(cb: () -> Task): Task "
        "    await cb(); "
        "  end Ok; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Fan-out: accumulate into a purely-local collection, discharge every
// element with `await Task.WhenAll(pending)` (§28.14 v0.179's own named
// idiom).
BEZEL_TEST(call_bounded_task_abandonment_1527, async_fanout_whenall_clean) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static async method Ok(cb: () -> Task): Task "
        "    var pending: Array<Task> := []; "
        "    pending.Add(cb()); "
        "    pending.Add(cb()); "
        "    await Task.WhenAll(pending); "
        "  end Ok; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Fan-out built incrementally across loop iterations — the realistic shape
// of "spawn one per element, then join" — is admitted: an `.Add` inside a
// (possibly zero-trip) loop body either ran (a real, still-owed member) or
// didn't (nothing to owe), unlike a direct discharge, which the loop's
// uncertain trip count CANNOT certify.
BEZEL_TEST(call_bounded_task_abandonment_1527, async_loop_fanout_whenall_clean) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static async method Ok(cb: () -> Task, xs: Array<int>): Task "
        "    var pending: Array<Task> := []; "
        "    for var x in xs do "
        "      pending.Add(cb()); "
        "    end; "
        "    await Task.WhenAll(pending); "
        "  end Ok; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// if/else BOTH branches await the same bound task on every live path — the
// join-based check must not over-reject this (precision control).
BEZEL_TEST(call_bounded_task_abandonment_1527, async_if_else_both_branches_await_clean) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static async method Ok(cb: () -> Task, flag: bool): Task "
        "    var t := cb(); "
        "    if flag then "
        "      await t; "
        "    else "
        "      await t; "
        "    end; "
        "  end Ok; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A `scoped` callback is an entirely different §28.14 admission (storage-
// bounded, not call-bounded) — invoking it and never awaiting the resulting
// task is NOT this section's concern at all (accept-control: the new check
// must not fire for a `scoped` parameter).
BEZEL_TEST(call_bounded_task_abandonment_1527,
           scoped_callback_task_unaffected) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public async method Install(scoped cb: () -> Task): Task "
        "    var t := cb(); "
        "  end Install; "
        "  destructor end; "
        "end C;");
    BEZEL_EXPECT_FALSE(diagContains(r, "call-bounded"));
    BEZEL_EXPECT_FALSE(diagContains(r, "§28.14, v0.179"));
}

// ===========================================================================
// Async callee, REJECTED shapes (the named escape sinks).
// ===========================================================================

// Falling through to the method's end without ever awaiting — the most
// direct "not awaited to settlement" violation.
BEZEL_TEST(call_bounded_task_abandonment_1527, async_never_awaited_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static async method Bad(cb: () -> Task): Task "
        "    var t := cb(); "
        "  end Bad; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "not awaited to settlement"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§28.14, v0.179"));
    BEZEL_EXPECT_TRUE(diagContains(r, "Task.WhenAll"));
}

// The `using var t := cb(); with no await` shape the spec names verbatim.
BEZEL_TEST(call_bounded_task_abandonment_1527,
           async_using_var_bind_and_drop_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static async method Bad(cb: () -> Task): Task "
        "    using var t := cb(); "
        "  end Bad; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "not awaited to settlement"));
}

// Returning before awaiting.
BEZEL_TEST(call_bounded_task_abandonment_1527, async_return_before_await_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static async method Bad(cb: () -> Task): Task "
        "    var t := cb(); "
        "    return; "
        "  end Bad; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "not awaited to settlement"));
    BEZEL_EXPECT_TRUE(diagContains(r, "before this return"));
}

// Returning the task itself is the named "returned" sink.
BEZEL_TEST(call_bounded_task_abandonment_1527, async_return_the_task_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static async method Bad(cb: () -> Task): Task "
        "    return cb(); "
        "  end Bad; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "is returned"));
}

// `Task.FireAndForget` on the bound task — the named sink verbatim.
BEZEL_TEST(call_bounded_task_abandonment_1527, async_fire_and_forget_bound_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static async method Bad(cb: () -> Task): Task "
        "    var t := cb(); "
        "    Task.FireAndForget(t); "
        "  end Bad; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "passed to `Task.FireAndForget`"));
}

// `Task.FireAndForget(cb())` inline — the same sink, unbound spelling.
BEZEL_TEST(call_bounded_task_abandonment_1527, async_fire_and_forget_inline_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static async method Bad(cb: () -> Task): Task "
        "    Task.FireAndForget(cb()); "
        "  end Bad; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "passed to `Task.FireAndForget`"));
}

// Storing the task in a field — "storing it" per the spec text.
BEZEL_TEST(call_bounded_task_abandonment_1527, async_store_in_field_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  private _t: Task?; "
        "  public async method Bad(cb: () -> Task): Task "
        "    _t := cb(); "
        "  end Bad; "
        "  destructor end; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "is stored"));
}

// A bare, unbound, never-awaited invocation used (and dropped) as a plain
// statement — the most direct "drop" spelling.
BEZEL_TEST(call_bounded_task_abandonment_1527, async_bare_drop_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static async method Bad(cb: () -> Task): Task "
        "    cb(); "
        "  end Bad; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "dropped without being awaited"));
}

// Reassigning a still-open local to a fresh invocation before awaiting the
// old one — a drop-on-overwrite spelling of "binding it and dropping it".
BEZEL_TEST(call_bounded_task_abandonment_1527,
           async_drop_by_reassignment_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static async method Bad(cb: () -> Task): Task "
        "    var t := cb(); "
        "    t := cb(); "
        "    await t; "
        "  end Bad; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "dropped by reassignment"));
}

// if/else where only ONE branch awaits — "on every path" is violated; the
// join must NOT clear the obligation from the guard-false (no-else) or
// non-awaiting arm.
BEZEL_TEST(call_bounded_task_abandonment_1527,
           async_if_only_one_branch_awaits_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static async method Bad(cb: () -> Task, flag: bool): Task "
        "    var t := cb(); "
        "    if flag then "
        "      await t; "
        "    end; "
        "  end Bad; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "not awaited to settlement"));
}

// Fan-out into a collection that is never discharged via `Task.WhenAll` —
// accumulating alone does not satisfy the obligation.
BEZEL_TEST(call_bounded_task_abandonment_1527,
           async_fanout_without_whenall_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static async method Bad(cb: () -> Task): Task "
        "    var pending: Array<Task> := []; "
        "    pending.Add(cb()); "
        "  end Bad; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "not awaited to settlement"));
}
