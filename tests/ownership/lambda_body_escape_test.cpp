// Spec-derived regression tests for issues #3565 and #3564 — the two
// surviving channels of the "ownership's escape checks never enter a LAMBDA
// BODY" traversal hole that fix #3546 opened up and closed only one leg of.
//
// The hole. Ownership's statement walks deliberately never enter a lambda
// body: a lambda body lives inside a `LambdaExpr`, an *expression*. So every
// escape channel driven from the dataflow's statement walk — `handleReturn` →
// `checkClosureEscapesOnReturn`, `handleAssignment` →
// `checkBorrowStoreEscape` — was simply not reached for a `return` or an
// assignment inside a lambda body, while the byte-identical METHOD-level
// spelling of the same program was rejected. fix #3546 (v0.283) reached the
// §31.1 caught-exception RETURN leg. Two channels survived:
//
//   #3565 — §31.1 "Exception ownership" (v0.286). The bound exception
//   variable "is a **borrow** of the in-flight exception; its source is the
//   runtime, and its lifetime is the arm body's"; it may not be "captured by a
//   closure that may outlive the arm", and "Exception instances themselves
//   cannot escape the arm that caught them." A closure capturing the binding
//   and ASSIGNED to a local declared outside the arm compiled clean inside a
//   lambda body and heap-use-after-freed (measured rc=139 / ASan UAF).
//
//   #3564 — §28.9 "Runtime-managed types" (v0.286): "Capturing an *owned*
//   class-typed local does **not** transfer ownership into the closure: the
//   capture is a borrow whose source is the local's enclosing scope,
//   borrow-state throughout — the closure may not assign to, transfer from, or
//   dispose the captured binding, and may not outlive that scope (§28.3)." A
//   closure over a lambda body's own `using var`, RETURNED out of the lambda,
//   compiled clean, printed a value, and leaked 56 bytes.
//
// The fix is scope, not a new rule: `checkOneLambdaBodyEscapes` saves,
// clears and re-seeds `states_` and `localDepth_` from ONE lambda body, then
// feeds the SAME two checks with `EscapeScope::LambdaBody`. Every row below
// therefore asserts the METHOD-level diagnostic TEXT verbatim — if a second
// spelling of either rule ever appears, these rows fail.
//
// SOUNDNESS DISCIPLINE (as in catch_escape_test.cpp): every positive is paired
// with a structurally-similar but spec-LEGAL negative that the fix must NOT
// reject. The three boundaries that matter here are (a) the depth comparison
// (a holder declared INSIDE the arm is legal), (b) the escape — §28.3:
// "Capture is permitted for closures that are immediately invoked" — and
// (c) the obligation (a value-typed capture carries none).

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;

namespace {

// A helper class with a disposable instance method, and a `Use` that borrows
// an exception for the duration of a call — the two building blocks the rows
// below need. `R` is an ordinary class, so `using var r := new R();` is an
// owned scope-bound local (§28.3) whose capture is a §28.9 borrow.
std::string wrap(const std::string& method) {
    return std::string(
        "module M; "
        "class R "
        "  public method V(): int return 5; end V; "
        "end R; "
        "class C ") + method +
        "  private method Use(e: Exception): int return 0; end Use; "
        "  private method DoWork() end DoWork; "
        "end C;";
}

// ---------------------------------------------------------------- #3565 ----

// POSITIVE (§31.1, #3565): the filed repro. A closure capturing the catch
// binding is ASSIGNED to `f`, declared outside the `try` but inside the lambda
// body. `f` outlives the arm, the runtime disposes the exception when the arm
// completes, and calling `f` afterwards reads freed memory. Rejected at method
// level since fix #3520; unreachable inside a lambda body until #3565.
BEZEL_TEST(lambda_body_escape, catch_closure_assign_to_outer_local_rejected_31_1) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Handle(): int "
        "  var mk := lambda(): int => do "
        "    var f := lambda(): int => 0; "
        "    try DoWork(); "
        "    catch var e: Exception => do "
        "      f := lambda(): int => Use(e); "
        "    end; "
        "    end; "
        "    return f() + f(); "
        "  end; "
        "  return mk(); "
        "end Handle;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "caught exception `e` may not be captured by a closure assigned "
           "to `f`"));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "(§31.1"));
}

// POSITIVE (§31.1, #3565): the PLAIN-STORE spelling of the same channel — no
// closure at all. fix #3520 gave it its own wording at method level because
// the closure sentence would name a closure the program does not contain; it
// was equally unreachable inside a lambda body.
BEZEL_TEST(lambda_body_escape, catch_plain_store_to_outer_local_rejected_31_1) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Handle(): int "
        "  var mk := lambda(): int => do "
        "    var holder: Exception? := null; "
        "    try DoWork(); "
        "    catch var e: Exception => do "
        "      holder := e; "
        "    end; "
        "    end; "
        "    return 0; "
        "  end; "
        "  return mk(); "
        "end Handle;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "caught exception `e` may not be assigned to `holder`"));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "(§31.1"));
}

