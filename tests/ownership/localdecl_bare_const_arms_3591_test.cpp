// Ownership — narrow #3591: seven of the eighteen LocalDecl-bare
// `dynamic_cast<const ast::LocalDecl*>` sites in Ownership_Checks.cpp were
// APPLICABLE-AND-WRONG — each is a general statement walker whose entry point
// (a per-statement `dynamic_cast<... LocalDecl*>` dispatch) silently skipped
// every §21.1 runtime `const` local, even though §21.1 explicitly admits a
// `const` to hold everything each walker needed to see: a value-typed result
// of an expression that CONTAINS the pattern under check, a borrow of a
// scope-bound source, or the settled result of an await. Six prior instances
// of exactly this defect shape were found and fixed one at a time (#2802,
// #3375, #3402, #3416, #3417, #3427); this issue (#3591) is the ratchet that
// finds the next batch instead of waiting for each to be independently
// rediscovered as its own leak/UAF/false-reject report.
//
// Each row below is the CONST twin of an already-legal `var` shape (cited in
// the row's own comment — several are already pinned by an existing test in
// this suite); every row was measured on this lane's HEAD to diverge from the
// `var` spelling before the fix and to match it after. Fail-without / pass-
// with, and the exact filters run, are recorded in the fixing commit's body.
//
// Spec revision v0.286.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;
using bezel::test::OwnershipResult;

// ===========================================================================
// Site 1 (fix, was Ownership_Checks.cpp:2088) — collectIdentifiersInStmt.
//
// §31.1 "Exception ownership": a caught exception is a borrow whose lifetime
// is the catch arm; a closure that escapes the arm and captures it is
// rejected ("returned closure captures the caught exception"). The `var`
// spelling of this exact shape is already pinned by
// catch_escape_closure.return_closure_capturing_binding_diagnosed_31_1
// (tests/ownership/catch_escape_test.cpp) for an EXPRESSION-bodied lambda;
// this row is the missing STATEMENT-bodied-lambda / `const`-capture twin —
// collectIdentifiersInStmt (feeding collectLambdaIdentifiers) never walked a
// `const` local's initializer, so an identifier referenced ONLY inside a
// `const` binding's initializer inside a block-bodied lambda was never
// collected as a capture at all.
//
// Fail-without/pass-with (measured on this lane's HEAD): before the fix this
// compiled with NO diagnostic; after the fix it is rejected with the same
// §31.1 message the `var m` spelling already draws.
BEZEL_TEST(localdecl_bare_const_arms_3591,
           catch_exception_const_capture_stmt_lambda_rejected_31_1) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public method Handle(): () -> string "
        "    try DoWork(); "
        "    catch var ex: Exception => "
        "      return lambda() => do const m := ex.Message; return m; end; "
        "    end; "
        "    return lambda() => \"\"; "
        "  end Handle; "
        "  private method DoWork() end DoWork; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "returned closure captures the caught exception"));
}

// ===========================================================================
// Site 2 (fix, was Ownership_Checks.cpp:2504) — scanCapturedOwnedConsumeStmt.
//
// §28.3 v0.184 "Captured owned bindings": a closure may not consume, reassign
// or dispose a captured owned binding — a marked transfer `Sink(owned r)`
// inside a closure is rejected. The `var y := Sink(owned r);` shape is
// already pinned by
// own_captured_owned_1958.marked_transfer_in_closure_rejected_28_3
// (tests/ownership/captured_owned_consume_closure_1958_test.cpp); this row is
// its `const` twin. scanCapturedOwnedConsumeStmt never scanned a `const`
// local's initializer, so the same consuming call inside a `const`
// declaration was never scanned at all — the exact #1958 double-dispose
// shape, one binding-form later.
//
// Fail-without/pass-with: before the fix this compiled with NO diagnostic
// (rc=0, a full AOT link succeeded — the double-dispose is a runtime UAF);
// after the fix it is rejected with the same §28.3 v0.184 message.
BEZEL_TEST(localdecl_bare_const_arms_3591,
           captured_owned_const_marked_transfer_rejected_28_3) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class R "
        "  public V: int := 0; "
        "  public method Get(): int return V; end Get; "
        "  destructor end destructor; "
        "end R; "
        "class C "
        "  public static method Sink(owned r: R): int "
        "    r.Dispose(); return 0; "
        "  end Sink; "
        "  public static method Main(): int "
        "    using var r := new R(); "
        "    var f := lambda() => do const y := Sink(owned r); return y; end; "
        "    return f(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "captured owned binding"));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "\xc2\xa7""28.3, v0.184"));
}

