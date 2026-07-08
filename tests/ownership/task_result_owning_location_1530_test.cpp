// Spec-derived tests for #1530 (§30.3 / §30.5, v0.179): `Task<T>.Result`
// (Pattern A, class T) owning-location check.
//
// §30.3: "Reading `Result` on a completed `Task<T>` whose `T` is a **class**
// type (Pattern A) is additionally subject to the same owning-source rule as
// `await` (§30.5 consumer-side rules): the read produces a borrow whose
// source is the task expression's own location ... A form whose borrow
// escapes a temporary read's full expression remains a compile error, exactly
// as the corresponding `await` is."
//
// The v0.162 `OwningTask<T>.Result` amendment (#1379) added the synchronous
// `.Result` escape hatch for Pattern B but left this Pattern A sibling gap
// unenforced: pre-fix, `SomeProducer().Result` (a sourceless read on a
// class-typed `Task<T>`) compiled cleanly with no diagnostic, silently
// admitting a borrow with no source. This file mirrors the Pattern A
// `await`-site coverage in async_ownership_test.cpp ("(2a) Pattern A
// await-site") one-for-one, applied to `.Result` instead of `await`. Unlike
// those tests, the method under test here is NOT `async` — `.Result` is the
// SYNCHRONOUS escape hatch (§30.3) and is rejected lexically inside an async
// method/lambda by a separate, pre-existing check (ExprTypecheck_Calls.cpp,
// §30.3 v0.162/#1362), so these bodies are ordinary (non-async) methods.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

// Preamble: a Producer with concrete (non-async, body-bearing) methods
// returning `Task<Conn>` (class Conn — Pattern A) and `OwningTask<Conn>`
// (Pattern B, for the regression guard below), plus a class C hosting the
// SYNCHRONOUS method under test `M`. `Use(c: Conn)` is a borrow-consuming
// helper for the argument-position tests; `t` is an `owned` field of type
// `Task<Conn>` for the owned-field-receiver test. Mirrors the `wrap` helper
// in async_ownership_test.cpp, minus `async`. `sig` lets a test override the
// signature of `M` to add a parameter (owned or borrow).
std::string wrap(const std::string& body, const std::string& sig = "M()") {
    return std::string("module M; "
        "class Conn end Conn; "
        "class Producer "
        "  public method MakeShared(): Task<Conn> => null; "
        "  public method MakeOwned(): OwningTask<Conn> => null; "
        "  public method MakeInt(): Task<int> => null; "
        "end Producer; "
        "class C "
        "  public p: Producer := null; "
        "  public owned t: Task<Conn>; "
        "  public method Use(c: Conn) end Use; "
        "  public method ") + sig + " " + body + " end M; "
        "end C;";
}

} // namespace

// ---------------------------------------------------------------------------
// Sourceless `.Result` reads (§30.3 / §30.5; Appendix C.6) — rejected.
// ---------------------------------------------------------------------------

