// Spec-derived regression tests for issue #3702 — the SIGSEGV residual of
// #3564. #3564 gave the §28.3 owned-local return-escape check a lambda-body
// scope; #3702 closes the method-level sibling the issue's repro exercises:
//
//     using var r := new R();
//     var mk := lambda(): () -> int => do
//       return lambda(): int => r.V();
//     end;
//     return mk();   // mk() returns the inner closure, which captures r;
//                    // Mk() returning hands that closure to its caller,
//                    // where r is already disposed — SIGSEGV on the next call.
//
// `return mk();` is a `CallExpr` whose callee is a function-typed local whose
// value comes from a lambda-literal initializer. The method-level return
// check (`checkClosureEscapesOnReturn`) descends into a call's ARGUMENTS only
// (per the §28.14 / §30.11 history); the callee's initializer is a separate
// AST subtree that no caller-side walk had visited. The lambda-literal that
// built `mk` therefore slipped past both the method-level leg and the
// lambda-body driver — neither scope saw a closure to check.
//
// The fix adds the callee's initializer lambda to the same `lambdas` list the
// lambda-literal arm feeds; the existing capture walks run against it and the
// §28.3 leg resolves `r` against the method's `states_` and rejects. Cited
// spec: §28.3 ("Borrow lifetime rules", v0.286) — the closure-over-owned-
// local does-not-outlive-its-scope rule — and §27.5 ("Closure literals", v0.286)
// for the lambda-literal initializer shape.
//
// The safe sibling — `var g := mk(); return g() + g();` (the closure consumed
// in place, before `q` is disposed) — was already accepted by #3564 (it pins the
// ESCAPE boundary) and stays accepted: my fix triggers on the callee of a
// CallExpr in the return position, and the returned expression `g() + g()`
// is a `BinaryExpr` whose operands are `CallExpr`s whose callees are NOT
// local-with-lambda-initializer (g's initializer is the result of `mk()`,
// not a lambda literal itself). Regression-tested by the existing
// `method_scope_owned_local_capture_still_accepted_28_9` row.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;

namespace {

// A helper class with a `V(): int` instance method and the framework the
// rows below need. `R` is a plain class so `using var r := new R();` is a
// §28.3 scope-bound owned local whose capture is a §28.3 borrow.
std::string wrap(const std::string& method) {
    return std::string(
        "module M; "
        "class R "
        "  public method V(): int return 5; end V; "
        "end R; "
        "class C ") + method + " end C;";
}

}  // namespace

// POSITIVE (§28.3 / §27.5, #3702): the filed repro. `Mk()` returns `mk()`,
// whose result is a closure that captures method-scope `r`; `r` is disposed
// when `Mk` returns, and the returned closure (now held by the caller) is
// the borrow source the next call reads — a SIGSEGV measured on the parent.
// The method-level return check must REJECT this on the same §28.3 wording
// the method-level twin ("return a lambda literal directly") has always
// used; that wording is verbatim here, so the fix introduces no second
// spelling of the rule.
BEZEL_TEST(method_scope_owned_local_escape, mk_call_returned_method_closure_rejected_28_9) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Mk(): () -> int "
        "  using var r := new R(); "
        "  var mk := lambda(): () -> int => do "
        "    return lambda(): int => r.V(); "
        "  end; "
        "  return mk(); "
        "end Mk;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "returned closure captures `r` whose source is disposed at scope "
           "end"));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "(§28.3 closure-capture escape"));
}

// POSITIVE (§28.3 / §27.5, #3702): the structurally SIMPLER unsafe case —
// mk's body returns the inner closure directly via a single `return`. No
// intervening `do ... end`, no `f` local. `mk`'s body is `=> E` (expression-
// bodied), where `E` is the inner lambda literal capturing `r`. The fix's
// exprBody branch picks this up exactly the same way the block-bodied
// branch does.
BEZEL_TEST(method_scope_owned_local_escape, mk_exprbody_returns_closure_rejected_28_9) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Mk(): () -> int "
        "  using var r := new R(); "
        "  var mk := lambda(): () -> int => lambda(): int => r.V(); "
        "  return mk(); "
        "end Mk;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "returned closure captures `r` whose source is disposed at scope "
           "end"));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "(§28.3 closure-capture escape"));
}