// NEGATIVE (§31.1, #3565) — the DEPTH boundary. `n` is declared inside the
// catch arm, so it dies with the arm and can never outlive the exception.
// §31.1 forbids capture "by a closure that MAY outlive the arm"; this one
// cannot. The rejection is a real depth comparison, and this row proves the
// lambda body's re-seeded `localDepth_` still makes it.
BEZEL_TEST(lambda_body_escape, catch_closure_assign_to_inner_local_accepted_31_1) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Handle(): int "
        "  var mk := lambda(): int => do "
        "    var total := 0; "
        "    try DoWork(); "
        "    catch var e: Exception => do "
        "      var n := lambda(): int => 0; "
        "      n := lambda(): int => Use(e); "
        "      total := n() + n(); "
        "    end; "
        "    end; "
        "    return total; "
        "  end; "
        "  return mk(); "
        "end Handle;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// NEGATIVE (§31.1, #3565) — the spec's own remedy. "copy the relevant data out
// of it ... into separate owned values": `k` is an `int` copy, nothing borrows
// the exception, and the store into the outer `f` is legal.
BEZEL_TEST(lambda_body_escape, catch_copied_value_assign_to_outer_local_accepted_31_1) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Handle(): int "
        "  var mk := lambda(): int => do "
        "    var f := lambda(): int => 0; "
        "    try DoWork(); "
        "    catch var e: Exception => do "
        "      var k := e.Message.Length; "
        "      f := lambda(): int => k; "
        "    end; "
        "    end; "
        "    return f() + f(); "
        "  end; "
        "  return mk(); "
        "end Handle;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------- #3564 ----

// POSITIVE (§28.9, #3564): the filed repro. `r` is an owned scope-bound local
// of the lambda body's inner `do ... end`; the closure capturing it is
// RETURNED out of the lambda, so it outlives `r`'s disposal.
BEZEL_TEST(lambda_body_escape, owned_local_returned_closure_rejected_28_9) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Handle(): int "
        "  var mk := lambda(): () -> int => do "
        "    do "
        "      using var q := new R(); "
        "      return lambda(): int => q.V(); "
        "    end; "
        "    return lambda(): int => 0; "
        "  end; "
        "  var g := mk(); "
        "  return g() + g(); "
        "end Handle;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "returned closure captures `q` whose source is disposed at scope "
           "end"));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "(§28.9 closure-capture escape"));
}

// POSITIVE (§28.9, #3564): the same escape with the owned local at the lambda
// body's TOP level rather than in a nested block — the depth is irrelevant to
// this leg (the return leaves the whole lambda invocation either way), and the
// row proves the seed is not accidentally scoped to nested blocks only.
BEZEL_TEST(lambda_body_escape, owned_top_level_local_returned_closure_rejected_28_9) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Handle(): int "
        "  var mk := lambda(): () -> int => do "
        "    using var q := new R(); "
        "    return lambda(): int => q.V(); "
        "  end; "
        "  var g := mk(); "
        "  return g() + g(); "
        "end Handle;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "returned closure captures `q` whose source is disposed at scope "
           "end"));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "(§28.9 closure-capture escape"));
}

// NEGATIVE (§28.9, #3564) — the ESCAPE boundary. Same owned local, same
// capture, no return: the closure is invoked in place inside the block that
// owns `q`. §28.9 bounds the capture by "may not outlive that scope"; §28.3
// spells the admission out — "Capture is permitted for closures that are
// immediately invoked" — so this is the legal shape the rule leaves alone.
BEZEL_TEST(lambda_body_escape, owned_local_closure_invoked_in_lambda_accepted_28_9) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Handle(): int "
        "  var mk := lambda(): int => do "
        "    var total := 0; "
        "    do "
        "      using var q := new R(); "
        "      var f := lambda(): int => q.V(); "
        "      total := f() + f(); "
        "    end; "
        "    return total; "
        "  end; "
        "  return mk(); "
        "end Handle;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// NEGATIVE (§28.9, #3564) — the OBLIGATION boundary. The closure escapes the
// lambda exactly as in the positive rows, but captures a value-typed local,
// which is never Owned (§28.3) and carries no scope-bound discharge
// obligation. A seed that marked every lambda-body local Owned would reject
// this.
BEZEL_TEST(lambda_body_escape, value_local_returned_closure_accepted_28_9) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Handle(): int "
        "  var mk := lambda(): () -> int => do "
        "    var k := 7; "
        "    return lambda(): int => k; "
        "  end; "
        "  var g := mk(); "
        "  return g() + g(); "
        "end Handle;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// NEGATIVE (§28.9, #3564) — the LAMBDA PARAMETER boundary. A lambda's own
