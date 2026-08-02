// Ownership tests for #1432 (§28.14 v1 subset, v0.177): a LAMBDA's own
// function-typed parameters are call-bounded within its own body, exactly as
// an enclosing method's/constructor's formals are.
//
// Before this fix, `callBoundedParams_` (the set the callee-side sink checks
// consult) was seeded ONLY from the enclosing method's/constructor's own
// `paramSyms` (Ownership.cpp `run()`); a lambda literal's OWN parameters were
// never added, so a plain function-typed parameter of a LAMBDA could be
// stored / returned / forwarded from inside that lambda's own body with none
// of the §28.14 v0.177 callee-side verification a method formal gets:
//
//   var sink := lambda(cb: () -> ()) => do _f := cb; end;   // unverified
//
// §28.14, v0.177: "A function-typed parameter declared without `scoped` is
// call-bounded … it may not be stored in a field of any object, stored into
// any collection that is itself reachable beyond the call …, returned,
// captured by a closure that itself escapes the callee (judged by these same
// rules, applied transitively), or forwarded into a `scoped` parameter
// position … These obligations are compiler-verified for user-authored
// callees" — a lambda literal is a user-authored callee like any other, so
// this fix seeds the lambda's own plain function-typed parameter symbols into
// `callBoundedParams_` for the duration of its own body walk
// (`seedLambdaCallBoundedParams`, called from `callBoundedWalkExpr`'s
// LambdaExpr case), pushing before and popping after so one lambda's own
// parameters never leak into a sibling's or the enclosing method's
// classification.
//
// NOTE on scope: a lambda's own EXPRESSION-body (`lambda(g) => g`) treating
// its bare value as an implicit RETURN sink was a separate, pre-existing gap
// in the callee-side walk (it is not specific to a lambda's own parameters —
// the same gap exists for the enclosing method's captured parameter forwarded
// through a nested lambda's `=>` body); it is closed by #2343, whose cells
// live in the "EXPRESSION body" sections below. The BLOCK-bodied
// `do return g; end` form (the shape the #1432 cells below exercise, matching
// the issue's own repro style) was already covered.

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
// Forbidden sink: field store, from INSIDE the lambda's own body
// ===========================================================================

// #1432 / §28.14 v0.177: the issue's own repro shape — a lambda's own plain
// function-typed parameter stored into a field of `this` from inside the
// lambda's body is the same "stored in a field of any object" sink a method
// formal gets.
BEZEL_TEST(lambda_own_param_call_bounded_1432, field_store_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  private _f: (() -> ())?; "
        "  public method Set() "
        "    var sink := lambda(cb: () -> ()) => do "
        "      _f := cb; "
        "    end; "
        "    sink(lambda() => ()); "
        "  end Set; "
        "  destructor end; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(
        r, "parameter `cb` is call-bounded; declare it `scoped` to store it"));
}

// #1432 / §28.14 v0.177: storing into ANOTHER object's field is the same
// sink (mirrors call_bounded_2814.other_object_field_store_rejected, but the
// callee doing the storing is a lambda, not a method).
BEZEL_TEST(lambda_own_param_call_bounded_1432,
          other_object_field_store_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Holder "
        "  public _fn: (() -> int)?; "
        "  destructor end; "
        "end Holder; "
        "class C "
        "  public static method Run(h: Holder) "
        "    var stash := lambda(g: () -> int, target: Holder) => do "
        "      target._fn := g; "
        "    end; "
        "    stash(lambda() => 1, h); "
        "  end Run; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "call-bounded"));
}

// #1432 / §28.14 v0.177 transitivity: a local bound from the lambda's own
// parameter is the same value — storing the alias in a field is the same
// sink (mirrors call_bounded_2814.field_store_via_local_alias_rejected).
BEZEL_TEST(lambda_own_param_call_bounded_1432,
          field_store_via_local_alias_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  private _f: (() -> ())?; "
        "  public method Set() "
        "    var sink := lambda(cb: () -> ()) => do "
        "      var alias := cb; "
        "      _f := alias; "
        "    end; "
        "    sink(lambda() => ()); "
        "  end Set; "
        "  destructor end; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "call-bounded"));
}

// ===========================================================================
// Forbidden sink: collection store (beyond the call), from the lambda's body
// ===========================================================================

