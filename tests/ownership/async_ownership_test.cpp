// Spec-derived tests for async ownership — async-readiness increment 3c.
//
// Two spec rules are exercised here, both ADDITIVE: they fire only on
// `async` method bodies, which do not yet codegen, so no compiling program
// changes behavior.
//
//   1. Across-suspend disposal (§30.5 "Result lifetime and task disposal",
//      §30.9 "Exception handling in async methods"). An owned local whose
//      live range crosses an `await` (a *suspend-local*) must be disposed on
//      every completion path of the async method — normal return, fault /
//      exception, abandonment, and cancellation. The state-machine frame
//      disposes the suspend-local on the abandonment / cancellation /
//      exception completion edges (modelled by synthetic completion edges and
//      the §28.3 exception-path discharge already in the ownership pass); the
//      NORMAL completion path is the author's obligation and is the same
//      §28.3 discharge requirement as for a synchronous owned local. So a
//      suspend-local dropped without discharge on the normal path is
//      diagnosed; one discharged on every path is clean. An activation-local
//      (no await in its live range) behaves exactly as a synchronous owned
//      local — dropped without discharge it is auto-dropped at scope end with
//      the §28.3 must-use warning (§28.12 synchronous implicit discharge, v0.184,
//      #1663), not rejected; the across-suspend annotation never changes a
//      non-suspend disposition.
//
//   2. Pattern A / Pattern B enforcement at the await SITE (§30.5
//      "Consumer-side rules"; Appendix C.6). For `await <task>` where the
//      task is `Task<T>` with class T (Pattern A), the awaited task
//      expression must occupy a syntactically-visible owning location
//      (§30.5: a `using var`-bound local, an `owned` parameter, an `owned`
//      field, an owning-collection slot, a borrowing-iteration variable, a
//      Pattern-A `return await Producer()`), OR (v0.182, §30.5/§28.3, #1570)
//      be an UNBOUND producer call whose borrowed result is confined to the
//      same full expression (`Use(await Producer())`) — admitted because the
//      disposal of the unbound temporary task defers to the end of the full
//      expression, after the borrowing use. Only a borrow that ESCAPES the
//      full expression — bound to a `var`, stored, or a non-chained return —
//      remains rejected (`var x := await Producer();`). For `OwningTask<T>`
//      (Pattern B), the await must appear in an owned-consuming context
//      (§30.5: `using var`/`await using var` RHS, `var` RHS, owned-field /
//      owned-slot assignment RHS, `owned` parameter argument, `owned` return,
//      owned `yield return`); otherwise the await is rejected. The Pattern B
//      await is also owned-consuming WITH RESPECT TO ITS OPERAND (§30.5 line
//      ~6110): awaiting an owned task local consumes that local, so a
//      straight-line second `await` of the same bound local is a read of a
//      consumed value — the statically-detectable slice of the §30.4 /
//      Appendix C.6 single-consumer rule (the general cross-method second
//      await is explicitly NOT statically preventable per Appendix C.6 and is
//      left to the runtime `InvalidOperationException`).

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;

namespace {

// Preamble: a Producer INTERFACE declaring methods that return the three task
// shapes (abstract — no body — so the producer contributes no arrow-body of its
// own to type-check; a `=> null` stub body is a §7 error, null into a
// non-nullable task type, now that §22.2 arrow bodies run the same §5.1/§7
// return-convertibility check the block form does, #1774), plus a class C
// hosting the async method under test. Only the awaiting method `M` is tested.
// `Conn` / `T` are class types so `OwningTask<Conn>` / `Task<Conn>` are the
// class-typed Pattern B / Pattern A forms. A `Use(c: Conn)` borrow-consuming
// helper sits alongside `M` for the argument-position tests.
std::string wrap(const std::string& body, const std::string& sig = "M(): Task") {
    return std::string("module M; "
        "class Conn end Conn; class T end T; "
        "interface Producer "
        "  method MakeOwned(): OwningTask<Conn>; "
        "  method MakeShared(): Task<Conn>; "
        "  method MakeUnit(): Task; "
        "end Producer; "
        "class C(p: Producer) "
        "  public method Use(c: Conn) end Use; "
        "  public method Sink(owned c: Conn) c.Dispose(); end Sink; "
        "  public async method ") + sig + " " + body + " end M; "
        "end C;";
}

} // namespace

