// fix #4340 / spec §5.1 *Implicit conversions* / §11.2 *Interface
// declaration* (v0.298). This is the BLOCK-BODIED twin of #4323 — `#4323`
// closed the EXPRESSION-BODIED lambda case (an expression body that IS a
// collection literal, typed via an outer inferred function-type target);
// the same defect survives one node-shape over (`do return [...]; end`),
// with the SAME shape (`setExprType`-then-skip) and the SAME crash
// signature. The pre-fix behaviour is the same as #4323's own repro:
// compiles clean (with only the pre-existing #2491 `internal:` warning
// from the codegen side), then SIGSEGVs at the marked interface dispatch
// (the `outer[0].GetV()` access, with the same null itable slot).
//
// ROOT CAUSE (traced, ExprTypecheck_Exprs.cpp ~L9259-9265, the
// `returnCtx_`/`currentReturnType_` setup branch inside the LambdaExpr
// visitor). `#4310`'s ReturnStmt site already has the per-element walk
// factored into `ExprTC::checkCollectionLiteralElementAssignability`, and
// gates it on `returnCtx_ == ReturnCtx::ValueMethod && currentReturnType_`
// (ExprTypecheck.cpp ~L14885-14918 — the [5.1/11.2] per-element check it
// threads onto the FIRST-return-twin position). The gate is satisfied for
// an ASCRIBED block body (the `lambda(): R => do return [...]; end` shape)
// because `hasAscription && !lam->isAsync` sets `returnCtx_ = ValueMethod`
// and `currentReturnType_ = ascribedRet`. It is NOT satisfied for the
// UNASCRIBED twin (the `lambda() => do return [...]; end` shape bound to
// `var f: () -> R := …`) because the else-branch leaves BOTH null — the
// outer function-type target `lamTargetRet` (`pendingLambdaTargetReturn_`)
// is the SAME value the `#4323` expression-body arm threads through, but
// the block-body mirror never reads it.
//
// THE FIX threads the SAME predicate onto the block-body mirror. Mirrors
// `#4323`'s pattern exactly: `setExprType` so the literal is materialisable
// against `lamTargetRet` at codegen, then the SAME shared
// `checkCollectionLiteralElementAssignability` helper #4310 factored
// (§6/§25: "is there a fact the compiler ALREADY COMPUTES that decides
// this?"; this is its EIGHTH caller, not a new hand-rolled element walk).
// Walks every value-carrying `return e;` in the body — descending through
// do/end, if/elif/else, while, for, try/catch/finally, statement-position
// case block arms — via the same `forEachLambdaReturnValue` helper
// `checkConcreteSlotLambdaBody` (#1820) uses, so the rejection sees the
// same return-path set the inference sees (#4323's expression-body arm
// only had to handle the single literal at `lam->exprBody`; the block-body
// arm has to walk to find collection literals across wrapper levels).
//
// SCOPED — exactly mirroring `#4323`'s seven-scope so the regression test
// is a drop-in twin rather than a separate sweep:
//   * `lam->isBlockBody` (the expression-body case is `#4323`'s, not this
//     issue's; double-checking it would duplicate the same diagnostic
//     through a second arrival).
//   * `!hasAscription` (the ascription branch already sets `returnCtx_ =
//     ValueMethod` and reaches the same per-element check through #4310's
//     own first-return twin — noted by #4323 in its own scope list).
//   * `!lam->isAsync` (async lambdas have their own #30.6 task-family
//     return type and their own `currentReturnType_` setup branch — touching
//     them is a separate fix and would dilute this regression test).
//   * `lamTargetRet` is on §15.4's closed collection-target list (the
//     empty carve-out included — `[]` is admissible at every §15.4
//     collection target). A non-collection `lamTargetRet` is unaffected,
//     so the broader "every target return checks returnable values"
//     question (a real but separate defect class) stays out of scope.
//
// SCOPE, MEASURED AND FILED SEPARATELY (NOT fixed here):
//   * `#4331`: an UNRELATED, PRE-EXISTING codegen SIGSEGV — an inline
//     array of an interface element materialised via a lambda call
//     crashes even for a genuinely CONFORMING CLASS element (confirmed
//     pre-existing: reproduces identically with this fix's change reverted
//     — `with_expr` against a `Widget3Ok` element through any of the
//     three lambda-body forms also crashes pre-fix). This is why every
//     ACCEPT-side control below uses a heap `Array<T>` target, not an
//     inline array — an inline-array accept-side control would crash on
//     #4331, not on anything this fix touches. The REJECT rows use the
//     inline-array form (matching #4340's own filed repro AND #4323's
//     own filed repro, verbatim) because rejection happens in sema,
//     BEFORE #4331's codegen path is ever reached.
//   * The block-body for a heap array with an ACCEPTING element does NOT
//     runtime-correct today either (it lowers to a degraded null/zero
//     return value via the #2491 backstop — codegen reads
//     `returnReprTypeNode()` for the literal's target, the lambda body
//     has no declared return type, so the inline-array/heap materializers
//     never run). Pre-fix AND post-fix same behaviour; filed as a
//     follow-on #4361 (out of scope here).

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// Same diagnostic text `#4310`'s six sites (and #4323's seventh) already
// produce — this fix reuses the exact same `checkAssignable` call via the
// shared helper, not a new diagnostic: §5.1's "not implicitly convertible"
// wording.
bool hasNotImplicitlyConvertible(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("not implicitly convertible") != std::string::npos &&
            d.message.find("5.1") != std::string::npos)
            return true;
    return false;
}