// parameter is not a local of its body and is never seeded, so a closure
// returned over it stays accepted.
BEZEL_TEST(lambda_body_escape, lambda_param_returned_closure_accepted_28_9) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Handle(): int "
        "  var mk := lambda(n: int): () -> int => do "
        "    return lambda(): int => n; "
        "  end; "
        "  var g := mk(3); "
        "  return g() + g(); "
        "end Handle;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ------------------------------------------------- scope of the fix --------

// POSITIVE (§28.9 / §31.1): NESTED lambdas. `collectBodyLambdas` produces a
// FLAT list containing the inner lambda too, and each lambda is seeded from
// its OWN body — so an escape wholly inside the inner lambda is still caught.
BEZEL_TEST(lambda_body_escape, owned_local_returned_closure_in_nested_lambda_rejected_28_9) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Handle(): int "
        "  var outer := lambda(): int => do "
        "    var inner := lambda(): () -> int => do "
        "      using var q := new R(); "
        "      return lambda(): int => q.V(); "
        "    end; "
        "    var g := inner(); "
        "    return g(); "
        "  end; "
        "  return outer(); "
        "end Handle;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "returned closure captures `q` whose source is disposed at scope "
           "end"));
}

// NEGATIVE — the deliberate SCOPE BOUNDARY of this fix, pinned so it is a
// recorded choice rather than a later surprise.
//
// `q` is a METHOD-scope owned local. The lambda body's re-seeded `states_`
// holds only the LAMBDA's own owned obligations, so the §28.9 leg does not see
// `q` from inside the lambda — deliberately. A `return` inside a lambda leaves
// only THAT LAMBDA; whether the lambda's own value then escapes the METHOD is
// escape analysis this pre-pass does not have, and issue #3564 records it as
// the open half. Here the closure is invoked while `q` is still alive, so the
// program is genuinely memory-safe (measured: exit 0, prints 10) and accepting
// it is CORRECT, not merely tolerated.
//
// The unsafe sibling of this shape — `return mk();` out of the method, where
// the returned expression is a CallExpr and no lambda is visible to the
// method-level return check either — still compiles and SIGSEGVs; it is filed
// separately with its measured repro, and it is NOT what this row pins.
BEZEL_TEST(lambda_body_escape, method_scope_owned_local_capture_still_accepted_28_9) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Handle(): int "
        "  using var q := new R(); "
        "  var mk := lambda(): () -> int => do "
        "    return lambda(): int => q.V(); "
        "  end; "
        "  var g := mk(); "
        "  return g() + g(); "
        "end Handle;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// NEGATIVE — the EXPRESSION-BODIED lambda path, and the regression this fix's
// own first draft introduced (kept as a row, per §7: a fix that reverses a
// rejection owes the test that pins the reversal).
//
// `checkOneLambdaBodyEscapes` clears and re-seeds `states_` before running the
// §28.9 leg; an expression-bodied lambda has no statements to seed FROM, so
// the clear must still happen and leave the map EMPTY. The first draft
// returned early before the clear, and the leg then resolved the enclosing
// METHOD's `q` and rejected this program at 11:55 — a false reject of a
// memory-safe program (`g` is called while `q` is in scope). Whether such a
// closure escapes the METHOD is issue #3702's question, not this fix's.
BEZEL_TEST(lambda_body_escape, exprbody_lambda_over_method_local_accepted_28_9) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Handle(): int "
        "  using var q := new R(); "
        "  var mk := lambda(): () -> int => lambda(): int => q.V(); "
        "  var g := mk(); "
        "  return g() + g(); "
        "end Handle;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// POSITIVE (§31.1, #3565) — the CASE-ARM axis, where two walker repairs meet.
// `narrow #3520` could not reject even the METHOD-level spelling of this shape
// (issue #3552: `collectLocalDepthsStmt` had no `ast::CaseStmt` arm, so a
// case-arm catch binding got no depth and every consumer's guard bailed). fix
// #3656 put that walker on `forEachNestedStmt`, the single statement grammar,
// which does walk case arms. This row pins that the repairs COMPOSE: the depth
// seed #3565 rebuilds for the lambda body reaches an arm binding nested in a
// `case` inside that body.
BEZEL_TEST(lambda_body_escape, catch_in_case_arm_in_lambda_rejected_31_1) {
    auto r = parseAndCheckOwnership(wrap(
        "public method Handle(k: int): int "
        "  var mk := lambda(): int => do "
        "    var f := lambda(): int => 0; "
        "    case k of "
        "      1 => do "
        "        try DoWork(); "
        "        catch var e: Exception => do "
        "          f := lambda(): int => Use(e); "
        "        end; "
        "        end; "
        "      end, "
        "      _ => do end "
        "    end case; "
        "    return f() + f(); "
        "  end; "
        "  return mk(); "
        "end Handle;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "caught exception `e` may not be captured by a closure assigned "
           "to `f`"));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "(§31.1"));
}

}  // namespace
