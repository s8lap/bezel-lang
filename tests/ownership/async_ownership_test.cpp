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
//      local — the across-suspend annotation never changes a non-suspend
//      diagnosis.
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
        "class C "
        "  public p: Producer := null; "
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
// discharge is diagnosed. This guards that the across-suspend machinery does
// not silence a plain leak in an async body. // per §30.5, §28.3
BEZEL_TEST(async_own_suspend, activation_local_drop_diagnosed_as_today) {
    auto r = parseAndCheckOwnership(wrap(
        "var x := new T(); "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
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

// Pattern A (v0.182): the confinement admission is scoped to an UNBOUND
// producer expression (a bare call), not to "any operand
// `awaitOperandIsOwningLocation` rejects." A reference to a plain
// (non-`using`) owned local — `var t := p.MakeShared();` — is a NAMED value
// whose lifetime is its declaring scope, not the statement that reads it;
// reading it in a later, argument-position statement must NOT be admitted by
// the new rule (it is a different case from a fresh, unnamed producer call).
// Diagnosed. // per §30.5, §28.3
BEZEL_TEST(async_own_patternA, await_plain_var_task_in_arg_still_diagnosed) {
    auto r = parseAndCheckOwnership(wrap(
        "var t := p.MakeShared(); "
        "Use(await t); "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
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