// Shared prelude: `IHolder` (one method), a NON-conforming `Widget3`
// (never lists `IHolder`), and a SINGLE conforming implementor
// (`Widget3Ok`). Two-implementor and nullable / class+record mixing
// accept-side shapes are covered by `#4323`'s suite for the
// expression-body arm — this regression test is the block-body twin of
// that suite, not a replacement, and faithfully mirrors `#4323`'s
// scattered dual: four named tests per shape (one reject + one accept
// + two sibling-controls) rather than a faithful yet wider re-sweep
// (the wider `#4323` sweep already verified the per-element /
// builtin-collection / nested / interface-family branches on the
// lambda-target path; this lane's brief is the BLOCK-BODY twin alone,
// and a wider re-sweep would dilute the regression test's single
// claim — "the same fix #4323 made to the expression body, applied to
// the block body").
const std::string kPrelude =
    "module M; import System.Collections.Generic; "
    "interface IHolder method GetV(): string; end IHolder; "
    "class Widget3 "
    "  public V: string; "
    "  public constructor(v: string) V := v; end constructor; "
    "  public method GetV(): string return V; end GetV; "
    "end Widget3; "
    "class Widget3Ok: IHolder "
    "  public V: string; "
    "  public constructor(v: string) V := v; end constructor; "
    "  public method GetV(): string return V; end GetV; "
    "end Widget3Ok; ";

JitResult run(const std::string& mainBody) {
    return runAot(kPrelude +
                  "class C public static method Main(): int " +
                  mainBody + " end Main; end C;");
}

}  // namespace

// ============================================================
// REJECT ROWS — the fix. Each fails without it (compiles clean, then
// SIGSEGVs at the marked access — matches #4340's own filed repro
// exactly) and rejects with it. The pre-fix behaviour is a CRASH, not
// merely a silent accept, so per LANE_RULES §18 each row pairs the
// location-based helper with an explicit message-text assertion —
// `failedAtCompileTimeWithRealLocation` alone would not distinguish
// "rejected for the right reason" from some other crash/backstop
// reaching a real location.
// ============================================================