// NEGATIVE (§28.3 / §27.5, #3702) — the OBLIGATION boundary. Same shape as
// the first positive row but `q` is a value-typed local, so it is never
// Owned in `states_` (§28.3) and carries no scope-bound discharge
// obligation. A seed that admitted every identifier as Owned would reject
// this; the fix's check consults `states_` so a non-Owned binding falls
// through the §28.3 leg.
BEZEL_TEST(method_scope_owned_local_escape, mk_call_over_value_local_accepted_28_9) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Mk(): int "
        "  var k := 7; "
        "  var mk := lambda(): int => k; "
        "  return mk(); "
        "end Mk;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// NEGATIVE (§28.3, #3702) — the CONSUME-IN-PLACE boundary. The lambda
// stored in `mk` is invoked inside the body but NOT in the return position
// (the ExpressionStmt `mk();` discards the int result). `findLambdas`
// reaches only return-position expressions; an ExpressionStmt is not
// visited at all. The same closure shape is accepted.
BEZEL_TEST(method_scope_owned_local_escape, mk_call_discarded_not_returned_accepted_28_9) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Mk(): int "
        "  using var r := new R(); "
        "  var mk := lambda(): int => r.V(); "
        "  mk(); "
        "  return 0; "
        "end Mk;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// NEGATIVE (§28.3, #3702) — the CONSUMED-IN-METHOD boundary. The closure
// returned by `mk()` (the inner closure over `r`) is bound to `g` and
// invoked twice before `Mk` returns. The captures are read while `r` is
// still in scope, and `g` is consumed at end of `Mk`. This is the SAFE
// shape that #3564's `method_scope_owned_local_capture_still_accepted_28_9`
// pinned (with a slightly different control-flow shape), and the fix must
// preserve it: `g() + g()` is a BinaryExpr, not a CallExpr, so my
// CallExpr-arm addition never fires on `g`.
BEZEL_TEST(method_scope_owned_local_escape, mk_call_result_consumed_in_method_accepted_28_9) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Mk(): int "
        "  using var r := new R(); "
        "  var mk := lambda(): () -> int => do "
        "    return lambda(): int => r.V(); "
        "  end; "
        "  var g := mk(); "
        "  return g() + g(); "
        "end Mk;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// POSITIVE (§28.3 / §27.5, #3702 + #3591): the §21.1 `const`-binding sibling
// of the filed repro. `mk` is a `const` local (§21.1) bound to a
// lambda-literal initializer, and §21.1 explicitly admits a `const` carrying
// the initializer's source-liveness constraints, exactly as a plain
// unmarked `var` alias. Without the #3591 sibling `LocalConstDecl` arm the
// walker at Ownership_Checks.cpp:3277 silently never visited the lambda
// literal, so the method-level §28.3 escape check fired on NO closures and
// `Mk()` returned a closure capturing `r` past `r`'s scope end — identical
// to the `var mk := ...` repro, one binding-form over.
BEZEL_TEST(method_scope_owned_local_escape, const_mk_call_returned_method_closure_rejected_28_9) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Mk(): () -> int "
        "  using var r := new R(); "
        "  const mk := lambda(): () -> int => do "
        "    return lambda(): int => r.V(); "
        "  end; "
        "  return mk(); "
        "end Mk;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "returned closure captures `r` whose source is disposed at scope "
           "end"));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "(§28.3 closure-capture escape"));
}

// POSITIVE (§28.3 / §27.5, #3702 + #3591): the §21.1 `const`-binding sibling
// of the expression-bodied row. `mk` is a `const` local whose initializer is
// an expression-bodied lambda returning an inner closure that captures `r`.
// Without the #3591 sibling arm the exprBody branch never ran for a `const`,
// so this compiled clean.
BEZEL_TEST(method_scope_owned_local_escape, const_mk_exprbody_returns_closure_rejected_28_9) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Mk(): () -> int "
        "  using var r := new R(); "
        "  const mk := lambda(): () -> int => lambda(): int => r.V(); "
        "  return mk(); "
        "end Mk;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "returned closure captures `r` whose source is disposed at scope "
           "end"));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "(§28.3 closure-capture escape"));
}