// ---------------------------------------------------------------------------
// (1) Across-suspend disposal (§30.5 / §30.9).
// ---------------------------------------------------------------------------

// An owned local whose live range crosses an `await` (a suspend-local) and is
// dropped on the normal-completion path without discharge is diagnosed — the
// same §28.3 obligation as a synchronous owned local, now confirmed to apply
// to async bodies. // per §30.5, §30.9, §28.3
BEZEL_TEST(async_own_suspend, suspend_local_dropped_on_normal_path_diagnosed) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new T(); "
        "await p.MakeUnit(); "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// The same suspend-local discharged on every path (explicit Dispose after the
// await) is clean. // per §30.5, §28.3
BEZEL_TEST(async_own_suspend, suspend_local_discharged_clean) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new T(); "
        "await p.MakeUnit(); "
        "x.Dispose(); "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A `using var` suspend-local is clean — implicit discharge on every path,
// including the abandonment / cancellation / exception completion edges. The
// across-suspend annotation never converts a using-var into a leak.
// // per §30.5
BEZEL_TEST(async_own_suspend, using_var_suspend_local_clean) {
    auto r = parseAndCheckOwnership(wrap(
        "using var x := new T(); "
        "await p.MakeUnit(); "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// An ACTIVATION-local owned value — one whose live range does NOT cross any
// await — behaves exactly as a synchronous owned local: dropped without
// discharge it is AUTO-DROPPED at scope end with the §28.3 must-use *warning*
// (no error), taking the synchronous Dispose() path (§28.12 "implicit discharge
// is synchronous", v0.184, #1663). It never reaches a suspend point, so it is
// created-and-disposed within one resume activation and is exactly-once-safe;
// the across-suspend machinery must not convert this into a hard reject (its
// pre-#1663 behavior). A local that DOES cross an await stays the reject — see
// suspend_local_dropped_on_normal_path_diagnosed above. // per §28.12, §28.3
BEZEL_TEST(async_own_suspend, activation_local_drop_auto_dropped_1663) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new T(); "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// An activation-local owned value that IS discharged is clean — identical to
// the synchronous behavior. // per §28.3
BEZEL_TEST(async_own_suspend, activation_local_discharged_clean) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new T(); "
        "x.Dispose(); "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A suspend-local that is owned AT an await but discharged in a LATER block
// (here on both arms of an if after the await) is clean — the synthetic
// completion edges (abandonment/cancellation) give the still-owned local the
// frame-disposal treatment, so they do NOT manufacture a false leak, while the
// real normal path discharges it. Guards that injecting completion edges into
// the analysed CFG does not over-reject. // per §30.5, §30.9
BEZEL_TEST(async_own_suspend, owned_at_await_discharged_in_later_block_clean) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new T(); "
        "await p.MakeUnit(); "
        "if true then x.Dispose(); else x.Dispose(); end; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A suspend-local owned at an await and dropped on the normal path even though
// control flows through a later block (an if whose arms do not discharge x) is
// still diagnosed — the completion edges must not MASK the real normal-path
// leak. The if's branches contain only an unrelated statement, so x is never
// discharged. // per §30.5, §30.9, §28.3
BEZEL_TEST(async_own_suspend, owned_at_await_dropped_through_later_block_diagnosed) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new T(); "
        "await p.MakeUnit(); "
        "if true then var z := 1; else var z := 2; end; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "would be dropped here without disposal") ||
        hasErrorMessage(r, "is implicitly disposed"));
}

// ---------------------------------------------------------------------------
// (2a) Pattern A await-site (§30.5 Consumer-side; Appendix C.6).
// ---------------------------------------------------------------------------

// Pattern A: awaiting a class-typed `Task<T>` via a `using var`-bound task
// local is the canonical admitted owning location — clean. // per §30.5
BEZEL_TEST(async_own_patternA, await_using_var_bound_task_clean) {
    auto r = parseAndCheckOwnership(wrap(
        "using var t := p.MakeShared(); "
        "var r := await t; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Pattern A: a bare producer call awaited into a `var` binding occupies NO
// admitted owning location — the borrowed result would have no source.
// Diagnosed. // per §30.5, Appendix C.6
BEZEL_TEST(async_own_patternA, await_bare_producer_into_var_diagnosed) {
    auto r = parseAndCheckOwnership(wrap(
        "var r := await p.MakeShared(); "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "Pattern A") ||
        hasErrorMessage(r, "borrowed result"));
}

// Pattern A (v0.182, §30.5/§28.3, #1570): a bare producer call awaited in an
// argument position IS now admitted — `p.MakeShared()` is an unbound owned
// temporary, and its borrowed result is consumed by `Use` within the same
// full expression (the statement), so the temporary's disposal defers to the
// end of that expression, after the borrowing use — exactly the
// `Process(await Producer())` shape §30.5 names as the admitted form. Through
// v0.181 this was rejected outright; this test previously asserted the
// pre-v0.182 rejection and is updated to the new admission. Clean.
// // per §30.5, §28.3
BEZEL_TEST(async_own_patternA, await_bare_producer_in_arg_confined_clean) {
    auto r = parseAndCheckOwnership(wrap(
        "Use(await p.MakeShared()); "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Pattern A (v0.182): a bare producer call awaited as a standalone statement
// (its borrow used nowhere) is trivially confined to the full expression —
// zero uses is confined by definition — so it is admitted. // per §30.5, §28.3
BEZEL_TEST(async_own_patternA, await_bare_producer_bare_statement_clean) {
    auto r = parseAndCheckOwnership(wrap(
        "await p.MakeShared(); "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Pattern A (v0.190 SB-01, §28.3 "Task-family calls produce owned frames" +
// §30.5 "A reference to a plain owned local of static type `Task<T>`", line
// 7351, resolving #1533; issue #1786 SB01-A/C1). A `Task`-family CALL now MINTS
// an OWNED task at the call site (SB01-A), so `var t := p.MakeShared();` is a
// plain owned task local — an ADMITTED Pattern A owning location (SB01-C1): the
// local's scope bounds the borrow the await produces, and the §28.3 consume-
// site freeze protects it. Awaiting it is BORROW-await (§30.5 — the await
// neither consumes nor disposes a `Task<T>` operand), so `t` keeps its ordinary
// §28.3 discharge and auto-drops at scope end (a must-use WARNING, not an
// error) — exactly the disposition of an admitted `using var` task local. FLIP
// (was rejected pre-v0.190 under the storage model, where a plain task call was
// a borrow with no admitted source). Clean. // per §28.3, §30.5 (v0.190)
BEZEL_TEST(async_own_patternA, await_plain_var_task_admitted_v0190) {
    auto r = parseAndCheckOwnership(wrap(
        "var t := p.MakeShared(); "
        "Use(await t); "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Pattern A (v0.182): the confinement admission does NOT extend to a borrow
// that escapes the full expression — binding the awaited bare producer's
// result to a `var` is still the classic rejection (this is the sibling of
// `await_bare_producer_into_var_diagnosed` above, confirming the escape rule
// is unchanged by the new admission). // per §30.5, §28.3
BEZEL_TEST(async_own_patternA, await_bare_producer_into_var_still_diagnosed) {
    auto r = parseAndCheckOwnership(wrap(
        "var r := await p.MakeShared(); "
        "Use(r); "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "Pattern A") ||
        hasErrorMessage(r, "borrowed result"));
}

// Pattern A: `return await Producer()` is the one admitted bare-producer
// position (§30.5 line ~6068) — the producer's task is owned by the enclosing
// state machine and the borrow flows into the outer Pattern A result slot.
// Clean. // per §30.5
BEZEL_TEST(async_own_patternA, return_await_producer_clean) {
    auto r = parseAndCheckOwnership(wrap(
        "return await p.MakeShared(); ", "M(): Task<Conn>"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Pattern A: the borrow produced by awaiting a `using var`-bound task local
// may flow to a non-owned argument (§30.5 line ~6080: `Process(await task)`).
// `Use(c: Conn)` borrows its argument — clean because the operand `t` is the
// admitted owning location. // per §30.5
BEZEL_TEST(async_own_patternA, await_bound_task_into_borrow_arg_clean) {
    auto r = parseAndCheckOwnership(wrap(
        "using var t := p.MakeShared(); "
        "Use(await t); "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Pattern A: awaiting an `owned` PARAMETER of type `Task<T>` is an admitted
// owning location (the parameter's lifetime spans the body) — clean.
// // per §30.5
BEZEL_TEST(async_own_patternA, await_owned_param_task_clean) {
    auto r = parseAndCheckOwnership(wrap(
        "var r := await t; "
        "move t; ",
        "M(owned t: Task<Conn>): Task"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Pattern A (§30.5 rule (a), v0.193): awaiting a PLAIN (non-`owned`) parameter
// of `Task<T>` (class T) is admitted — a parameter is a named binding whose
// scope is visible at the await site (§30.5: "A reference to a plain (non-
// `owned`) local or parameter"; the everyday `public async method Report(t:
// Task<Data>): Task` shape). Guards the bare-parameter half of the v0.193 rule.
// // per §30.5 rule (a) / C.6
BEZEL_TEST(async_own_patternA, await_plain_param_task_clean) {
    auto r = parseAndCheckOwnership(wrap(
        "var d := await t; ",
        "M(t: Task<Conn>): Task"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Pattern A (§30.5 rule (a), v0.193, #1800): a borrowed ALIAS of an admitted
// operand INHERITS its admission — `var t2 := t; var d := await t2;` where `t`
// is a plain borrowed parameter (itself an admitted operand). The alias is a
// borrow of the source operand; its own scope bounds the result borrow, so
// awaiting the alias is exactly awaiting the source. This is the exact defect
// #1800 records: without the fix the await of the alias is REJECTED (the
// LocalDecl-with-initializer operand fell to the plain-producer-bound reject
// branch). // per §30.5 rule (a) / C.6
BEZEL_TEST(async_own_patternA, await_borrowed_alias_of_plain_param_clean) {
    auto r = parseAndCheckOwnership(wrap(
        "var t2 := t; "
        "var d := await t2; ",
        "M(t: Task<Conn>): Task"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Pattern A (§30.5 rule (a), v0.193, #1800): a borrowed alias of a `using var`-
// bound task local is likewise admitted — the alias inherits the using-var's
// admission, and its own scope bounds the result borrow. // per §30.5 rule (a)
BEZEL_TEST(async_own_patternA, await_borrowed_alias_of_using_var_clean) {
    auto r = parseAndCheckOwnership(wrap(
        "using var t := p.MakeShared(); "
        "var t2 := t; "
        "var d := await t2; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Pattern A (§30.5 rule (a) alias chain, v0.193 #1800; v0.190 SB-01 #1786): a
// borrowed ALIAS inherits its source operand's admission. Under v0.190 a
// `Task`-family CALL mints an OWNED task (SB01-A), so `var t := p.MakeShared();`
// is now an ADMITTED plain owned task local (SB01-C1, §30.5 line 7351 —
// resolving #1533), and the borrowed alias `var t2 := t` of it INHERITS that
// admission: `await t2` borrows t2, which borrows t, whose scope bounds the
// result. `t` (borrow-awaited through the alias) auto-drops at scope end (a
// must-use warning), `t2` is a plain borrow with no obligation. FLIP: this was
// rejected pre-v0.190 (the plain task call was a borrow with no admitted
// source, so the alias inherited non-admission — the old #1672/#1533 boundary).
// Clean. // per §30.5 rule (a) / C.6, §28.3 (v0.190)
BEZEL_TEST(async_own_patternA, await_alias_of_producer_var_admitted_v0190) {
    auto r = parseAndCheckOwnership(wrap(
        "var t := p.MakeShared(); "
        "var t2 := t; "
        "Use(await t2); "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Pattern A: a non-class `Task<T>` (here `Task<int>`) carries no ownership
// obligation across the await boundary, so any await position is admitted —
// a bare `var r := await producer()` is clean. Guards that the Pattern A
// site rule is scoped to CLASS T only (§30.5 producer-side rules). // per §30.5
BEZEL_TEST(async_own_patternA, await_value_typed_task_any_position_clean) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public method MakeInt(): Task<int> => Task.FromResult(0); "
        "  public async method M(): Task "
        "    var r := await MakeInt(); "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// v0.190 SB-01 (§28.3 "Task-family calls produce owned frames", #1786) —
// caller-side classification: the flip's boundary cases.
// ---------------------------------------------------------------------------

// v0.190 SB-01 D2 (#1868): a plain owned task local that is NEVER awaited but
// held live ACROSS a suspend point is ADMITTED with auto-drop (was rejected
// pre-D2). §30.5 line 7341 says a plain owned task local "keeps its ordinary
// §28.3 discharge (auto-drop at scope end)"; that scope-end task disposal
// (bz_task_fire_and_forget) fires on EVERY reachable terminal edge — the normal
// fall-off, a synchronous throw / faulted-await re-raise (unwind), AND
// cancellation, which the runtime delivers as a fault on the awaited task that
// resumes the frame and unwinds through the same disposal. The frame is only
// freed from inside its own resume function, after those disposals run, so an
// owned task suspend-local needs no separate frame-teardown "completion edge"
// disposal, and its await-ness was never the soundness criterion (SB01-C1's
// borrow-await was one admitting signal; the minting decl itself is the other).
// This FLIPS the pre-D2 KEEP-REJECT — verified leak-clean under ASan/LSan on the
// normal, throw, and cancellation edges (async_own_task_across_suspend_1868
// codegen suite). A plain owned CLASS local across a suspend stays rejected
// (activation_local_drop / suspend_local_dropped tests). // per §28.3, §30.5, §28.12 (v0.190)
BEZEL_TEST(async_own_patternA, plain_owned_task_never_awaited_across_suspend_admitted_1868) {
    auto r = parseAndCheckOwnership(wrap(
        "var t := p.MakeShared(); "
        "await p.MakeUnit(); "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// SB01-D2 (#1868) companion: a plain owned CLASS local held live across a suspend
// point is STILL REJECTED — its only disposal is the normal/unwind scope path
// (the genuine frame-teardown gap a task's fire-and-forget disposal sidesteps),
// so admitting it would leak on the abandonment edge. Pins that D2 is scoped to
// TASK-family locals and did not weaken the class-local obligation
// (declInitializerIsOwnedTaskMint's type gate). // per §28.3, §30.5 (v0.190)
BEZEL_TEST(async_own_patternA, plain_owned_class_never_used_across_suspend_still_rejected_1868) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new T(); "
        "await p.MakeUnit(); "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "would be dropped here without disposal") ||
        hasErrorMessage(r, "is implicitly disposed"));
}

// SB01-D2 (#1868): a plain owned `OwningTask<T>` (Pattern B) local never awaited
// and held across a suspend is likewise admitted with auto-drop — its scope-end
// task disposal (with the per-T owned-result disposer) covers the same reachable
// edges. Guards that the D2 admission is not narrowed to Pattern A. // per §30.5 (v0.190)
BEZEL_TEST(async_own_patternA, plain_owned_owningtask_never_awaited_across_suspend_admitted_1868) {
    auto r = parseAndCheckOwnership(wrap(
        "var t := p.MakeOwned(); "
        "await p.MakeUnit(); "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// SB01-D2 (#1868) IDENTITY GUARD: a USER class literally NAMED `OwningTask` (not
// a reserved name — §3.4 only reserves primitive aliases) held live across a
// suspend is STILL REJECTED. The D2 admission keys on builtin-task IDENTITY
// (declInitializerIsOwnedTaskMint: no user TypeDecl), not on the name-head string
// alone, so a user type that happens to share the name is an ordinary owned CLASS
// local and keeps the §28.3 suspend-local reject. Without the identity gate the
// name-head match would wrongly admit it and leak on the abandonment edge (it has
// no task fire-and-forget disposal). // per §30.5, §28.3 (v0.190), #1868
BEZEL_TEST(async_own_patternA, user_class_named_task_across_suspend_still_rejected_1868) {
    auto r = parseAndCheckOwnership(
        "module M; import System.Threading; "
        "class OwningTask public method Ping(): int return 1; end Ping; end OwningTask; "
        "class C "
        "  public static method Make(): OwningTask return new OwningTask(); end Make; "
        "  public static async method Main(): Task<int> "
        "    var t := C.Make(); "        // user class named OwningTask — NOT a builtin task
        "    await Task.Delay(5); "       // held live across a suspend
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// v0.190 SB-01 GENERIC GUARD (#1801 keying): a call whose DECLARED (pre-
// substitution) return is a BARE TYPE PARAMETER is NOT classified by the
// Task-family rule, even when that parameter is bound to `Task<int>` at the call
// site. `Pick<T>(a, b): T` at `T = Task<int>` yields a BORROW of its arguments
// (governed by *Bare-type-parameter returns*, #1801), NOT an owned mint — so its
// result carries NO auto-drop obligation. Keyed off the callee's DECLARED return
// head (`T`, not `Task`); a resolved/post-substitution key would misclassify it
// as owned and emit a spurious §28.3 auto-drop. The DIRECT task calls `pa`/`pb`
// ARE minted owned (SB01-A) and auto-drop; the bare-T result `picked` is not.
// Clean (only the two direct owned-task locals warn; no error). // per §28.3 (v0.190), #1801
BEZEL_TEST(async_own_patternA, task_family_generic_guard_bare_T_not_minted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public static method Pick<T>(a: T, b: T): T return a; end Pick; "
        "  public static async method P(): Task<int> return 1; end P; "
        "  public method Test(): int "
        "    var pa := P(); "
        "    var pb := P(); "
        "    var picked := Pick<Task<int>>(pa, pb); "
        "    return 0; "
        "  end Test; "
        "end C;");
    // No ERROR — a bare-T result is not owned-minted, so `picked` is not an
    // undischarged owned local; `pa`/`pb` auto-drop with a must-use warning.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// v0.190 SB-01 FUNCTION-VALUE arm (§28.3 "Task-family calls produce owned
// frames" — "an invocation through a function value"; #1865, the SB01-A
// residual). A call through a function VALUE whose DECLARED return slot is a
// plain `Task`-family type mints an OWNED task at the call site, exactly like an
// instance/interface/static Task-returning call. Pinned through the §28.3
// deferred-activation escaping-arg reject: an unbound owned task temporary
// passed as a plain argument to a Task-family callee that binds it (`Chain`) may
// not escape the full expression. BEFORE the fix, the function-value call was
// classified Borrowed — no MethodSymbol reaches the SB01-A `calleeMethodDecl`
// mint, and the name-head fallback carries `OwningTask` but not the plain `Task`
// head — so `mk()` was not an unbound owned temp, the reject never fired, and
// the fresh frame leaked silently. WITH the fix it is Owned → the reject fires.
// FAILS-without / PASSES-with. // per §28.3 (v0.190), #1865
BEZEL_TEST(async_own_patternA, funcval_task_call_minted_owned_reject_escaping_arg) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public static async method Chain(t: Task): Task<int> "
        "    await Task.Delay(1); return 0; end Chain; "
        "  public static async method MakeTask(): Task "
        "    await Task.Delay(1); end MakeTask; "
        "  public static async method Main(): Task<int> "
        "    var mk: () -> Task := MakeTask; "
        "    using var w := Chain(mk()); "
        "    return await w; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// v0.190 SB-01 FUNCTION-VALUE arm — OVER-REJECTION GUARD (#1865). The minted
// owned task from a function-value call is a proper owned value the caller can
// discharge normally: bind it in its own `using var src` first (naming the
// source, exactly as the deferred-activation diagnostic instructs), then pass
// the borrow `src` to `Chain`. No escaping unbound temporary, so the call is
// accepted — the mint must not turn a properly-discharged task into a spurious
// reject. Clean. // per §28.3 (v0.190), #1865
BEZEL_TEST(async_own_patternA, funcval_task_call_bind_first_clean) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public static async method Chain(t: Task): Task<int> "
        "    await Task.Delay(1); return 0; end Chain; "
        "  public static async method MakeTask(): Task "
        "    await Task.Delay(1); end MakeTask; "
        "  public static async method Main(): Task<int> "
        "    var mk: () -> Task := MakeTask; "
        "    using var src := mk(); "
        "    using var w := Chain(src); "
        "    return await w; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// v0.190 SB-01 FUNCTION-VALUE arm — CONTROL (#1865). An `OwningTask<T>` function-
// value call was ALREADY minted owned pre-#1865 (its head is in the producers
// name-head fallback), so it ALREADY rejected the escaping-arg shape. This pins
// that the bare-`Task` fix brings the plain-`Task` head to parity with its
// `OwningTask` sibling, and that the control's behavior is unchanged. Rejected.
// // per §28.3 (v0.190), #1865
BEZEL_TEST(async_own_patternA, funcval_owningtask_call_reject_escaping_arg_control) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public static async method Chain(t: Task): Task<int> "
        "    await Task.Delay(1); return 0; end Chain; "
        "  public static method MakeOT(): OwningTask<int> "
        "    return Task.FromResult(0); end MakeOT; "
        "  public static async method Main(): Task<int> "
        "    var mk: () -> OwningTask<int> := MakeOT; "
        "    using var w := Chain(mk()); "
        "    return await w; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// v0.190 SB-01 FUNCTION-VALUE arm — DECLINED RESIDUAL boundary (§28.3 v0.194
// recorded decline; #1865). A function value invoked through a BARE-TYPE-
// PARAMETER slot — `Wrap<T>(f: () -> T): T`, later instantiable at `T =
// Task<int>` — is NOT minted by the function-value arm: the mint keys PRE-
// SUBSTITUTION off the callee's declared function-return slot, which inside
// `Wrap`'s generic body has head `T`, not `Task`, so `f()` stays the bare-type-
// parameter borrow of its function-value source (#1794/#1801), admissibly
// returned through the non-`owned` `T` return. Were `f()` wrongly minted owned,
// `return f();` would be REJECTED (a fresh owned value may not escape through a
// non-`owned` bare-T return). Clean => not minted — the spec's recorded v0.194
// decline holds ("pre-substitution keying cannot see through the slot"; a leak,
// not a double-dispose). Pins that the fix does not over-reach past the
// declared-`Task`-slot case. // per §28.3 (v0.194 residual note), #1794, #1865
BEZEL_TEST(async_own_patternA, funcval_bare_type_param_slot_not_minted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public static method Wrap<T>(f: () -> T): T return f(); end Wrap; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// (2b) Pattern B await-site (§30.5 Consumer-side; Appendix C.6).
// ---------------------------------------------------------------------------

// Pattern B: awaiting a class-typed `OwningTask<T>` in a `using var`-RHS
// owned-consuming context (ownership transfers to `x`) is the canonical form —
// clean. // per §30.5
BEZEL_TEST(async_own_patternB, await_into_using_var_clean) {
    auto r = parseAndCheckOwnership(wrap(
        "using var x := await p.MakeOwned(); "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Pattern B: awaiting an `OwningTask<T>` in a BARE expression-statement
// position is NOT an owned-consuming context — the transferred result is
// dropped without discharge. Diagnosed. // per §30.5, Appendix C.6
BEZEL_TEST(async_own_patternB, await_in_bare_expr_position_diagnosed) {
    auto r = parseAndCheckOwnership(wrap(
        "await p.MakeOwned(); "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// Pattern B: awaiting into a plain `var` binding IS an owned-consuming
// context (§30.5: `var x := await ...` introduces an owned local), so the
// await itself is admitted — but the owned local `x` then carries the
// standard §28.3 discharge obligation and, undischarged, is diagnosed at
// scope exit. // per §30.5, §28.3
BEZEL_TEST(async_own_patternB, await_into_plain_var_undischarged_diagnosed) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := await p.MakeOwned(); "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// Pattern B: awaiting into a plain `var` that IS discharged is clean — the
// await site is admitted (owned-consuming) and the owned local reaches a
// discharge point. // per §30.5, §28.3
BEZEL_TEST(async_own_patternB, await_into_plain_var_discharged_clean) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := await p.MakeOwned(); "
        "x.Dispose(); "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Pattern B: a straight-line SECOND await of the same `using var`-bound owned
// task local is a read of a value the first await already consumed (§30.5: the
// Pattern B await is owned-consuming with respect to its operand). This is the
// statically-detectable slice of the §30.4 single-consumer rule — diagnosed.
// (The general cross-method / aliased second await is NOT statically
// preventable per Appendix C.6 and is left to the runtime.) // per §30.5, §30.4
// Pattern B: a `using var`-bound task is §28.3-immovable, so `await t` reads it
// as a BORROW, not a consume — a second await is therefore NOT statically
// prevented; the single-consumer guarantee is the runtime InvalidOperationException
// (§30.5 / Appendix C.6, v0.196). The compile-time moved-from error is reserved for
// a *plain* owned local (see the plain-local reject test). The runtime IOE is pinned
// by the #1785 runAotChecked gtest. (Corrected with #1785 — this previously asserted
// a static error, the pre-v0.196 spec-incorrect behavior.)
BEZEL_TEST(async_own_patternB, second_await_of_usingvar_task_not_statically_diagnosed) {
    auto r = parseAndCheckOwnership(wrap(
        "using var t := p.MakeOwned(); "
        "using var a := await t; "
        "using var b := await t; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Pattern B: `return await Producer()` from an `OwningTask<T>`-returning async
// method forwards the owned result through the state machine (§30.5 line
// ~6143) — an owned `return` is an owned-consuming context. Clean. // per §30.5
BEZEL_TEST(async_own_patternB, return_await_owned_clean) {
    auto r = parseAndCheckOwnership(wrap(
        "return await p.MakeOwned(); ", "M(): OwningTask<Conn>"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Pattern B: awaiting an `OwningTask<T>` into an `owned` argument (`Sink` takes
// `owned c`) is an owned-consuming context — the result transfers into the
// call. Clean. // per §30.5
BEZEL_TEST(async_own_patternB, await_into_owned_arg_clean) {
    auto r = parseAndCheckOwnership(wrap(
        "Sink(await p.MakeOwned()); "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Pattern B: awaiting an `OwningTask<T>` into a NON-owned argument (`Use`
// borrows `c`) is NOT an owned-consuming context — the transferred result is
// dropped. Diagnosed. // per §30.5, Appendix C.6
BEZEL_TEST(async_own_patternB, await_into_borrow_arg_diagnosed) {
    auto r = parseAndCheckOwnership(wrap(
        "Use(await p.MakeOwned()); "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// Pattern B: a single await of a `using var`-bound owned task local is clean —
// the await consumes the task (implicit disposal IS the discharge, §30.5),
// and the using-var's scope-end disposal is the idempotent no-op the spec
// describes. Guards that the operand-consume model does not over-reject the
// single-await bound-local form. // per §30.5
BEZEL_TEST(async_own_patternB, single_await_of_bound_task_clean) {
    auto r = parseAndCheckOwnership(wrap(
        "using var t := p.MakeOwned(); "
        "using var a := await t; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// fix #4064 (§30.5, v0.294): the binding-context half of `await <OwningTask>`
// in an `owned` argument position is just as owned-consuming as the bare-
// statement half — `var y := Sink(await p.MakeOwned());` transfers the
// produced `Conn` into the call exactly as `Sink(await p.MakeOwned());` does.
// The walker had the rule right for the bare `ExpressionStmt` arm (it walks
// the call's arguments); the LocalDecl / AssignmentStmt / ReturnStmt /
// YieldStmt / DiscardStmt arms only `awaitRoot`-checked the whole RHS and
// therefore false-rejected a call-arg position that was reached through any
// value-producing statement form.
BEZEL_TEST(async_own_patternB, await_into_owned_arg_localdecl_clean) {
    auto r = parseAndCheckOwnership(wrap(
        "var y := Sink(await p.MakeOwned()); "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(async_own_patternB, await_into_owned_arg_discard_clean) {
    auto r = parseAndCheckOwnership(wrap(
        "_ := Sink(await p.MakeOwned()); "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// (3) Await of `Task` (no result) — no ownership obligation either way.
// ---------------------------------------------------------------------------

// Awaiting a `Task` (no value) in a bare statement position is clean — there
// is no result to own, so neither the Pattern A nor Pattern B site rule
// applies. Guards that the site rules are scoped to class-typed results.
// // per §30.5
BEZEL_TEST(async_own_unit, await_unit_task_bare_clean) {
    auto r = parseAndCheckOwnership(wrap(
        "await p.MakeUnit(); "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