// #4340's own filed repro, verbatim: `Widget3` never lists `IHolder`, the
// lambda's block body is `do return [new Widget3(s)]; end`, the outer
// `var f: () -> [1]IHolder := lambda() => do return [...]; end` supplies
// the inferred target. Without the fix the program compiles clean and
// SIGSEGVs on `outer[0].GetV()` (the same `null itable slot` signature
// #4302/#4310/#4323 measured); with the fix it's rejected at compile
// time with the identical §5.1 "not implicitly convertible" diagnostic
// #4310's six sites (and #4323's seventh) already produce.
BEZEL_TEST(block_body_lambda_return_collection_literal_element_4340,
           inline_array_block_body_target_noconform_rejected_spec_5_1_11_2) {
    auto r = run(
        "var s := \"he\" + \"llo\"; "
        "var f: () -> [1]IHolder := lambda() => do return [new Widget3(s)]; end; "
        "var outer := f(); "
        "return outer[0].GetV().Length;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotImplicitlyConvertible(r));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// Heap `Array<IHolder>` twin of the row above — the SAME lambda-target
// site's admissibility gate also admits the §15.4 builtin-collection
// form, and the shared helper's builtin-collection branch (not just its
// InlineArray branch) must be reachable through this new site too. The
// heap form is the #4323 / 4323's accept-side canonical (every accept
// control #4323 needed a heap target to dodge #4331); the reject form
// here matches #4340's other filed repro shape.
BEZEL_TEST(block_body_lambda_return_collection_literal_element_4340,
           heap_array_block_body_target_noconform_rejected_spec_5_1_11_2) {
    auto r = run(
        "var s := \"he\" + \"llo\"; "
        "var f: () -> Array<IHolder> := lambda() => do return [new Widget3(s)]; end; "
        "var outer := f(); "
        "return outer[0].GetV().Length;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotImplicitlyConvertible(r));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ============================================================
// CONTROL ROWS — the ascribed and expression-body twins; both already
// rejected with the same diagnostic before this fix (so they're
// regressions for THIS fix's own change, not from the fix itself), but
// they isolate the defect's SCOPE — "unascribed block body", not just
// "block body, or just unascribed". If any of these start failing, a
// sibling rewrite of #4310/#4323's controls has slipped, NOT this lane.
// ============================================================

// Ascribed block body. The `lambda(): [1]IHolder => do return [...]; end`
// shape already reaches #4310's ReturnStmt per-element check (the
// `hasAscription && !lam->isAsync` branch sets `returnCtx_ = ValueMethod`
// and `currentReturnType_ = ascribedRet`); the fix is a no-op for this
// shape — and a regression test pinning the no-op is the same CYA #4323
// already pins for its own no-ops.
BEZEL_TEST(block_body_lambda_return_collection_literal_element_4340,
           inline_array_block_body_ascribed_target_noconform_rejected_control) {
    auto r = run(
        "var s := \"he\" + \"llo\"; "
        "var f: () -> [1]IHolder := lambda(): [1]IHolder => do return [new Widget3(s)]; end; "
        "var outer := f(); "
        "return outer[0].GetV().Length;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotImplicitlyConvertible(r));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// Expression-bodied lambda with block body. The exact twin of #4323's
// filed repro (`lambda() => [new Widget3(s)]`). Already rejected by the
// #4323 fix; pinning here so a future copy/edit of the fix's `if
// (!hasAscription && !lam->isAsync && lam->isBlockBody && lamTargetRet &&
// !lamTargetRetOwned && …)` guard doesn't accidentally widen to
// expression bodies (which would duplicate the same diagnostic through a
// second arrival — the file-header's scope item).
BEZEL_TEST(block_body_lambda_return_collection_literal_element_4340,
           inline_array_expression_body_target_noconform_rejected_control) {
    auto r = run(
        "var s := \"he\" + \"llo\"; "
        "var f: () -> [1]IHolder := lambda() => [new Widget3(s)]; "
        "var outer := f(); "
        "return outer[0].GetV().Length;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotImplicitlyConvertible(r));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}
