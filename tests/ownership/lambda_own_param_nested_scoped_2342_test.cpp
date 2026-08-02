// Ownership tests for #2342 (§28.14 v0.177, residual from #1432): a nested
// closure capturing a LAMBDA's own function-typed parameter, forwarded INLINE
// to a `scoped` position, must be recognized as a capture of a call-bounded
// parameter and rejected — the stored closure would extend the callback past
// its call (the §28.14 transitive escape judgment).
//
// #1432 seeded a lambda's own plain function-typed parameters into
// `callBoundedParams_` so the CALLEE-SIDE sink checks (field/collection store,
// return, forward-to-scoped) could see them, AND so that a lambda's own param
// forwarded DIRECTLY (the identifier `g` in `Install(g)`) or via a carrier
// local (`var h := lambda()=>g(); Install(h)`) reaches the call-bounded check
// in `checkScopedAdmission`'s identifier branch. But the LAMBDA-ARGUMENT
// branch's capture-reduction gate still used `methodParamSet_` (which holds
// only the enclosing METHOD's own formals) to skip "lambda-internal" params —
// so a param of an ENCLOSING LAMBDA captured by a nested closure passed INLINE
// as a lambda literal (`Install(lambda() => f())`) was wrongly skipped as
// "lambda-internal", never reaching the call-bounded check. The shape:
//
//   var outer := lambda(f: () -> ()) => do
//     Install(lambda() => f());   // inner closure captures f (outer lambda's
//   end;                            //   call-bounded param), forwarded to scoped
//
// was silently ADMITTED (unsound). The carrier-local and direct-identifier
// siblings of this shape were already rejected (#1432); only the inline-lambda
// form slipped through, because only it routes through the capture-reduction
// gate. The fix records the FORWARDED lambda's own params (+ nested lambdas'
// params within it) in a per-admission set and skips only those; every other
// ParameterSymbol — enclosing method OR enclosing lambda — is classified as a
// real capture and reaches the call-bounded check.
//
// §28.14, v0.177: "captured by a closure that itself escapes the callee (judged
// by these same rules, applied transitively), or forwarded into a `scoped`
// parameter position" is forbidden for a call-bounded callback; the inline
// nested-closure-forwarded-to-scoped shape IS that transitive escape, and a
// lambda is a user-authored callee exactly as a method is (#1432).

#include <string>

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

bool diagContains(const bezel::test::OwnershipResult& r,
                  const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

}  // namespace

// ===========================================================================
// The #2342 regression: the inline nested-closure shape. FAILS without the fix
// (silently admitted — no error) and PASSES with it (rejected). The carrier-
// local and direct-identifier siblings were already rejected by #1432; only
// this inline-literal form reached the capture-reduction gate.
// ===========================================================================

// #2342 / §28.14 v0.177: the issue's canonical shape — `Install(lambda() => f())`
// from inside the outer lambda's body, where `f` is the outer lambda's own plain
// function-typed parameter. Without the fix the `methodParamSet_` gate skipped
// `f` (a lambda param, not a method param) and the closure was silently
// admitted; with the fix `f` is recognized as a capture of a call-bounded
// parameter and the admission is rejected.
BEZEL_TEST(lambda_own_param_nested_scoped_2342,
          inline_nested_closure_capturing_call_bounded_param_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  private scoped _cb: (() -> ())?; "
        "  public method Install(scoped cb: () -> ()) _cb := cb; end Install; "
        "  public method Run() "
        "    var outer := lambda(f: () -> ()) => do "
        "      Install(lambda() => f()); "
        "    end; "
        "    outer(lambda() => ()); "
        "  end Run; "
        "  destructor end; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(
        r, "closure bound to a `scoped` parameter captures `f`, a call-bounded "
           "function-typed parameter"));
}

// #2342 / §28.14 v0.177: a second inline-lambda variant — the inner closure
// captures `g` and is forwarded to a `scoped` CONSTRUCTOR parameter (form 1),
// not a method. The fix's capture-reduction gate is the same for ctor and
// method scoped positions; this guards that the constructor path also reaches
// the call-bounded check for an enclosing-lambda param.
BEZEL_TEST(lambda_own_param_nested_scoped_2342,
          inline_nested_closure_to_scoped_ctor_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Runner "
        "  public scoped _cb: (() -> ())?; "
        "  public constructor(scoped cb: () -> ()) _cb := cb; end; "
        "  destructor end; "
        "end Runner; "
        "class C "
        "  public method Run() "
        "    var outer := lambda(g: () -> ()) => do "
        "      var r := new Runner(lambda() => g()); "
        "    end; "
        "    outer(lambda() => ()); "
        "  end Run; "
        "  destructor end; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(
        r, "closure bound to a `scoped` parameter captures `g`, a call-bounded "
           "function-typed parameter"));
}