// ===========================================================================
// Site 3 (fix, was Ownership_Checks.cpp:6235) — collectBorrowUsesStmt.
//
// §28.3 v0.180 (#1741) consume-site freeze: consuming a source (here `move
// w`) while a live borrow of it (held by `y`) has a reachable LATER use is
// rejected. `y`'s later use is the read inside `z`'s initializer
// (`y.V`); collectBorrowUsesStmt's `LocalDecl` arm is what records that read
// into the freeze's liveness pre-scan (`recordBorrowUseExpr`) — the `const`
// twin never fed the pre-scan, so the freeze could not see the later use.
//
// Fail-without/pass-with: `var z := y.V;` is already correctly rejected
// ("the borrow held by `y` is still live") — this row pins the identical
// `const z := y.V;` form, which compiled with NO diagnostic before the fix.
BEZEL_TEST(localdecl_bare_const_arms_3591,
           consume_freeze_later_use_via_const_rejected_28_3) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Widget "
        "  public V: int := 0; "
        "  destructor end destructor; "
        "end Widget; "
        "class C "
        "  public static method Main(): int "
        "    var w := new Widget(); "
        "    var y := w; "
        "    using var w2 := move w; "
        "    const z := y.V; "
        "    return z; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "the borrow held by `y` is still live"));
}

// CONTROL: the `var` twin of the row above stays rejected (regression guard —
// this fix must not change the already-correct `var` behavior).
BEZEL_TEST(localdecl_bare_const_arms_3591,
           consume_freeze_later_use_via_var_still_rejected_28_3) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Widget "
        "  public V: int := 0; "
        "  destructor end destructor; "
        "end Widget; "
        "class C "
        "  public static method Main(): int "
        "    var w := new Widget(); "
        "    var y := w; "
        "    using var w2 := move w; "
        "    var z := y.V; "
        "    return z; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "the borrow held by `y` is still live"));
}

// ===========================================================================
// Site 4 (fix, was Ownership_Checks.cpp:8877) — awaitOperandIsOwningLocation.
//
// §30.5 Pattern A: awaiting a class-typed `Task<T>` requires a
// syntactically-visible owning location. `arr[0]` on a non-owning
// `Array<Task<Res>>` is NOT an owning-collection slot (indexerSetterTakes
// Owned is false for a borrowing array), so `var t := arr[0]; await t;` is
// correctly rejected. The unconditional `if (!ld) return true;` fallback for
// a LocalConstDecl-backed symbol admitted `const t := arr[0]; await t;`
// regardless of what the const's initializer actually was.
//
// Fail-without/pass-with: before the fix this compiled with NO Pattern-A
// diagnostic; after the fix it draws the same rejection the `var` spelling
// does.
BEZEL_TEST(localdecl_bare_const_arms_3591,
           await_pattern_a_const_array_element_alias_rejected_30_5) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Res end Res; "
        "class C "
        "  public static async method Get(arr: Array<Task<Res>>): Task<int> "
        "    const t := arr[0]; "
        "    var r := await t; "
        "    return r; "
        "  end Get; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "awaiting a class-typed `Task<T>` (Pattern A) requires"));
}

// CONTROL: the `var` twin stays rejected identically.
BEZEL_TEST(localdecl_bare_const_arms_3591,
           await_pattern_a_var_array_element_alias_still_rejected_30_5) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Res end Res; "
        "class C "
        "  public static async method Get(arr: Array<Task<Res>>): Task<int> "
        "    var t := arr[0]; "
        "    var r := await t; "
        "    return r; "
        "  end Get; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "awaiting a class-typed `Task<T>` (Pattern A) requires"));
}

