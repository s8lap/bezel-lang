// Ownership — fix #3074: the §30.5 await-site check (and its §30.3 `.Result`
// sibling) walked a hand-maintained twelve-arm statement list with holes.
//
// Spec (revision v0.249):
//   §30.5 "Result lifetime and task disposal", Consumer-side rules, Pattern A —
//     a borrow that would ESCAPE the full expression of an unbound-temporary
//     await "has no source that survives the expression, and the await is a
//     compile error (K8.4: rejected, never lifetime-extended); bind the task to
//     a `using var` first."
//   §30.5 "The scoped await-bind", *Which binding forms qualify* — "`var` and
//     `const` alike — a runtime `const` binds values and borrows that carry no
//     obligation (§21.1), which the await's borrow is; the `const` adds
//     non-reassignability of `x` only." The two spellings are ONE row: whatever
//     the rule does to `var x := await E;` it does to `const x := await E;`.
//   §30.5 "*`.Result` in lockstep*" — "The admission applies identically to the
//     completed-task `.Result` read (§30.3) … exactly as await and `.Result`
//     have moved in lockstep since their temporary machinery was unified."
//   §21.1 — a `const` local "must not take an owned production"; that is the
//     authoritative rejection for the Pattern B `const` spelling.
//
// THE DEFECT. `checkAwaitSitesStmt` dispatched on exactly twelve statement
// forms — LocalDecl, ExpressionStmt, DiscardStmt, AssignmentStmt, ReturnStmt,
// ThrowStmt, YieldStmt, IfStmt, WhileStmt, ForStmt, DoEndBlock, TryStmt — and
// `checkResultSitesStmt` was a verbatim second copy of the same list.
// `ast::LocalConstDecl` (a separate AST node from `LocalDecl`) and
// `ast::CaseStmt` appeared in neither, so:
//
//     const cfg := await C.FetchAsync();   -- accepted; the `var` spelling errors
//     case n of 1 => do var cfg := await C.FetchAsync(); … end, … end;
//
// were never classified at all, and the resulting Pattern A borrow dangled at
// run time (nondeterministic garbage; heap-use-after-free under ASan). This is
// the positive-list-with-a-missing-entry shape: not a wrong predicate, but a
// correct predicate that is never REACHED, so it fails by silently ACCEPTING
// rather than by loudly rejecting.
//
// THE FIX is structural, not two arms. Two holes found by two independent routes
// says the list was accumulated, not derived; so both walks now share ONE
// traversal (`visitStmtExprSites`) whose arms are the complete `: Stmt`
// hierarchy of `src/ast/Node.hpp` in that header's order, including the kinds
// that hold no expression. `collectOwnedConsumingAwaitsStmt` — the complement
// that decides which Pattern B awaits sit in an owned-consuming context — gained
// the matching `CaseStmt` arm, without which reaching a `case` arm in the check
// walk alone would have turned a legal `using var x := await Acquire();` there
// into a FALSE REJECT (the `case_arm_pattern_b_using_var_clean` control below).
//
// These are diagnostic tests: the runtime consequence of the accepted-and-
// dangling `const` form is pinned separately by the #3070 codegen suite.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;

namespace {

// The same preamble shape `async_ownership_test.cpp` uses: an abstract producer
// interface supplying the three task shapes, and a class hosting the method
// under test. `Conn` is a class, so `Task<Conn>` is Pattern A and
// `OwningTask<Conn>` is Pattern B.
std::string wrap(const std::string& body, const std::string& sig = "M(): Task") {
    return std::string("module M; "
        "class Conn end Conn; "
        "interface Producer "
        "  method MakeOwned(): OwningTask<Conn>; "
        "  method MakeShared(): Task<Conn>; "
        "end Producer; "
        "class C(p: Producer) "
        "  public method Use(c: Conn) end Use; "
        "  public async method ") + sig + " " + body + " end M; "
        "end C;";
}

// A SYNCHRONOUS host for the `.Result` cases — §30.3's blocking read is rejected
// inside an `async` body ("use `await t` — blocking waits are not available in
// async code"), so the lockstep sibling must be exercised from sync code, which
// is also where `checkResultSites` runs unconditionally.
std::string wrapSync(const std::string& body) {
    return std::string("module M; "
        "class Conn end Conn; "
        "interface Producer "
        "  method MakeShared(): Task<Conn>; "
        "end Producer; "
        "class C(p: Producer) "
        "  public method Use(c: Conn) end Use; "
        "  public method M(): int " + body + " return 0; end M; "
        "end C;");
}

const char* kPatternAMessage =
    "awaiting a class-typed `Task<T>` (Pattern A) requires the awaited task to "
    "occupy a syntactically-visible owning location";

const char* kResultMessage =
    "reading `.Result` on a class-typed `Task<T>` (Pattern A) requires the task "
    "expression to occupy a syntactically-visible owning location";

} // namespace

// ---------------------------------------------------------------------------
// LocalConstDecl — the arm #3074 is filed against.
// ---------------------------------------------------------------------------

// `const x := await <fresh producer>;` binds the Pattern A borrow past the full
// expression that sources it, exactly as the `var` spelling does, and §30.5 puts
// the two spellings in one row. Rejected. Pre-fix: silently accepted, and the
// borrow dangled. // per §30.5, §21.1
BEZEL_TEST(await_site_stmt_walk_3074, const_bind_of_producer_await_rejected_30_5) {
    auto r = parseAndCheckOwnership(wrap(
        "const cfg := await p.MakeShared(); "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kPatternAMessage));
}

