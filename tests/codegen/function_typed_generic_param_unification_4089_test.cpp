// fix #4089 (§14.5 phase 1 / §22.6 *Parameter count at a function-typed
// position*, v0.296): "a function-typed parameter unifies component-wise —
// each parameter slot against the argument's slot in order, the return
// slot against the return slot — and only against a function type of the
// SAME parameter count: an argument whose function type has a different
// number of parameter slots reaches no unification, and the candidate is
// inapplicable — an inference failure that drops it out ... never an error
// reported at that candidate."
//
// Before this fix, `unifyArgSubst` (src/sema/ExprTypecheck_Calls.cpp) had
// NO case at all for `ast::TypeKind::Function`: a function-typed parameter
// contributed ZERO bindings, silently, regardless of whether the argument's
// arity matched. For a generic method whose only mention of a type
// parameter is inside a function-typed parameter's slot — the shape 0002's
// registration family and stdlib §6.8's `Domain.Spawn` both key their
// overload sets on — that type parameter was left as the raw, unsubstituted
// symbol all the way through to the call's result type, and nothing ever
// reported the gap: `collectBoundParamNames` (consulted by
// inferReturnTargetTypeArgs/reportNoTargetInferenceIfNeeded) treats ANY
// syntactic mention inside a function-typed parameter as "argument-bound",
// so the "needs a target" diagnostic never fires for it either. A member
// access chained on the call's result then resolved against the bare type
// parameter itself (measured below: "member `ToString` on `R`").
//
// This fix adds the missing Function arm (component-wise, count-gated) plus
// a head-name check on the pre-existing constructed-generic-type branch
// (needed so `Task<R>` does not wrongly unify against an `OwningTask<C>`
// argument merely because both are single-generic-argument Named types —
// see the sibling #4090 test), and a new check that reports cleanly when a
// type parameter mentioned inside a function-typed argument position is
// still unbound after the (now real) unification attempt, instead of
// silently letting the raw type parameter ride through as part of the
// call's result type.
//
// FAIL-WITHOUT / PASS-WITH: 2 of the 3 rows in this file fail without the
// fix and pass with it (measured via a revert-src + rebuild + re-run
// cycle): `lambda_arg_binds_return_typeparam_static_type_known` (a
// compile-time rejection citing the bare `R` symbol, without; the chained
// member access resolves and the program runs to the expected exit code,
// with) and `lambda_arg_arity_mismatch_reported_as_inference_failure`
// (both sides reject the call — the DIAGNOSTIC TEXT is the pin: a generic
// "not implicitly convertible" complaint, without; the precise "cannot
// infer the type argument" wording this fix adds, with).
// `lambda_arg_binds_return_typeparam_runtime_value` is NOT a fail-without
// row — see its own comment — kept as a second, independent observable.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

// §14.5 phase 1: a fresh lambda argument (bypasses checkAssignable's
// function-vs-function shape check entirely per its freshLambda carve, so
// this exercises ONLY unifyArgSubst's new Function arm) whose inferred type
// `(int) -> int` unifies against the declared `(A) -> R` slot, binding
// `R := int`. Before the fix, sema never attempted this: `r`'s static type
// stayed the bare `R`, so `r.ToString()` was rejected as a member access on
// an unimplemented-for-this-receiver universal-Object member (backstop
// #2457) rather than resolving `int.ToString()`. `"42".Length == 2`.
BEZEL_TEST(function_typed_generic_param_unification_4089,
           lambda_arg_binds_return_typeparam_static_type_known) {
    auto r = runAot(
        "module M; "
        "class Box "
        "  public static method Apply<A, R>(x: A, f: (A) -> R): R "
        "    return f(x); "
        "  end Apply; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    var r := Box.Apply(21, lambda(n: int) => n * 2); "
        "    return r.ToString().Length; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 2);  // "42".Length
}

// The same call, run to its runtime value directly. NOT a FAIL-WITHOUT
// control on its own — measured: this exact program already returns 42
// without this fix too, because codegen re-derives a generic call's
// monomorphization independently at the lowering site rather than reading
// sema's (pre-fix, incomplete) substitution. Kept as a second, independent
// observable beside the static-type probe above, which IS the row that
// fails without the fix (sema's substitution is what member-access
// resolution against the call's result actually reads).
BEZEL_TEST(function_typed_generic_param_unification_4089,
           lambda_arg_binds_return_typeparam_runtime_value) {
    auto r = runAot(
        "module M; "
        "class Box "
        "  public static method Apply<A, R>(x: A, f: (A) -> R): R "
        "    return f(x); "
        "  end Apply; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    return Box.Apply(21, lambda(n: int) => n * 2); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// §14.5 phase 1 / §22.6 *Parameter count at a function-typed position*: the
// lambda's OWN arity (1 param) differs from the declared function-typed
// slot's (2 params, `(A, B) -> R`) — the count mismatch must make R (and B)
// fail to bind through this argument, cleanly, rather than crash or wrongly
// bind. With only one candidate here the call is necessarily a compile
// error either way (there is nothing else for it to "drop out" to); this
// fix's value is the diagnostic naming the real reason (an uninferable type
// argument) instead of a generic "not implicitly convertible" complaint
// that does not mention arity or inference at all.
BEZEL_TEST(function_typed_generic_param_unification_4089,
           lambda_arg_arity_mismatch_reported_as_inference_failure) {
    auto r = runAot(
        "module M; "
        "class Box "
        "  public static method Apply<A, B, R>(x: A, f: (A, B) -> R): R "
        "    return f(x, x); "
        "  end Apply; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    var r := Box.Apply(21, lambda(n: int) => n); "
        "    return r; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "cannot infer the type argument"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xc2\xa7""14.5"));
}