// ===========================================================================
// Site 5 (fix, was Ownership_Checks.cpp:10024) —
// checkDeferredActivationTempSitesStmt.
//
// §28.3 / stdlib §5 (#1917): a lazy `Select` selector that mints a fresh
// owned value per element launders it out through the borrow-typed projected
// sequence and is rejected — REGARDLESS of how deep the `Select` call sits in
// an outer expression tree, because checkDeferredActivationTempSitesExpr's
// per-CallExpr checks (checkLazySelectMintingSelector) fire on every CallExpr
// the walker reaches, not just an escaping top-level one. `const n := items.
// Select(lambda(x) => new Box(x)).Count();` binds a VALUE (`.Count()`
// returns `int`, §21.1-admissible), but nothing ever walked into the nested
// `Select(...)` call to find the minting selector, because the STATEMENT
// itself was never dispatched at all.
//
// Fail-without/pass-with: before the fix this compiled with NO diagnostic
// (each `new Box` leaks, unreachable by any consumer); after the fix it
// draws the same "lazy `Select` selector that mints" rejection the `var n`
// spelling already draws.
BEZEL_TEST(localdecl_bare_const_arms_3591,
           lazy_select_minting_selector_via_const_wrapper_rejected_28_3) {
    auto r = parseAndCheckOwnership(
        "module M; import System.Collections.Linq; "
        "class Box "
        "  public V: int; "
        "  public constructor(x: int) V := x; end; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    const xs: Array<int> := [1, 2, 3]; "
        "    const n := xs.Select(lambda(x) => new Box(x)).Count(); "
        "    return n; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "a lazy `Select` selector that mints"));
}

// CONTROL: the `var n` twin stays rejected identically.
BEZEL_TEST(localdecl_bare_const_arms_3591,
           lazy_select_minting_selector_via_var_wrapper_still_rejected_28_3) {
    auto r = parseAndCheckOwnership(
        "module M; import System.Collections.Linq; "
        "class Box "
        "  public V: int; "
        "  public constructor(x: int) V := x; end; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    const xs: Array<int> := [1, 2, 3]; "
        "    var n := xs.Select(lambda(x) => new Box(x)).Count(); "
        "    return n; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "a lazy `Select` selector that mints"));
}

// ===========================================================================
// Site 6 (fix, was Ownership_Checks.cpp:15135) — the §28.14 call-bounded
// task-abandonment walk's `scanOne`.
//
// §28.14 v0.179: a task minted by invoking a call-bounded callback parameter
// must be awaited to settlement before the method returns. `var t := cb();
// var r := await t;` DOES settle it — `await t` is the discharge — but
// `scanOne`'s missing `const` arm meant `const r := await t;` never called
// `processValueExpr`, so the discharge (`dischargeAwaitOperand`) never ran:
// the method reached its end with `t`'s item still marked open, and drew a
// FALSE "not awaited to settlement" rejection.
//
// Fail-without/pass-with: before the fix `const r := await t;` was WRONGLY
// rejected with "not awaited to settlement"; after the fix it compiles clean,
// matching `var r := await t;`.
BEZEL_TEST(localdecl_bare_const_arms_3591,
           call_bounded_await_discharge_via_const_accepted_28_14) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public static async method Ok(cb: () -> Task<int>): Task "
        "    var t := cb(); "
        "    const r := await t; "
        "    _ := r; "
        "  end Ok; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL: the `var r` twin stays accepted identically.
BEZEL_TEST(localdecl_bare_const_arms_3591,
           call_bounded_await_discharge_via_var_still_accepted_28_14) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public static async method Ok(cb: () -> Task<int>): Task "
        "    var t := cb(); "
        "    var r := await t; "
        "    _ := r; "
        "  end Ok; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Regression control: an UNAWAITED call-bounded task (never discharged at
// all) must still be rejected — the fix must not make `scanOne` blind to a
// genuine abandonment.
BEZEL_TEST(localdecl_bare_const_arms_3591,
           call_bounded_never_awaited_still_rejected_28_14) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public static async method Bad(cb: () -> Task<int>): Task "
        "    var t := cb(); "
        "    _ := 0; "
        "  end Bad; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "not awaited to settlement"));
}

// ===========================================================================
// Site 7 (fix, was Ownership_Checks.cpp:15581) — taskScopeUsageScanStmt.
//
// stdlib §6.10 v0.175: `await L.Join()` is mandatory on the normal path
// unless `L` is DELEGATED (passed as a plain call argument — the callee may
// join it). `const x := Prog.Helper(scope);` delegates `scope` exactly as
// `var x := Prog.Helper(scope);` does, but taskScopeUsageScanStmt's missing
// `const` arm meant the whole statement — and the delegation it contains —
// was never scanned, so `sawDelegation` stayed false and the scope's
// declaration drew a FALSE "await scope.Join() is missing" rejection.
//
// Fail-without/pass-with: before the fix the `const x := ...` form was
// WRONGLY rejected; after the fix it compiles clean, matching `var x := ...`.
BEZEL_TEST(localdecl_bare_const_arms_3591,
           taskscope_delegation_via_const_accepted_6_10) {
    auto r = parseAndCheckOwnership(
        "module M; import System.Threading; "
        "class Prog "
        "  public static method Helper(s: TaskScope): int return 1; end Helper; "
        "  public static async method Run(): Task "
        "    await using var scope := new TaskScope(); "
        "    const x := Prog.Helper(scope); "
        "    _ := x; "
        "  end Run; "
        "end Prog;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL: the `var x` twin stays accepted identically.
BEZEL_TEST(localdecl_bare_const_arms_3591,
           taskscope_delegation_via_var_still_accepted_6_10) {
    auto r = parseAndCheckOwnership(
        "module M; import System.Threading; "
        "class Prog "
        "  public static method Helper(s: TaskScope): int return 1; end Helper; "
        "  public static async method Run(): Task "
        "    await using var scope := new TaskScope(); "
        "    var x := Prog.Helper(scope); "
        "    _ := x; "
        "  end Run; "
        "end Prog;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Regression control: a TaskScope that is neither joined nor delegated at all
// must still be rejected — the fix must not make the scan blind to a genuine
// missing join.
BEZEL_TEST(localdecl_bare_const_arms_3591,
           taskscope_never_joined_or_delegated_still_rejected_6_10) {
    auto r = parseAndCheckOwnership(
        "module M; import System.Threading; "
        "class Prog "
        "  public static async method Run(): Task "
        "    await using var scope := new TaskScope(); "
        "  end Run; "
        "end Prog;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "Join()"));
}

// ===========================================================================
// Site 8 (fix, was Ownership_Checks.cpp:12089, review follow-up) —
// suspSourceIsFrameResident + walkSuspStmts's LocalConstDecl registration.
//
// §30.1 / §21.1: a borrow-bearing local (a span) held live across an `await`
// is admitted only when its borrow's source is FRAME-RESIDENT (an owned
// local / `using var` / `owned` parameter the async state-machine frame
// itself owns). §21.1: a `const` borrow "carries the initializer's
// source-liveness constraints, exactly as a plain unmarked `var` alias" — so
// a `const` span sourced from a `using var` buffer must be admitted exactly
// as the `var` spelling is.
//
// THE DEFECT (found by review, corrected from this file's original —
// WRONG — "identical answer" claim): `const sp := buf.AsSpan();` was never
// registered in `suspCands_` at all (walkSuspStmts's LocalConstDecl arm did
// reads/crossing-tracking only), and `suspSourceIsFrameResident`'s own
// LocalDecl-only cast had no alias-recursion fallback for a const-rooted
// hop — so `var sp2 := sp;` (a further `var` alias of the const span) fell
// straight to "not frame-resident" and was FALSELY REJECTED, where the
// identical `var sp := buf.AsSpan(); var sp2 := sp;` twin (and the direct
// `var sp2 := buf.AsSpan();` form) both compile clean.
//
// Fail-without/pass-with: before the fix, binding the span via `const`
// anywhere in the alias chain drew "borrow-bearing local `sp2` is held live
// across a suspension point, but its borrow's source is not frame-resident"
// (§30.1/§29.4); after the fix it compiles clean, matching every-`var` twin.
BEZEL_TEST(localdecl_bare_const_arms_3591,
           suspend_across_await_const_span_alias_frame_resident_accepted_30_1) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public static async method Get(): Task<int> "
        "    using var buf := new OwningBuffer<int>(4); "
        "    const sp := buf.AsSpan(); "
        "    var sp2 := sp; "
        "    var d := await Task.Delay(1); _ := d; "
        "    return sp2.Length; "
        "  end Get; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL: the all-`var` twin stays accepted identically (regression guard).
BEZEL_TEST(localdecl_bare_const_arms_3591,
           suspend_across_await_var_span_alias_frame_resident_still_accepted_30_1) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public static async method Get(): Task<int> "
        "    using var buf := new OwningBuffer<int>(4); "
        "    var sp := buf.AsSpan(); "
        "    var sp2 := sp; "
        "    var d := await Task.Delay(1); _ := d; "
        "    return sp2.Length; "
        "  end Get; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Regression control: a span rooted at a genuinely CALLER-SIDE (non-frame-
// resident) source must still be rejected across a suspension — the fix
// must not widen admission beyond the `var` twin's.
BEZEL_TEST(localdecl_bare_const_arms_3591,
           suspend_across_await_const_span_caller_side_still_rejected_30_1) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public static async method Get(buf: OwningBuffer<int>): Task<int> "
        "    const sp := buf.AsSpan(); "
        "    var d := await Task.Delay(1); _ := d; "
        "    return sp.Length; "
        "  end Get; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "not frame-resident"));
}

// ===========================================================================
// Site 9 (fix, was Ownership_Checks.cpp:15677, review follow-up; UPGRADED
// from this file's original — WRONG — "no divergence" LOCALDECL-ONLY claim)
// — checkTaskScopeConsumption's outer `scan`.
//
// stdlib §6.10 v0.175: a TaskScope may only be consumed by `await using` —
// this file's original marker asserted a `const` alias of an already-
// `await using`-bound scope needed no such check because it "inherits
// nothing to track". That is true for the JOIN-TRACKING obligation, but
// review measured a REAL divergence on a DIFFERENT, independent rule this
// same scan enforces: ANY TaskScope-typed local bound other than `await
// using` is unconditionally rejected ("bound to a plain `var`") — and a
// `const` binding was never checked by that rule at all, so `const s2 :=
// scope;` silently created a second, entirely unchecked name for the same
// scope object where `var s2 := scope;` is correctly rejected.
//
// Fail-without/pass-with: before the fix `const s2 := scope;` compiled
// clean; after the fix it draws "`s2` is a TaskScope bound to a `const`",
// mirroring the pre-existing `var` rejection.
BEZEL_TEST(localdecl_bare_const_arms_3591,
           taskscope_const_alias_rejected_6_10) {
    auto r = parseAndCheckOwnership(
        "module M; import System.Threading; "
        "class Prog "
        "  public static async method Run(): Task "
        "    await using var scope := new TaskScope(); "
        "    const s2 := scope; "
        "    await scope.Join(); "
        "    _ := s2; "
        "  end Run; "
        "end Prog;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "TaskScope bound to a `const`"));
}

// CONTROL: the `var` twin was already rejected before this fix and stays so
// (regression guard, and the shape the fix's message is modelled on).
BEZEL_TEST(localdecl_bare_const_arms_3591,
           taskscope_var_alias_still_rejected_6_10) {
    auto r = parseAndCheckOwnership(
        "module M; import System.Threading; "
        "class Prog "
        "  public static async method Run(): Task "
        "    await using var scope := new TaskScope(); "
        "    var s2 := scope; "
        "    await scope.Join(); "
        "    _ := s2; "
        "  end Run; "
        "end Prog;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "TaskScope bound to a plain `var`"));
}
