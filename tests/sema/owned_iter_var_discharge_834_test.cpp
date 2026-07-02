// fix #834 / §29.2 "Iterator yield ownership": the iteration variable of a
// `for`/`await for` loop over an IOwningEnumerable<T> / IOwningAsyncEnumerable<T>
// is a PLAIN OWNED LOCAL and "must be discharged inside the loop body.
// Falling off the body without discharging is a compile error."
//
// Before the fix, the iteration variable was never seeded as Owned in the
// ownership dataflow, so dropping it without discharge was wrongly ACCEPTED —
// a silent leak for the sync form, and a runtime SIGSEGV for the async form.
// The fix seeds the iteration variable as Owned at the loop body entry (each
// iteration) when the iterable resolves to an owning enumerable, so the
// standard §28.3 owned-local discharge check rejects an undischarged variable.
//
// These tests pin: the undischarged sync/async reject, the discharged accepts
// (Dispose / move-out / using-var-on-entry), the partial-conditional reject
// (path-sensitive, interplaying with #867), and — the key guardrail — that a
// BORROWING for-loop (Array<T>/IEnumerable<T>, range) is NOT over-rejected,
// since its iteration variable is a borrow with no discharge obligation.
// // per §29.2, §28.3, #834

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

// A disposable Box class plus an iterator method returning IOwningEnumerable<Box>
// (the owning-enumeration / drain path). `body` is the for-loop body that each
// test varies. A valid Main entry point is supplied so the program is complete.
std::string withOwnedFor(const std::string& body) {
    return std::string(
        "module M; "
        "class Box "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Box; "
        "class C "
        "  public static method Gen(): IOwningEnumerable<Box> "
        "    yield return new Box(); "
        "  end Gen; "
        "  public static method Run(c: bool): int "
        "    for var x in Gen() do ") +
        body +
        "    end; "
        "    return 0; "
        "  end Run; "
        "  public static method Main(): int return Run(true); end Main; "
        "end C;";
}

// Async variant: an async iterator returning IOwningAsyncEnumerable<Box>,
// consumed by an `await for`. `body` varies per test.
std::string withOwnedAwaitFor(const std::string& body) {
    return std::string(
        "module M; "
        "class Box "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Box; "
        "class C "
        "  public static async method Gen(): IOwningAsyncEnumerable<Box> "
        "    yield return new Box(); "
        "  end Gen; "
        "  public static async method Run(c: bool): Task "
        "    await for var x in Gen() do ") +
        body +
        "    end; "
        "  end Run; "
        "  public static method Main(): int return 0; end Main; "
        "end C;";
}

// True iff some diagnostic names `x` as an owned iteration-variable that is
// implicitly disposed per-iteration (the §29.2/§28.3 v0.157 auto-drop warning
// that replaced the old hard discharge error — #1329).
bool hasIterVarAutoDropWarning(const bezel::test::OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("owned value in `x`") != std::string::npos &&
            d.message.find("implicitly disposed") != std::string::npos)
            return true;
    return false;
}

}  // namespace

// §29.2 / §28.3 v0.157 / #1329 / #834 (headline): the owned iteration variable
// not discharged in the body — auto-dropped per-iteration with a must-use
// WARNING (no hard error; that was the pre-v0.157 behaviour). Before #834 this
// was silently accepted (no warning, a silent leak). Before #1329 it was a hard
// error. Now it is a must-use warning: no errors, one auto-drop warning.
BEZEL_TEST(owned_iter_var_discharge_834, undischarged_sync_warns) {
    auto r = parseAndCheckOwnership(withOwnedFor(""));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasIterVarAutoDropWarning(r));
}

