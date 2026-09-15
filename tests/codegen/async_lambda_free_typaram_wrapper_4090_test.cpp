// fix #4090 (§30.6 "Which wrapper, and from where", v0.296): "Otherwise —
// no target, or a target return slot that still contains a free type
// parameter — the wrapper follows from the body alone ... `async
// lambda(req: HttpRequest) => Http.Text("ok")`, whose body is a fresh owned
// class result, infers `(HttpRequest) -> OwningTask<HttpResponse>`, which
// reaches no instantiation of `Task<T>` (`Task<T>` and `OwningTask<T>` are
// siblings under `Task`) — that candidate is inapplicable."
//
// The WRAPPER an async lambda infers at a free `Task<R>` slot already
// matched the ruling before this fix — the compiler never threads a target
// return type to a lambda argument of a GENERIC callee at all
// (src/sema/ExprTypecheck_Exprs.cpp gates that seeding on
// `calleeMd2->genericParams.empty()`), so an async lambda argument to a
// generic method is unconditionally on the "body alone" rule by construction
// regardless of this ruling. What was MISSING is the sentence right after
// it: that the inferred `OwningTask<C>` must then fail to unify against a
// declared `Task<R>` slot and make the candidate inapplicable. Before this
// fix, `unifyArgSubst` had no notion of function types at all, so `R` was
// simply never examined — and separately, `collectBoundParamNames` (the
// heuristic gating whether a target-typed completion attempt or "cannot
// infer" report is even considered) treats any syntactic mention of `R`
// inside a function-typed parameter as "argument-bound", so nothing else
// ever asked whether the argument's actual shape supported that. The call
// proceeded with `R` left as the raw, unsubstituted type parameter — a
// SILENT WRONG VALUE with no diagnostic at all (measured below, pre-fix:
// the program ran to completion and returned 77, matching neither the
// null check nor either string-equality check the test body poses).
//
// This shares its fix with #4089 (same `unifyArgSubst` Function arm, same
// head-name-checked constructed-generic-type branch — needed so `Task<R>`
// does not wrongly unify against `OwningTask<Widget>` merely because both
// are single-generic-argument Named types — and the same "still unbound
// after argument-driven unification" report).
//
// FAIL-WITHOUT / PASS-WITH: 1 of the 3 rows in this file fails without the
// fix and passes with it (measured via a revert-src + rebuild + re-run
// cycle): `owned_class_body_task_r_slot_reports_cleanly` (silent wrong
// value, exit 77, zero diagnostics, without; a clean compile-time rejection
// citing "cannot infer the type argument", with). The other two are NOT
// fail-without rows, and are labelled as such in their own comments:
// `value_body_task_r_slot_still_applies` is a regression control (codegen
// independently re-derives the call's monomorphization at the lowering
// site — see #4089's file header — so this exact program already produced
// the correct runtime VALUE on both sides of the fix);
// `closed_target_slot_owned_class_body_already_conforms` pins pre-existing,
// unrelated behaviour (a REFUTED sub-finding — see its own comment).

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

// §30.6 / §14.5 / §22.6: an async lambda whose body is a fresh owned class
// production, passed to a generic method whose only function-typed
// parameter declares a free `Task<R>` return slot. Per source (3) the
// lambda's own inferred type is `(A) -> OwningTask<Widget>`; per the ruling
// this must fail to unify against `Task<R>` and make the (sole) candidate
// inapplicable — a clean compile error, not a silently wrong program.
BEZEL_TEST(async_lambda_free_typaram_wrapper_4090,
           owned_class_body_task_r_slot_reports_cleanly) {
    auto r = runAot(
        "module M; "
        "class Widget "
        "  public property Name: string; "
        "  public constructor(name: string) this.Name := name; end constructor; "
        "end Widget; "
        "class Router "
        "  public static async method Get<A, R>(route: A, h: (A) -> Task<R>): Task<R> "
        "    return await h(route); "
        "  end Get; "
        "end Router; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    var w := await Router.Get(\"x\", async lambda(route: string) => new Widget(route)); "
        "    if w = null then return 55; end; "
        "    if w.Name = \"x\" then return 0; end; "
        "    if w.Name = \"\" then return 66; end; "
        "    return 77; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "cannot infer the type argument"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xc2\xa7""30.6") ||
                       diagnosticMessageContains(r, "\xc2\xa7""14.5"));
}

// REGRESSION CONTROL (not a fail-without proof — see file header): a
// VALUE-typed (not fresh-owned-class) async lambda body at the same free
// `Task<R>` slot. Source (3)'s wrapper choice is `Task<int>`, which DOES
// unify against the declared `Task<R>` slot (`R := int`), so this call must
// keep compiling and running correctly.
BEZEL_TEST(async_lambda_free_typaram_wrapper_4090,
           value_body_task_r_slot_still_applies) {
    auto r = runAot(
        "module M; "
        "class Router "
        "  public static async method Get<A, R>(route: A, h: (A) -> Task<R>): Task<R> "
        "    return await h(route); "
        "  end Get; "
        "end Router; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    var v := await Router.Get(21, async lambda(n: int) => n * 2); "
        "    return v; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// TEST #4090 — already-conformant, unrelated to this fix (a separate
// "source (2), closed target slot" probe pinned here as a REFUTED finding
// rather than left unrecorded). §30.6 source (2): a CLOSED target return
// slot (no free type parameter — a plain typed-local `() -> Task<Widget>`,
// not a generic-callee argument position) names the wrapper, and admits a
// fresh-owned-class body under EITHER `Task<C>` (Pattern A) or
// `OwningTask<C>` (Pattern B). #4148 (the implementation tracker this
// ruling opened) flagged this specific shape as "Probe" — unverified
// whether the compiled program actually conforms to Pattern A's disposal
// contract. Measured on HEAD (pre- and post- this fix, unaffected by it):
// 200 iterations of "bind the outer task via `using var`, drop it without
// ever awaiting it" reclaim the fresh Widget every time (`Widget.n == 0`)
// — Pattern A's own destructor-disposes-the-result contract, not Pattern
// B's transfer-to-the-awaiter one (which would leak on every never-awaited
// iteration). The `() -> Task<Widget>` target is never actually read by
// codegen's Task-vs-OwningTask wrapper decision (src/sema/
// ExprTypecheck_Exprs.cpp's `ownedResult` test looks only at the body's own
// inferred type, ignoring `lamTargetRet`); this passes anyway because
// `emitAsyncLambda`'s own disposer-decision machinery keys off the
// CALL-SITE's declared task type, not the lambda literal's own structural
// type — a different, independent mechanism from the one #4089/#4090's
// core fix touches. Loop-amplified per LANE_RULES (a one-shot leak row can
// pass on both sides).
BEZEL_TEST(async_lambda_free_typaram_wrapper_4090,
           closed_target_slot_owned_class_body_already_conforms) {
    constexpr int kIters = 200;
    std::string program =
        "module M; "
        "class Widget "
        "  public static n: int := 0; "
        "  public constructor() Widget.n := Widget.n + 1; end constructor; "
        "  destructor Widget.n := Widget.n - 1; end destructor; "
        "end Widget; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    for var i in 0..<" + std::to_string(kIters) + " do "
        "      do "
        "        var g: () -> Task<Widget> := async lambda() => new Widget(); "
        "        using var t := g(); "
        "      end; "
        "    end; "
        "    return Widget.n; "
        "  end Main; "
        "end C;";
    auto r = runAot(program);
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 0);  // Pattern A: reclaimed, none leaked
}
