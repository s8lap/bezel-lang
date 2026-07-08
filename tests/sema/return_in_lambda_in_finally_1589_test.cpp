// Spec-derived tests for §21.7 (issue #1589): `return` inside a LAMBDA that
// is itself lexically nested inside an enclosing method's `finally` block.
//
// §21.7: "`return` inside a lambda exits the lambda, not the method that
// defined it." A lambda is its own callable (§21.5's `finallyLoopBoundary_`
// reset for break/continue makes the same point for jumps): its body is a
// fresh control-flow scope, so the enclosing method's `return`-inside-
// `finally` restriction (§21.7/§31.2: "`return` inside a `finally` block is
// a compile error") must not apply to a `return` that targets the lambda.
//
// Bug #1589: `finallyDepth_` (the counter the §31.2 check reads) was reset
// across the lambda boundary for `loopDepth_`/`catchDepth_`/
// `finallyLoopBoundary_` (per #1578) but NOT for itself, so a `return`
// inside a lambda declared inside a `finally` was wrongly rejected as if it
// escaped that `finally`. These tests verify the fix, and that the original
// §31.2 guard (a bare `return` directly in a `finally`, not inside a lambda)
// still rejects correctly — the gate must not be over-opened.

#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// Convenience: wrap a body inside a static int method.
std::string wrapIntMethod(const std::string& body) {
    return "module M; class C "
           "public static method Foo(): int "
           + body +
           " end Foo; "
           "end C;";
}

} // namespace

// §21.7: a bare `return;` inside a lambda that is itself declared inside a
// `finally` block is legal — the return targets the lambda, not the method
// whose `finally` encloses it.
BEZEL_TEST(return_in_lambda_in_finally_1589,
           bare_return_in_lambda_in_finally_ok_spec_21_7) {
    auto r = parseSemaAll(wrapIntMethod(
        "var n := 0; "
        "try n := n + 1; "
        "finally "
        "  var f := () => do return; end; "
        "  f(); "
        "end; "
        "return n; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §21.7: same, but the lambda returns a value (matches issue #1589's repro
// shape: a loop with a `break` inside the lambda, then `return i;`).
BEZEL_TEST(return_in_lambda_in_finally_1589,
           value_return_in_lambda_in_finally_ok_spec_21_7) {
    auto r = parseSemaAll(wrapIntMethod(
        "var n := 0; "
        "try n := n + 1; "
        "finally "
        "  var f := () => do "
        "    var i := 0; "
        "    while i < 3 do "
        "      i := i + 1; "
        "      if i = 2 then break; end; "
        "    end; "
        "    return i; "
        "  end; "
        "  n := n + f(); "
        "end; "
        "return n; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §31.2 guard sanity: a bare `return` directly in a `finally` (NOT inside a
// lambda) must still be rejected — the fix must not over-open the gate.
BEZEL_TEST(return_in_lambda_in_finally_1589,
           bare_return_directly_in_finally_still_rejected_spec_31_2) {
    auto r = parseSemaAll(wrapIntMethod(
        "var n := 0; "
        "try n := n + 1; "
        "finally return n; "
        "end; "
        "return n; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §31.2 guard sanity: a bare `return` directly in a `finally`, AFTER a
// lambda declared earlier in that same `finally` has already been visited
// and exited, must still be rejected — verifies `finallyDepth_` is restored
// (not left at 0) once the lambda visit completes.
BEZEL_TEST(return_in_lambda_in_finally_1589,
           bare_return_in_finally_after_lambda_still_rejected_spec_31_2) {
    auto r = parseSemaAll(wrapIntMethod(
        "var n := 0; "
        "try n := n + 1; "
        "finally "
        "  var f := () => do return 1; end; "
        "  n := n + f(); "
        "  return n; "
        "end; "
        "return n; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §31.2 guard sanity: a `return` inside a NESTED `try`/`finally` that is
// itself inside the lambda's body (not the lambda's own top-level statement
// list) is still rejected — the lambda resets `finallyDepth_` to 0 on
// entry, but a `finally` opened inside the lambda body increments it again,
// so a `return` in THAT nested finally is still an escape from a finally
// (the lambda's own finally, not the outer method's).
BEZEL_TEST(return_in_lambda_in_finally_1589,
           return_in_nested_finally_inside_lambda_still_rejected_spec_31_2) {
    auto r = parseSemaAll(wrapIntMethod(
        "var n := 0; "
        "try n := n + 1; "
        "finally "
        "  var f := () => do "
        "    try "
        "      var x := 1; "
        "    finally "
        "      return x; "
        "    end; "
        "  end; "
        "  n := n + f(); "
        "end; "
        "return n; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §21.5/§31.2 sanity (companion to #1578): `break` targeting a loop declared
// wholly inside the lambda's own body, where the lambda sits inside an
// enclosing `finally`, is also legal — the same lambda-boundary reset covers
// `finallyLoopBoundary_` alongside `finallyDepth_`.
BEZEL_TEST(return_in_lambda_in_finally_1589,
           break_in_loop_inside_lambda_in_finally_ok_spec_21_5) {
    auto r = parseSemaAll(wrapIntMethod(
        "var n := 0; "
        "try n := n + 1; "
        "finally "
        "  var f := () => do "
        "    var total := 0; "
        "    while total < 10 do "
        "      total := total + 1; "
        "      if total = 3 then break; end; "
        "    end; "
        "    return total; "
        "  end; "
        "  n := n + f(); "
        "end; "
        "return n; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