// The same rejection the `var` spelling already produced — pinned alongside so
// the two forms cannot drift apart again. // per §30.5
BEZEL_TEST(await_site_stmt_walk_3074, var_bind_of_producer_await_rejected_30_5) {
    auto r = parseAndCheckOwnership(wrap(
        "var cfg := await p.MakeShared(); "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kPatternAMessage));
}

// CONTROL: a `const` bound from an ADMITTED named operand is clean — the new arm
// classifies the site, it does not blanket-reject the `const` spelling.
// // per §30.5
BEZEL_TEST(await_site_stmt_walk_3074, const_bind_of_named_task_await_clean_30_5) {
    auto r = parseAndCheckOwnership(wrap(
        "using var t := p.MakeShared(); "
        "const cfg := await t; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §30.5 "`.Result` in lockstep": the same hole existed in `checkResultSitesStmt`,
// so the synchronous sibling read was equally exempt under a `const` binding.
// // per §30.3, §30.5
BEZEL_TEST(await_site_stmt_walk_3074, const_bind_of_producer_result_rejected_30_3) {
    auto r = parseAndCheckOwnership(wrapSync(
        "const cfg := p.MakeShared().Result; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kResultMessage));
}

// CONTROL for the lockstep sibling: an admitted named receiver stays clean.
// // per §30.3, §30.5
BEZEL_TEST(await_site_stmt_walk_3074, const_bind_of_named_task_result_clean_30_3) {
    auto r = parseAndCheckOwnership(wrapSync(
        "using var t := p.MakeShared(); "
        "const cfg := t.Result; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The Pattern B `const` spelling keeps §21.1's authoritative message (a `const`
// local may not take an owned production). It is NOT recorded as an
// owned-consuming context — §30.5's owned-binding row names `using var` /
// `await using var` / `var` only — so the §30.5 Pattern B diagnostic joins it;
// both name `using var` as the remedy. // per §21.1, §30.5
BEZEL_TEST(await_site_stmt_walk_3074, const_bind_of_pattern_b_await_rejected_21_1) {
    auto r = parseAndCheckOwnership(wrap(
        "const c := await p.MakeOwned(); "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "must not take an owned production"));
}

// ---------------------------------------------------------------------------
// CaseStmt — the arm found in review on the same list.
// ---------------------------------------------------------------------------

// An await in a statement-position `case` arm body was never §30.5-classified,
// so the escaping Pattern A bind was silently accepted there. Rejected now.
// // per §30.5
BEZEL_TEST(await_site_stmt_walk_3074, case_arm_body_producer_await_rejected_30_5) {
    auto r = parseAndCheckOwnership(wrap(
        "var n := 1; "
        "case n of "
        "  1 => do var cfg := await p.MakeShared(); end, "
        "  _ => do end, "
        "end; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kPatternAMessage));
}

// A Pattern B await in a `case` arm body that is NOT in an owned-consuming
// context drops its transferred result — diagnosed now, silently accepted
// before. // per §30.5
BEZEL_TEST(await_site_stmt_walk_3074, case_arm_body_bare_pattern_b_await_rejected_30_5) {
    auto r = parseAndCheckOwnership(wrap(
        "var n := 1; "
        "case n of "
        "  1 => do await p.MakeOwned(); end, "
        "  _ => do end, "
        "end; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "must appear in an owned-consuming context"));
}

// ★ THE FALSE-REJECT CONTROL. Reaching a `case` arm in the CHECK walk without
// the matching arm in `collectOwnedConsumingAwaitsStmt` would report this
// perfectly legal Pattern B binding as "not in an owned-consuming context". The
// two walks are complements and must be extended together. // per §30.5
BEZEL_TEST(await_site_stmt_walk_3074, case_arm_body_pattern_b_using_var_clean_30_5) {
    auto r = parseAndCheckOwnership(wrap(
        "var n := 1; "
        "case n of "
        "  1 => do using var x := await p.MakeOwned(); end, "
        "  _ => do end, "
        "end; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL: the §30.5 expression-confinement admission still holds inside a
// `case` arm — `Use(await Producer())` is admitted there exactly as it is at
// statement level, so the new arm did not turn the arm body into a binding
// position. // per §30.5
BEZEL_TEST(await_site_stmt_walk_3074, case_arm_body_confined_producer_await_clean_30_5) {
    auto r = parseAndCheckOwnership(wrap(
        "var n := 1; "
        "case n of "
        "  1 => do Use(await p.MakeShared()); end, "
        "  _ => do end, "
        "end; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The `case` SCRUTINEE is an evaluated, non-binding position — an unbound
// producer await confined to it is admitted. Pins that the scrutinee is walked
// with `bindingValue = false`. // per §30.5
BEZEL_TEST(await_site_stmt_walk_3074, case_scrutinee_confined_producer_await_clean_30_5) {
    auto r = parseAndCheckOwnership(wrap(
        "var n := 0; "
        "case (await p.MakeShared()) is Conn of "
        "  true => do n := 1; end, "
        "  _ => do end, "
        "end; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
