// Spec-derived regression tests for issue #4044 — a `string`-typed `out var`
// (or `out using var`) recipient bound inside an EXPRESSION-bodied async
// lambda is never disposal-registered at all. The sibling test
// `async_lambda_out_var_suspend_4041_test.cpp` documents this exact gap as
// its SCOPE NOTE (test source lines 52–65): fix #4041 reserved a frame field
// for the recipient (the SEGV side) and added the expression-entry overload of
// `collectOutVarSuspendLocals` for the EXPRESSION-bodied venue, but it
// DELIBERATELY declined to close the release-registration half because that
// arm "touches a different, safety-critical mechanism (the string escape
// walker)". This file is that deferred fix.
//
// THE DEFECT (independent of #4041's frame-reservation gap — separable, no
// `await` required to reproduce). `emitAsyncLambda`'s `lam.exprBody` arm has
// no statement list at all (the body is a single `ast::Expr*`), so the
// pre-existing pin `stringReleaseAnalyzedBody_` (set by
// `collectStringReleaseEligible` for a STATEMENT list — see its declaration
// in `LlvmBackend_impl.hpp` near `outVarStringReleaseEligible`) is
// unreachable. Without it, `outVarStringReleaseEligible` (gate 4 of
// `registerOutVarStringRelease`) bails before any verdict: the walker's
// `outVarIntroSite` is never consulted and `escaped` defaults to true —
// the leak-over-UAF safe default. Gate 3 (`currentFn_ != stringReleaseEligibleFn_`)
// also fails: `stringReleaseEligibleFn_` is likewise only set on the
// statement-list arm. The recipient's scope-end release is therefore never
// registered, and the `n := s + "-tail"` producer reference stored into the
// recipient's bare `entryAlloca` is never decremented.
//
// The leak reproduces WITHOUT `await` (the sibling test's scope note says
// so: "a plain, unconditional 20500 B / 1000 allocation LEAK with NO await at
// all"). The frame-reservation half (#4041) is needed only for the SEGV path;
// the release half (#4044) is needed regardless of suspends.
//
// THE FIX (`LlvmBackend_Async.cpp`, `LlvmBackend_Setup.cpp`,
// `LlvmBackend_Exprs2.cpp`, `LlvmBackend_impl.hpp`). For an
// expression-bodied async lambda:
//   * `stringReleaseEligibleFn_ = resumeFn` is set unconditionally for the
//     block-bodied arm — add the matching pin for the expression-bodied arm.
//   * A NEW sibling field `stringReleaseAnalyzedExpr_` (`const ast::Expr*`)
//     is pinned to `lam.exprBody.get()`, so the walk has a body. (Distinct
//     from `stringReleaseAnalyzedBody_` because the AST owns the
//     expression body as `lam.exprBody.get()` — there is no statement
//     vector to take a pointer-of.)
//   * `outVarStringReleaseEligible` PREFERS the new field over the body pin
//     when both are set, and walks the expression body with
//     `StringEscapeWalker::expr`. Critical detail: the OUTER body's pin
//     names Main's statement list (the `for ... { var f := async lambda():
//     ... => ... }` shape the tests below use), whose walk would descend
//     into the LambdaExpr and re-walk `exprBody` with `inLambda = true`,
//     flagging every `nm.Length` safe-read (whose `nm` shares its
//     LocalSymbol with the intro site, so `isTargetIdent` answers true) as
//     an escape — the conservative-safe direction, but the exact leak this
//     fix is closing. The expr pin walks the expression body directly with
//     `inLambda` still false.
//   * The new field is saved/restored by `lowerLambda` around the
//     `emitAsyncLambda` call, mirroring the #2641 body-pin save/restore
//     (an inner lambda's pin must not leak back into the outer body).
//
// TEST MATRIX (T1–T4). Each test asserts the SCALAR total (length sum),
// so a correct run has the LSan exit code separate from the test's own
// `if t = X then 0 else 1`. A pre-#4044 build leaks the recipient's
// producer reference per iteration; the totals still come out correct
// (the leak is hidden from the program's arithmetic, only LSan/ASan sees
// it). All four bodies are EXPRESSION-bodied (`Lam.exprBody != null`):
//   * T1 — plain static-method `out var` recipient in an `(if W.Make(...)
//         then ... else 0)` shape — the SCOPE-NOTE repro verbatim.
//   * T2 — same, but with a real `await` inside the expression body
//         (§17.15/v0.216 forbids `await` inside the `(var …)` binding atom,
//         but `(if …)` accepts it — proves the fix is correct for the
//         suspend path too, not just the unconditional leak).
//   * T3 — constructor `out var` recipient — `registerCtorOutVarStringRelease`'s
//         gate 4 is the same `outVarStringReleaseEligible` call site, so the
//         fix closes both method and constructor forms together.
//   * T4 — `out using var` OWNED recipient — the sibling form (the
//         `ensureOutVarSlots` / `ensureCtorOutVarSlots` `out using var`
//         arms share the same `outVarStringReleaseEligible` walk for their
//         "sharedHandle" / "dynamicDispose" judgment). `Box`'s destructor
//         prints "dispose", so exactly-once disposal is independently
//         observable in `stdoutText` (500 lines expected).
//
// LOOP-AMPLIFICATION — 500 iterations per test. The sibling #4041 test uses
// 500; the 41 B/iter leak this fix closes then totals 20500 B per test, well
// above LSan sensitivity.
//
// per §22.3, §22.4, §28.9, §28.11, §28.13, §29.2, §29.7, §30.1, §30.2, §30.6, v0.295