// §29.2 / #834: discharge via an explicit `x.Dispose()` on every iteration —
// must be ACCEPTED.
BEZEL_TEST(owned_iter_var_discharge_834, discharged_dispose_accepted) {
    auto r = parseAndCheckOwnership(withOwnedFor("x.Dispose();"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §29.2 / #834: discharge by moving the iteration variable into an owned sink
// (`using var y := move x;`) — the move discharges `x` (it becomes
// moved-from). Must be ACCEPTED.
BEZEL_TEST(owned_iter_var_discharge_834, discharged_moved_out_accepted) {
    auto r = parseAndCheckOwnership(withOwnedFor("using var y := move x;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §29.2 / #834: the spec's canonical "discharge on entry, then break" pattern —
// `using var c := x;` immediately on entry converts the per-edge obligation
// into scope-bound disposal that dominates every exit. Must be ACCEPTED.
BEZEL_TEST(owned_iter_var_discharge_834, using_var_on_entry_then_break_accepted) {
    auto r = parseAndCheckOwnership(withOwnedFor("using var c := x; break;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §29.2 / #834: discharge on ALL paths of a conditional — both branches
// dispose `x`. Must be ACCEPTED (an over-reject would wrongly flag this).
BEZEL_TEST(owned_iter_var_discharge_834, conditional_all_paths_discharged_accepted) {
    auto r = parseAndCheckOwnership(
        withOwnedFor("if c then x.Dispose(); else x.Dispose(); end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §29.2 / §28.3 v0.157 / #1329 / #834 (path-sensitive): discharge on only
// the `c` path — `x` is still-owned on the `c==false` fall-through. Must emit
// the auto-drop WARNING (not an error). The null-guarded per-iteration disposal
// fires on the fall-through path and is a no-op on the disposed path (slot is
// null after x.Dispose()).
BEZEL_TEST(owned_iter_var_discharge_834, conditional_partial_discharge_warns) {
    auto r = parseAndCheckOwnership(
        withOwnedFor("if c then x.Dispose(); end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasIterVarAutoDropWarning(r));
}

// §29.2 / §28.3 v0.157 / #1329 / #834: `break` without discharging `x` on
// the break path — `x` is still-owned when breaking. Must emit the auto-drop
// WARNING (emitLoopDisposals fires the iter-var scope on break). Not an error.
BEZEL_TEST(owned_iter_var_discharge_834, undischarged_break_warns) {
    auto r = parseAndCheckOwnership(
        withOwnedFor("if c then break; end; x.Dispose();"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasIterVarAutoDropWarning(r));
}

// §29.2 / §28.3 v0.157 / #1329 / #834 (async form): `await for` over an
// IOwningAsyncEnumerable<Box> with the iteration variable not discharged — must
// emit the auto-drop WARNING (not an error, matching the sync form).
BEZEL_TEST(owned_iter_var_discharge_834, undischarged_async_warns) {
    auto r = parseAndCheckOwnership(withOwnedAwaitFor(""));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasIterVarAutoDropWarning(r));
}

// §29.2 / #834 (async over-reject guard): the async iteration variable
// discharged via move-out — must be ACCEPTED.
BEZEL_TEST(owned_iter_var_discharge_834, discharged_async_accepted) {
    auto r = parseAndCheckOwnership(withOwnedAwaitFor("using var y := move x;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §29.2 / #834 (THE GUARDRAIL): a BORROWING for-loop over an Array<int>
// (IEnumerable<int>, default borrowing iteration) — the iteration variable is
// a borrow with NO discharge obligation. Must be ACCEPTED. An over-broad seed
// (keying on the for-loop alone rather than the owning-enumerable iterable
// type) would wrongly reject this — the most important regression to guard.
BEZEL_TEST(owned_iter_var_discharge_834, borrowing_for_loop_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    using var arr := new Array<int>(); "
        "    arr.Add(1); arr.Add(2); "
        "    var total := 0; "
        "    for var x in arr do "
        "      total := total + x; "
        "    end; "
        "    return total; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §29.2 / #834 (guardrail): a range for-loop — the iteration variable is a
// value (int), no discharge obligation. Must be ACCEPTED.
BEZEL_TEST(owned_iter_var_discharge_834, range_for_loop_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 0..<3 do "
        "      t := t + i; "
        "    end; "
        "    return t; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §29.2 / §30.5 / §30.4 / #834 (the WhenAllOwned over-reject guard, stdlib
// §2.13): the §29.2 drain-and-await pattern — `for var task in arr.Drain() do
// results.Add(await task) end` over an OwningArray<OwningTask<T>> in a GENERIC
// async method. The owned iteration variable `task` is discharged by the
// `await task` inside the `Add(owned item)` owned-consuming context: awaiting
// an `OwningTask<...>` consumes the single-consumer task (§30.5/§30.4). This
// must be ACCEPTED. The seeding of `task` as owned (the #834 fix) made this
// program reachable; the consume of `await task` in an argument position (and
// for a generic OwningTask<T> whose result class kind awaitTaskKind cannot
// prove) must be recognized, else this conformance-clean program is wrongly
// rejected. This is the headline regression to guard against over-rejection.
BEZEL_TEST(owned_iter_var_discharge_834, drain_and_await_generic_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public static async method WhenAllOwned<T>("
        "    owned tasks: OwningArray<OwningTask<T>>"
        "  ): OwningTask<OwningArray<T>> where T: class "
        "    using var taskArray := tasks; "
        "    var results := new OwningArray<T>(capacity := taskArray.Count); "
        "    for var task in taskArray.Drain() do "
        "      results.Add(await task); "
        "    end; "
        "    return results; "
        "  end WhenAllOwned; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
