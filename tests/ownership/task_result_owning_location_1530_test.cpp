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
//
// v0.182 (§30.3/§28.3, #1570) amended the rule: "an unbound owned temporary
// (`SomeProducer().Result`) is admitted when the borrow is confined to the
// same full expression, the temporary being disposed at the expression's end
// exactly as for the corresponding `await`." Only a borrow that ESCAPES the
// full expression — bound to a `var`, stored, or returned — remains rejected.
// The tests below were updated (one flipped, several added) to reflect this;
// see the inline notes.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

// Preamble: a Producer with concrete (non-async, body-bearing) methods
// returning `Task<Conn>` (class Conn — Pattern A) and `OwningTask<Conn>`
// (Pattern B, for the regression guard below), plus a class C hosting the
// SYNCHRONOUS method under test `M`. `Use(c: Conn)` is a borrow-consuming
// helper for the argument-position tests. Mirrors the `wrap` helper
// in async_ownership_test.cpp, minus `async`. `sig` lets a test override the
// signature of `M` to add a parameter (owned or borrow).
//
// Post-#4025 §7.9 (v0.295, cycles-6/7): a non-nullable field of any
// heap-pointer builtin (Mutex, Semaphore, Task<T>, IEnumerable<T>, …)
// requires an explicit initializer or ctor assignment; pre-fix such a
// field declared `owned t: Task<Conn>;` with no initializer silently
// compiled and SIGSEGV'd on first access. The default helper omits `t`
// entirely so the ten tests that don't read `t` keep their original
// fixture. `wrapField` (below) is reserved for `result_owned_field_clean`,
// which exercises the `t` admitted owning-location rule, and supplies the
// initializing ctor `#4025` requires.
std::string wrap(const std::string& body, const std::string& sig = "M()") {
    return std::string("module M; "
        "class Conn end Conn; "
        // Producer is an INTERFACE (abstract methods, no bodies): a `=> null`
        // stub body is now a §7 error (null into a non-nullable task type),
        // since §22.2 arrow bodies run the same §5.1/§7 return-convertibility
        // check as the block form (#1774). The call sites the tests exercise
        // are unchanged — Producer is only ever a `p: Producer := null;` field.
        "interface Producer "
        "  method MakeShared(): Task<Conn>; "
        "  method MakeOwned(): OwningTask<Conn>; "
        "  method MakeInt(): Task<int>; "
        "end Producer; "
        "class C(p: Producer) "
        "  public method Use(c: Conn) end Use; "
        "  public method ") + sig + " " + body + " end M; "
        "end C;";
}

// fixture for the single test that exercises an `owned t: Task<Conn>`
// field as an admitted §30.3/§30.5 owning location for `.Result`. Per
// #4025 the field has a post-#4025 strict-storage obligation — Task<T>
// is a heap-pointer builtin (§7.9 v0.295), so a non-nullable field of
// that type MUST be initialized (no default exists). Drop the primary
// ctor and supply an explicit one that captures `p` as a public field
// and binds `x` (an owned `Task<Conn>` parameter) into `t`. The test
// only parses/typechecks (`parseAndCheckOwnership`); instantiation is
// not exercised, so this ctor needs to compile, not actually run.
std::string wrapField(const std::string& body, const std::string& sig = "M()") {
    return std::string("module M; "
        "class Conn end Conn; "
        "interface Producer "
        "  method MakeShared(): Task<Conn>; "
        "  method MakeOwned(): OwningTask<Conn>; "
        "  method MakeInt(): Task<int>; "
        "end Producer; "
        "class C "
        "  public p: Producer; "
        "  public owned t: Task<Conn>; "
        "  public constructor(prod: Producer, owned x: Task<Conn>) "
        "    p := prod; "
        "    t := x; "
        "  end constructor; "
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

// v0.182 (§30.3/§28.3, #1570): a bare producer call read via `.Result` in an
// argument position IS now admitted — `p.MakeShared()` is an unbound owned
// temporary, and the `.Result` borrow it sources is consumed by `Use` within
// the same full expression (the statement), so the temporary's disposal
// defers to the end of that expression, after the borrowing use — exactly
// the `Producer().Result` shape §30.3 names as admitted. Through v0.181 this
// was rejected outright; this test previously asserted the pre-v0.182
// rejection and is updated to the new admission. Mirrors
// await_bare_producer_in_arg_confined_clean. Clean. // per §30.3, §30.5, §28.3
BEZEL_TEST(result_own_patternA, result_bare_producer_in_arg_confined_clean) {
    auto r = parseAndCheckOwnership(wrap(
        "Use(p.MakeShared().Result); "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// v0.182: a bare producer call read via `.Result` as a standalone statement
// (the borrow used nowhere) is trivially confined to the full expression —
// zero uses is confined by definition — so it is admitted.
// // per §30.3, §30.5, §28.3
BEZEL_TEST(result_own_patternA, result_bare_producer_bare_statement_clean) {
    auto r = parseAndCheckOwnership(wrap(
        "p.MakeShared().Result; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// v0.190 SB-01 (§28.3 "Task-family calls produce owned frames" + §30.5 plain
// owned task local admission, line 7351; #1786 SB01-A/C1). A `Task`-family CALL
// mints an OWNED task (SB01-A), so `var task := p.MakeShared();` is a plain
// owned task local — an ADMITTED `.Result` owning location (SB01-C1 reuses
// awaitOperandIsOwningLocation for the `.Result` site). `.Result` borrows the
// result within `task`'s scope; `r` is a borrow of it; `task` auto-drops at
// scope end (a must-use WARNING, not an error). FLIP: was rejected pre-v0.190
// (a plain task call was a borrow with no admitted source). Clean.
// // per §30.3, §30.5, §28.3 (v0.190)
BEZEL_TEST(result_own_patternA, result_plain_var_bound_producer_admitted_v0190) {
    auto r = parseAndCheckOwnership(wrap(
        "var task := p.MakeShared(); "
        "var r := task.Result; "
        "Use(r); "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// v0.190 SB-01 (§28.3 + §30.5 line 7351, #1786): the argument-position sibling
// of the above — `Use(task.Result)` reading a plain owned task local's result.
// Same admission: `task` is a plain owned task local (SB01-A/C1), so its
// `.Result` read is admitted and `task` auto-drops at scope end (a must-use
// warning). FLIP (was rejected pre-v0.190). Clean.
// // per §30.3, §30.5, §28.3 (v0.190)
BEZEL_TEST(result_own_patternA, result_plain_var_task_in_arg_admitted_v0190) {
    auto r = parseAndCheckOwnership(wrap(
        "var task := p.MakeShared(); "
        "Use(task.Result); "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
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
    auto r = parseAndCheckOwnership(wrapField(
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