// #1432 / §28.14 v0.177: storing into a FIELD-reachable collection from
// inside the lambda's own body is rejected (mirrors
// call_bounded_2814.store_into_field_collection_rejected).
BEZEL_TEST(lambda_own_param_call_bounded_1432,
          store_into_field_collection_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  private _handlers: Array<() -> int> := []; "
        "  public method Register() "
        "    var sink := lambda(g: () -> int) => do "
        "      _handlers.Add(g); "
        "    end; "
        "    sink(lambda() => 1); "
        "  end Register; "
        "  destructor end; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "collection reachable beyond the call"));
}

// #1432 / §28.14 v0.177 transitivity: a closure capturing the lambda's own
// parameter, stored into a beyond-call collection, is the same sink (mirrors
// call_bounded_2814.capturing_closure_into_field_collection_rejected).
BEZEL_TEST(lambda_own_param_call_bounded_1432,
          capturing_closure_into_field_collection_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  private _handlers: Array<() -> int> := []; "
        "  public method Register() "
        "    var sink := lambda(g: () -> int) => do "
        "      _handlers.Add(lambda() => g()); "
        "    end; "
        "    sink(lambda() => 1); "
        "  end Register; "
        "  destructor end; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "call-bounded"));
}

// ===========================================================================
// Forbidden sink: return, from the lambda's own BLOCK body
// ===========================================================================

// #1432 / §28.14 v0.177: `return`-ing the lambda's own parameter from inside
// its own block body is the same "returned" sink a method formal gets.
BEZEL_TEST(lambda_own_param_call_bounded_1432, return_block_body_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public method Set(f: () -> int): () -> int "
        "    var pass := lambda(g: () -> int): () -> int => do "
        "      return g; "
        "    end; "
        "    return pass(f); "
        "  end Set; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "call-bounded"));
}

// ===========================================================================
// Forbidden sink: implicit return, from the lambda's own EXPRESSION body (#2343)
//
// #2343 (§28.14 v0.177/v0.196, twin of #1432's block-body `return` cell above):
// a lambda's EXPRESSION body `=> E` is an implicit RETURN of `E` (§27.3), so a
// call-bounded parameter (or a closure capturing one) returned through `=> g`
// escapes the callback exactly as `return g;` does. `callBoundedWalkExpr`'s
// LambdaExpr case previously walked the exprBody only STRUCTURALLY (a top-level
// identifier `g` hit no sink), so the expr-body twin of the block-body return
// was silently admitted. The fix treats the exprBody as an implicit return sink
// — mirroring callBoundedWalkStmt's ReturnStmt case, with the SAME §28.14
// *Call-bounded × deferred activation* (v0.196) frame-residence carve-out
// (computed against the LAMBDA's own return type, not the enclosing method's).
// ===========================================================================

// #2343 / §28.14 v0.177: the issue's canonical shape — a lambda whose OWN plain
// function-typed parameter is the whole expression body (`=> g`), an implicit
// return of the call-bounded value. Without the fix the structural walk found
// no sink (a top-level identifier) and this was silently ADMITTED; with the fix
// the implicit return is the "returned" sink. The block-body twin
// (`return_block_body_rejected` above) was already rejected by #1432.
BEZEL_TEST(lambda_own_param_call_bounded_1432,
          return_expr_body_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public method Set(f: () -> int): () -> int "
        "    var pass := lambda(g: () -> int): () -> int => g; "
        "    return pass(f); "
        "  end Set; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "call-bounded"));
    BEZEL_EXPECT_TRUE(diagContains(r, "may not be returned"));
}

// #2343 / §28.14 v0.177: the broader aspect — the ENCLOSING METHOD's call-
// bounded parameter captured and returned through a LOCAL lambda's `=>` body
// (`lambda() => f`, where `f` is the method's formal). The implicit return of
// the captured callback escapes it exactly as `return f;` does; before the fix
// the structural walk missed the top-level capture. (The block-body twin
// `do return f; end` was already caught via the ReturnStmt handler.)
BEZEL_TEST(lambda_own_param_call_bounded_1432,
          enclosing_param_implicit_return_via_expr_body_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public method Run(f: () -> int): () -> int "
        "    var cap := lambda() => f; "
        "    return cap(); "
        "  end Run; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "call-bounded"));
    BEZEL_EXPECT_TRUE(diagContains(r, "may not be returned"));
}

// ===========================================================================
// Accept controls for the #2343 carve-out: §28.14 *Call-bounded × deferred
// activation* (v0.196) frame residence through a lambda's `=>` body — the
// spec's "expressible neighbor" idiom (`xs.Where(f)`), which must stay ADMITTED.
// A bare "treat the exprBody as a return sink" would wrongly reject these; the
// fix's per-lambda frame-residence carve-out (lambdaReturnTypeIsDeferredActivation)
// admits them. Guards against an over-broad fix.
// ===========================================================================