// ===========================================================================
// Accept controls: legitimate captures that MUST stay admitted (guards against
// an over-broad fix that rejects shapes §28.14 admits)
// ===========================================================================

// #2342 ACCEPT: a VALUE-TYPED enclosing-lambda parameter captured by an inline
// nested closure, forwarded to `scoped` under a `using var` bound. A value-
// typed capture lives in its heap capture cell (which the closure keeps alive)
// — no lifetime hazard (§28.14 v1 bound #1). The enclosing-lambda param IS now
// classified as a capture (so the bound must resolve), and the using-var bound
// admits it. Guards against the fix over-rejecting value-typed captures.
BEZEL_TEST(lambda_own_param_nested_scoped_2342,
          value_typed_enclosing_lambda_param_captured_admitted) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Scope "
        "  public scoped _cb: (() -> int)?; "
        "  public method Spawn(scoped cb: () -> int) _cb := cb; end Spawn; "
        "  destructor end; "
        "end Scope; "
        "class C "
        "  public method Run(): int "
        "    var maker := lambda(n: int) => do "
        "      using var sc := new Scope(); "
        "      sc.Spawn(lambda() => n); "
        "    end; "
        "    maker(42); "
        "    return 0; "
        "  end Run; "
        "  destructor end; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// #2342 ACCEPT: the FORWARDED lambda's OWN parameter is NOT a capture — it is
// internal to the forwarded closure. `Install(lambda(x: int) => x)` from inside
// an enclosing lambda: `x` is the forwarded lambda's own param (excluded by the
// internal-param set the fix records), and the forwarded closure is otherwise
// capture-free, so the admission holds. Guards against the fix wrongly treating
// the forwarded lambda's own param as a capture (the regression that would
// re-introduce the over-broad `methodParamSet_` skip in the other direction).
BEZEL_TEST(lambda_own_param_nested_scoped_2342,
          forwarded_lambda_own_param_not_a_capture_admitted) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  private scoped _cb: ((int) -> int)?; "
        "  public method Install(scoped cb: (int) -> int) _cb := cb; end Install; "
        "  public method Run() "
        "    var outer := lambda(f: () -> int) => do "
        "      Install(lambda(x: int) => x); "
        "    end; "
        "    outer(lambda() => 1); "
        "  end Run; "
        "  destructor end; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// #2342 ACCEPT: an inline nested closure capturing an enclosing lambda's call-
// bounded parameter and forwarded to a PLAIN (non-`scoped`) parameter is
// admitted — every closure (capture-bearing or not) may be passed to a call-
// bounded parameter (§28.14: "any closure may be passed to a call-bounded
// parameter"). Guards against the fix rejecting the plain-forward case.
BEZEL_TEST(lambda_own_param_nested_scoped_2342,
          inline_nested_closure_to_plain_param_admitted) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static method Inner(h: () -> int): int return h(); end Inner; "
        "  public method Run(f: () -> int): int "
        "    var apply := lambda(g: () -> int): int => "
        "      C.Inner(lambda() => g()); "
        "    return apply(f); "
        "  end Run; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ===========================================================================
// Consistency guard: the METHOD-param sibling (pre-#2342, already working via
// methodParamSet_) still rejects after the gate change — method params are not
// in the forwarded lambda's internal-param set, so they reach the call-bounded
// check exactly as before.
// ===========================================================================

// #2342 consistency guard: an inline closure capturing an enclosing METHOD's
// (not lambda's) call-bounded parameter, forwarded to `scoped`, was already
// rejected via the methodParamSet_ path before this fix. The gate change (skip
// only forwarded-lambda-internal params) must keep this rejection.
BEZEL_TEST(lambda_own_param_nested_scoped_2342,
          method_param_captured_by_inline_closure_forwarded_to_scoped_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  private scoped _cb: (() -> ())?; "
        "  public method Install(scoped cb: () -> ()) _cb := cb; end Install; "
        "  public method Run(f: () -> ()) "
        "    Install(lambda() => f()); "
        "  end Run; "
        "  destructor end; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(
        r, "closure bound to a `scoped` parameter captures `f`, a call-bounded "
           "function-typed parameter"));
}