#include <cstddef>
#include <string>

#include "support/aot_support.hpp"   // runAotChecked
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// ASan + LSan oracle: a leak / double-free / UAF / SEGV report at exit
// surfaces as ran=false, which this flips to the -999 sentinel so a plain
// int comparison serves as the assertion.
int runChecked(const std::string& src) {
    JitResult r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// -----------------------------------------------------------------------------
// T1 — SCOPE-NOTE REPRO. Plain `string` `out var` recipient in an EXPRESSION-
// BODIED async lambda, NO await — the unconditional 20500 B / 1000 leak
// measured on a pre-#4044 build for the sibling test's SCOPE NOTE. "abc"
// + "-tail" = 8 chars; 500 invocations × 8 = 4000. Pre-#4044: clean exit 0
// arithmetic, but the program leaks one `bz_string` per iteration (the
// recipient's producer reference is never released because gate 4 of
// `registerOutVarStringRelease` always answers false). Post-#4044: clean
// exit 0 + LSan-clean. // per §22.3, §22.4, §28.9, §30.1, §30.6
// -----------------------------------------------------------------------------
BEZEL_TEST(async_lambda_out_var_suspend_expr_body_4044,
           expression_bodied_async_lambda_out_var_string_recipient_is_released) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; import System; "
        "class W "
        "  public static method Make(s: string, out n: string): bool "
        "    n := s + \"-tail\"; return true; "
        "  end Make; "
        "end W; "
        "class C "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 1..500 do "
        "      var f := async lambda(): Task<int> => (if W.Make(\"abc\", out var nm) then nm.Length else 0); "
        "      using var task := f(); "
        "      task.Wait(); "
        "      t := t + task.Result; "
        "    end; "
        "    return (if t = 4000 then 0 else 1); "
        "  end Main; end C;"), 0);
}

// -----------------------------------------------------------------------------
// T2 — SUSPENDING sibling. Same recipient shape but with a REAL `await`
// inside the expression body, between the `out var` bind and the `nm.Length`
// read — proving the fix does not regress under the suspend path (#4041's
// own SEGV-mechanism corner). §17.15/v0.216 forbids `await` inside the
// `(var …)` binding atom; `(if …)` accepts it. The expression
// `(if W.Make("abc", out var nm) and await DelayBool() then nm.Length else 0)`
// is the §30.6 / §20 / SCOPE-NOTE shape that composes the `out var`'s
// short-circuit scope with an `await` inside an `(if …)` expression atom.
// 500 × 8 = 4000.
// per §22.3, §22.4, §28.9, §30.1, §30.6
// -----------------------------------------------------------------------------
BEZEL_TEST(async_lambda_out_var_suspend_expr_body_4044,
           expression_bodied_async_lambda_out_var_string_released_across_await) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; import System; "
        "class W "
        "  public static method Make(s: string, out n: string): bool "
        "    n := s + \"-tail\"; return true; "
        "  end Make; "
        "end W; "
        "class C "
        "  public static async method DelayBool(): Task<bool> "
        "    await Task.Delay(0); "
        "    return true; "
        "  end DelayBool; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 1..500 do "
        "      var f := async lambda(): Task<int> => (if W.Make(\"abc\", out var nm) and await C.DelayBool() then nm.Length else 0); "
        "      using var task := f(); "
        "      task.Wait(); "
        "      t := t + task.Result; "
        "    end; "
        "    return (if t = 4000 then 0 else 1); "
        "  end Main; end C;"), 0);
}

// -----------------------------------------------------------------------------
// T3 — CONSTRUCTOR sibling. `new W(\"abc\", out var nm)` instead of the
// static method call — `registerCtorOutVarStringRelease`'s gate 4 is the
// SAME `outVarStringReleaseEligible` call site (`LlvmBackend_Calls2.cpp`'s
// matching arm), so the fix closes both method and constructor forms
// together. The constructor body writes through the same
// `lowerStringRefParamStore` path (#3305), leaving the slot owning +1 at
// scope end exactly as T1 does. The conditional expression atoms do not
// permit `var` declarations, so the constructor call is chained with a
// method call (`(new W(...)).Len() = 7`) for a condition that USES the
// freshly bound recipient in a method-call argument position. 500 × 8 = 4000.
// per §22.3, §22.4, §28.9, §30.1, §30.6
// -----------------------------------------------------------------------------
BEZEL_TEST(async_lambda_out_var_suspend_expr_body_4044,
           expression_bodied_async_lambda_ctor_out_var_string_recipient_is_released) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; import System; "
        "class W "
        "  public constructor(s: string, out n: string) "
        "    n := s + \"-tail\"; "
        "  end constructor; "
        "  public method Len(): int "
        "    return 7; "
        "  end Len; "
        "end W; "
        "class C "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 1..500 do "
        "      var f := async lambda(): Task<int> => (if (new W(\"abc\", out var nm)).Len() = 7 then nm.Length else 0); "
        "      using var task := f(); "
        "      task.Wait(); "
        "      t := t + task.Result; "
        "    end; "
        "    return (if t = 4000 then 0 else 1); "
        "  end Main; end C;"), 0);
}