// #2343 / §28.14 v0.196 ACCEPT: forwarding the lambda's own call-bounded
// parameter into a deferred-activation call (`xs.Where(g)`) and returning the
// resulting frame UNDER the lambda's DECLARED deferred-activation return type
// is announced frame residence — admitted. The lambda twin of
// call_bounded_2814.forward_into_lazy_returned_under_deferred_type_clean.
BEZEL_TEST(lambda_own_param_call_bounded_1432,
          expr_body_frame_residence_declared_return_type_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static method Apply(xs: Array<int>, "
        "                             f: (int) -> bool): IEnumerable<int> "
        "    var mk := lambda(g: (int) -> bool): IEnumerable<int> "
        "             => xs.Where(g); "
        "    return mk(f); "
        "  end Apply; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// #2343 / §28.14 v0.196 ACCEPT: the same frame-residence forward with an
// INFERRED lambda return type (no `: IEnumerable<int>` ascription). The
// inferred return type equals the exprBody's resolved type, so the
// announcement composes exactly when the body is a frame — admitted.
BEZEL_TEST(lambda_own_param_call_bounded_1432,
          expr_body_frame_residence_inferred_return_type_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static method Apply(xs: Array<int>, "
        "                             f: (int) -> bool): IEnumerable<int> "
        "    var mk := lambda(g: (int) -> bool) => xs.Where(g); "
        "    return mk(f); "
        "  end Apply; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// #2531 / §28.14 v0.196: the #2343 lambda-`=>` shape with a NULLABLE widening
// lambda return type `: IEnumerable<int>?`. The carve-out
// (lambdaReturnTypeIsDeferredActivation → isDeferredActivationTypeNode) is
// false for the nullable adornment, so the implicit return of `xs.Where(g)`
// falls through to `reportCallBounded`. Same shape as the method case above
// — confirms #2343's parity across the three return-sink paths (method
// block, method `=>`, lambda `=>`) once #2531's nullable fix is in.
BEZEL_TEST(lambda_own_param_call_bounded_1432,
          expr_body_frame_returned_under_nullable_widening_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static method Apply(xs: Array<int>, "
        "                             f: (int) -> bool): IEnumerable<int> "
        "    var mk := lambda(g: (int) -> bool): IEnumerable<int>? "
        "             => xs.Where(g); "
        "    return mk(f); "
        "  end Apply; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "call-bounded"));
    BEZEL_EXPECT_TRUE(diagContains(r, "may not be returned"));
}

// ===========================================================================
// Forbidden sink: forwarding into a `scoped` parameter position
// ===========================================================================

// #1432 / §28.14 v0.177: the lambda's own parameter forwarded into a call's
// `scoped` position is rejected exactly as a method formal's would be — the
// lambda cannot vouch for a capture set it cannot see either (mirrors
// call_bounded_2814.forward_to_scoped_param_rejected).
BEZEL_TEST(lambda_own_param_call_bounded_1432,
          forward_to_scoped_param_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  private scoped _cb: (() -> ())?; "
        "  public method Install(scoped cb: () -> ()) _cb := cb; end Install; "
        "  public method Relay() "
        "    var pass := lambda(g: () -> ()) => do "
        "      Install(g); "
        "    end; "
        "    pass(lambda() => ()); "
        "  end Relay; "
        "  destructor end; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(r, "call-bounded"));
    BEZEL_EXPECT_TRUE(diagContains(r, "scoped"));
}

// ===========================================================================
// Accept controls: legal uses of a lambda's own function-typed parameter
// (guards against an over-broad fix that rejects merely CALLING it)
// ===========================================================================

// #1432 / §28.14 v0.177 ACCEPT: invoking the lambda's own parameter is the
// admitted use — a call-bounded value may always be invoked.
BEZEL_TEST(lambda_own_param_call_bounded_1432, invoke_only_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public method Run(f: () -> int): int "
        "    var apply := lambda(g: () -> int): int => g(); "
        "    return apply(f); "
        "  end Run; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// #1432 / §28.14 v0.177 ACCEPT: binding the lambda's own parameter to a
// local and invoking through it is a local binding — clean (mirrors
// call_bounded_2814.bind_to_local_and_invoke_clean).
BEZEL_TEST(lambda_own_param_call_bounded_1432,
          bind_to_local_and_invoke_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public method Run(f: () -> int): int "
        "    var apply := lambda(g: () -> int): int => do "
        "      var h := g; "
        "      return h() + h(); "
        "    end; "
        "    return apply(f); "
        "  end Run; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// #1432 / §28.14 v0.177 ACCEPT: forwarding the lambda's own parameter to
// ANOTHER plain (non-`scoped`) function-typed parameter is plain-to-plain
// forwarding — clean (mirrors call_bounded_2814.plain_to_plain_forwarding_clean).
BEZEL_TEST(lambda_own_param_call_bounded_1432,
          plain_to_plain_forwarding_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static method Inner(h: () -> int): int return h(); end Inner; "
        "  public method Run(f: () -> int): int "
        "    var apply := lambda(g: () -> int): int => C.Inner(g); "
        "    return apply(f); "
        "  end Run; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// #1432 / §28.14 v0.177 ACCEPT: storing the lambda's own parameter into a
// PURELY LOCAL collection (never itself stored beyond the call) is a local
// binding — clean (mirrors call_bounded_2814.store_into_local_collection_clean).
BEZEL_TEST(lambda_own_param_call_bounded_1432,
          store_into_local_collection_accepted) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public method Run(f: () -> int): int "
        "    var apply := lambda(g: () -> int): int => do "
        "      var xs: Array<() -> int> := []; "
        "      xs.Add(g); "
        "      return xs[0](); "
        "    end; "
        "    return apply(f); "
        "  end Run; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ===========================================================================
// Push/pop isolation: a lambda's own parameter classification must not leak
// ===========================================================================

// #1432 regression guard: TWO SIBLING lambdas in the same method each declare
// their OWN plain function-typed parameter of the same name. The first stores
// its parameter into a field (rejected); the second only invokes its own
// (same-named) parameter, which must stay clean — proves
// `seedLambdaCallBoundedParams`'s push is popped after each lambda's body
// walk rather than leaking into a sibling lambda.
BEZEL_TEST(lambda_own_param_call_bounded_1432, sibling_lambdas_do_not_leak) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  private _f: (() -> ())?; "
        "  public method Run() "
        "    var bad := lambda(cb: () -> ()) => do "
        "      _f := cb; "
        "    end; "
        "    var good := lambda(cb: () -> ()) => do "
        "      cb(); "
        "    end; "
        "    bad(lambda() => ()); "
        "    good(lambda() => ()); "
        "  end Run; "
        "  destructor end; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(
        r, "parameter `cb` is call-bounded; declare it `scoped` to store it"));
    // Exactly ONE call-bounded diagnostic — the clean sibling must not also
    // be flagged (would indicate the push leaked across lambdas).
    int callBoundedDiagCount = 0;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("call-bounded") != std::string::npos)
            ++callBoundedDiagCount;
    BEZEL_EXPECT_EQ(callBoundedDiagCount, 1);
}

// #1432 regression guard: a NESTED lambda (a lambda literal inside another
// lambda's body) also gets its own parameter verified — the seeding is not
// limited to the outermost lambda encountered by the walk.
BEZEL_TEST(lambda_own_param_call_bounded_1432, nested_lambda_own_param_checked) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  private _f: (() -> ())?; "
        "  public method Run() "
        "    var outer := lambda() => do "
        "      var inner := lambda(cb: () -> ()) => do "
        "        _f := cb; "
        "      end; "
        "      inner(lambda() => ()); "
        "    end; "
        "    outer(); "
        "  end Run; "
        "  destructor end; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(
        r, "parameter `cb` is call-bounded; declare it `scoped` to store it"));
}

// #1432 no-regression control: a method with NO function-typed formal of its
// own, whose body's only function-typed parameter belongs to a nested lambda,
// still runs the callee-side walk (guards the `callBoundedScan` emptiness
// gate removed by this fix — the walk must not bail out just because the
// ENCLOSING method's own `callBoundedParams_` seed was empty).
BEZEL_TEST(lambda_own_param_call_bounded_1432,
          enclosing_method_with_no_function_param_still_checks_lambda) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  private _f: (() -> ())?; "
        "  public method Make() "
        "    var sink := lambda(cb: () -> ()) => do "
        "      _f := cb; "
        "    end; "
        "    sink(lambda() => ()); "
        "  end Make; "
        "  destructor end; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagContains(
        r, "parameter `cb` is call-bounded; declare it `scoped` to store it"));
}