// A bare producer call read via `.Result` into a `var` binding occupies NO
// admitted owning location — the borrowed result would have no source.
// Diagnosed. Mirrors await_bare_producer_into_var_diagnosed. // per §30.3, §30.5
BEZEL_TEST(result_own_patternA, result_bare_producer_into_var_diagnosed) {
    auto r = parseAndCheckOwnership(wrap(
        "var r := p.MakeShared().Result; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// A bare producer call read via `.Result` in an argument position is
// likewise not an admitted owning location — diagnosed. Mirrors
// await_bare_producer_in_arg_diagnosed. // per §30.3, §30.5
BEZEL_TEST(result_own_patternA, result_bare_producer_in_arg_diagnosed) {
    auto r = parseAndCheckOwnership(wrap(
        "Use(p.MakeShared().Result); "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// A plain (non-`using`) `var t := producer()` local read via `.Result` is
// STILL not an owning location — the bare-producer form the rule rejects,
// just expressed through one intervening binding. // per §30.3, §30.5
BEZEL_TEST(result_own_patternA, result_plain_var_bound_producer_diagnosed) {
    auto r = parseAndCheckOwnership(wrap(
        "var task := p.MakeShared(); "
        "var r := task.Result; "
        "Use(r); "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// Admitted owning locations (§30.3 / §30.5) — clean.
// ---------------------------------------------------------------------------

// The canonical form: a `using var`-bound `Task<Conn>` local. `.Result`
// borrows from `task`'s scope. Mirrors await_using_var_bound_task_clean.
// // per §30.3, §30.5
BEZEL_TEST(result_own_patternA, result_using_var_bound_task_clean) {
    auto r = parseAndCheckOwnership(wrap(
        "using var task := p.MakeShared(); "
        "var r := task.Result; "
        "Use(r); "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The borrow produced by `task.Result` (task in an admitted owning location)
// may flow to a non-owned argument directly, without an intermediate `var`.
// Mirrors await_bound_task_into_borrow_arg_clean. // per §30.3, §30.5
BEZEL_TEST(result_own_patternA, result_using_var_bound_task_into_borrow_arg_clean) {
    auto r = parseAndCheckOwnership(wrap(
        "using var task := p.MakeShared(); "
        "Use(task.Result); "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// An `owned` PARAMETER of type `Task<Conn>` is an admitted owning location —
// its lifetime spans the method body. Mirrors await_owned_param_task_clean.
// // per §30.3, §30.5
BEZEL_TEST(result_own_patternA, result_owned_param_task_clean) {
    auto r = parseAndCheckOwnership(wrap(
        "var r := task.Result; "
        "Use(r); "
        "move task; ",
        "M(owned task: Task<Conn>)"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A plain BORROW parameter is ALSO admitted — its source is the caller's
// frame, which outlives the body, so the borrow cannot dangle (§30.5: "a
// plain BORROW parameter is admitted too"). // per §30.3, §30.5
BEZEL_TEST(result_own_patternA, result_borrow_param_task_clean) {
    auto r = parseAndCheckOwnership(wrap(
        "var r := task.Result; "
        "Use(r); ",
        "M(task: Task<Conn>)"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// An `owned` FIELD of `this` is an admitted owning location — its lifetime
// spans `this`'s lifetime. // per §30.3, §30.5
BEZEL_TEST(result_own_patternA, result_owned_field_clean) {
    auto r = parseAndCheckOwnership(wrap(
        "var r := t.Result; "
        "Use(r); "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// Scope guards — the rule is scoped to class-T `Task<T>` (Pattern A) only.
// ---------------------------------------------------------------------------

// A non-class `Task<T>` (here `Task<int>`) carries no ownership obligation
// across the `.Result` read, so any receiver position is admitted — a bare
// `var n := producer().Result` is clean. Guards that the site rule is scoped
// to CLASS T only (§30.5 producer-side rules). Mirrors
// await_value_typed_task_any_position_clean. // per §30.3, §30.5
BEZEL_TEST(result_own_patternA, result_value_typed_task_any_position_clean) {
    auto r = parseAndCheckOwnership(wrap(
        "var n := p.MakeInt().Result; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// `OwningTask<T>.Result` (Pattern B, v0.162/#1379) is a DIFFERENT rule — an
// owned-consuming-position obligation, not this borrow-source rule — and
// must not be spuriously rejected by the Pattern A owning-location check.
// A bare producer read is unaffected by THIS rule (it may or may not be
// diagnosed by the separate Pattern B rule, which is out of #1530's scope;
// this guard only asserts the Pattern A check does not fire on it).
// // per §30.3, §30.5
BEZEL_TEST(result_own_patternA, owning_task_result_not_gated_by_patternA_rule) {
    auto r = parseAndCheckOwnership(wrap(
        "var c := p.MakeOwned().Result; "
        "c.Dispose(); "));
    // The diagnostic this test guards against is specifically the Pattern A
    // "owning location" message; assert it does not appear.
    bool sawPatternAOwningLocationError = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("class-typed `Task<T>` (Pattern A)") !=
            std::string::npos) {
            sawPatternAOwningLocationError = true;
        }
    }
    BEZEL_EXPECT_FALSE(sawPatternAOwningLocationError);
}
