// fix #4323 / spec §5.1 *Implicit conversions* / §11.2 *Interface
// declaration* (v0.298). This is the SEVENTH position `fix #4310` missed —
// discovered while sweeping for a "seventh site" during that fix's own
// review, then measured and fixed here.
//
// ROOT CAUSE (traced, ExprTypecheck_Exprs.cpp ~L9274-9304, the `lam->exprBody`
// handling block inside the `LambdaExpr` visitor). `#4310` found that every
// site typing a COLLECTION LITERAL against a known target pre-sets the
// literal's own `exprType` to the target type (`ctx_.setExprType`) before the
// general `checkAssignable(ctx_.exprType(*literal), target, …)` call that
// follows, so that call trivially compares the target type to itself and
// never descends into the literal's own element expressions — a class that
// never lists the target interface in its inheritance clause is silently
// accepted, and interface dispatch on it reads a null itable slot (the
// SIGSEGV #4302/#4310 both measured). `#4310` fixed this at six sites
// (LocalDecl, LocalConstDecl, FieldDecl, PropertyDecl auto-initializer,
// ReturnStmt, general call/constructor arguments) by factoring the per-
// element walk into `ExprTC::checkCollectionLiteralElementAssignability` and
// calling it from each. A SEVENTH site has the identical `setExprType`-then-
// skip shape: an expression-bodied lambda whose collection-literal body is
// typed via an outer, INFERRED (not written) function-type target
// (`pendingLambdaTargetReturn_` / `lamTargetRet`) — this site pre-sets
// `exprType` via the same `admitted` gate and never called the new helper.
//
// THE FIX calls the SAME shared helper `#4310` factored — this is its
// seventh caller, not an eighth hand-rolled element walk (CLAUDE.md /
// LANE_RULES §6/§25: "is there a fact the compiler ALREADY COMPUTES that
// decides this?"). The only wrinkle versus `#4310`'s six sites: those all
// visit their initializer/argument expression BEFORE running the
// admissibility gate + element check (so the elements are already typed
// when the helper reads their `exprType`); here `ctx_.setExprType` on the
// literal happens BEFORE `visitExpr(*lam->exprBody)` (visiting the literal
// is what types its own elements). So the new call to
// `checkCollectionLiteralElementAssignability` is placed AFTER
// `visitExpr(*lam->exprBody)`, not immediately after the `setExprType` — the
// elements must be typed first.
//
// CENSUS (this fix's own investigation swept the six further positions
// `#4310`'s review had time-boxed out as unexplored — pattern-arm binding,
// default parameter value, `with`-expression, indexer setter, `yield`, cast
// target):
//   - `with`-expression: the SAME setExprType-then-skip shape, SIGSEGVs
//     identically — filed as `#4330` (an eighth #4310-family position, not
//     fixed here; out of scope for this lane's #4323 brief).
//   - indexer setter (`h[i] := [...]`): the SAME `AssignmentStmt` code path
//     `#4316` already covers (`toTy = ctx_.exprType(*as->target)` is generic
//     over any lvalue shape) — noted on `#4316`, not filed separately.
//   - default parameter value, `yield return`, a `case`/`when`-expression
//     arm, and the `as` operator: NONE of these thread a target to the
//     literal AT ALL (no `setExprType`, no admissibility gate) — each hits a
//     DIFFERENT pre-existing backstop (a silent-wrong-value #2491/#2434
//     degrade, or an IR-verifier ICE for the `case`-arm form) rather than
//     reaching the interface-conformance question this fix and #4310
//     answer. Bundled and filed as `#4332` (not fixed here).
//   - cast via the `T(x)` call-conversion form: not a live position for a
//     collection literal at all — `Array<IHolder>([...])` is rejected
//     outright with an unrelated, on-point §9.5 diagnostic steering to
//     `new Array(...)`.
//
// SCOPE, MEASURED AND FILED SEPARATELY (not fixed here):
//   - #4330: the `with`-expression eighth position (SIGSEGV, same severity).
//   - #4332: the four positions with no target-threading at all.
//   - #4331: an UNRELATED, PRE-EXISTING codegen SIGSEGV — an inline array of
//     an interface element materialised via a lambda call crashes even for a
//     genuinely CONFORMING CLASS element (confirmed pre-existing: reproduces
//     identically with this fix's own change reverted). This is why every
//     ACCEPT-side control below uses a heap `Array<T>` target, not an inline
//     array, for the lambda-body literal — an inline-array accept-side
//     control would crash on #4331, not on anything this fix touches. The
//     REJECT rows below use the inline-array form (matching #4323's own
//     filed repro) because rejection happens in sema, before #4331's
//     codegen path is ever reached.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// Same diagnostic text `#4310`'s six sites already produce (this fix reuses
// the exact same `checkAssignable` call via the shared helper, not a new
// diagnostic) — §5.1's "not implicitly convertible" wording.
bool hasNotImplicitlyConvertible(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("not implicitly convertible") != std::string::npos &&
            d.message.find("5.1") != std::string::npos)
            return true;
    return false;
}

// Shared prelude: `IHolder` (one method), a NON-conforming `Widget3` (never
// lists `IHolder`), two DIFFERENT conforming classes (`Widget3Ok`/
// `Widget4Ok`), and a conforming record (`RecHolderOk`, §9.6 — a record may
// implement an interface that declares no settable member).
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
    "end Widget3Ok; "
    "class Widget4Ok: IHolder "
    "  public V: string; "
    "  public constructor(v: string) V := v; end constructor; "
    "  public method GetV(): string return V; end GetV; "
    "end Widget4Ok; "
    "record RecHolderOk(V: string): IHolder "
    "  public method GetV(): string => V; "
    "end RecHolderOk; ";

JitResult run(const std::string& mainBody) {
    return runAot(kPrelude +
                  "class C public static method Main(): int " +
                  mainBody + " end Main; end C;");
}

}  // namespace

// ============================================================
// REJECT ROWS — the fix. Each fails without it (compiles clean, then
// SIGSEGVs at the marked access — matches #4323's own filed repro exactly)
// and rejects with it. The pre-fix behaviour is a CRASH, not merely a
// silent accept, so per LANE_RULES §18 each row pairs the location-based
// helper with an explicit message-text assertion — `failedAtCompileTime-
// WithRealLocation` alone would not distinguish "rejected for the right
// reason" from some other crash/backstop reaching a real location.
// ============================================================

// #4323's own filed repro, verbatim: `Widget3` never lists `IHolder`, the
// lambda's expression body is `[new Widget3(s)]`, the outer `var f: () ->
// [1]IHolder := lambda() => ...` supplies the inferred target.
BEZEL_TEST(lambda_body_collection_literal_element_interface_conformance_4323,
           inline_array_lambda_target_noconform_rejected_spec_5_1_11_2) {
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

// The heap `Array<IHolder>` twin of the row above — the SAME lambda-target
// site's admissibility gate also admits the §15.4 builtin-collection form,
// and the shared helper's builtin-collection branch (not just its
// InlineArray branch) must be reached through this new call site too.
BEZEL_TEST(lambda_body_collection_literal_element_interface_conformance_4323,
           heap_array_lambda_target_noconform_rejected_spec_5_1_11_2) {
    auto r = run(
        "var s := \"he\" + \"llo\"; "
        "var f: () -> Array<IHolder> := lambda() => [new Widget3(s)]; "
        "var outer := f(); "
        "return outer[0].GetV().Length;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNotImplicitlyConvertible(r));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ============================================================
// ACCEPT-SIDE CONTROLS (§15's discipline — a reject fix is only as good as
// its accept-side sweep). All use a heap `Array<T>` lambda target — see the
// file-header note on #4331, a pre-existing, UNRELATED codegen SIGSEGV for
// an inline array of an interface element materialised via a lambda call,
// even for a genuinely conforming CLASS element.
// ============================================================

// A single genuinely conforming class element through the lambda-target
// path.
BEZEL_TEST(lambda_body_collection_literal_element_interface_conformance_4323,
           conforming_single_element_accepted_control) {
    auto r = run(
        "var s := \"he\" + \"llo\"; "
        "var f: () -> Array<IHolder> := lambda() => [new Widget3Ok(s)]; "
        "var outer := f(); "
        "return outer[0].GetV().Length;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// Two DIFFERENT conforming implementors in one lambda-target literal.
BEZEL_TEST(lambda_body_collection_literal_element_interface_conformance_4323,
           two_different_conforming_implementors_accepted_control) {
    auto r = run(
        "var f: () -> Array<IHolder> := lambda() => "
        "  [new Widget3Ok(\"ab\"), new Widget4Ok(\"cde\")]; "
        "var outer := f(); "
        "return outer[0].GetV().Length + outer[1].GetV().Length;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// A nullable interface element defaulted to null (no initializer) through
// the lambda-target path — must stay accepted; the element check only runs
// against an actual collection-LITERAL's elements.
BEZEL_TEST(lambda_body_collection_literal_element_interface_conformance_4323,
           nullable_interface_element_defaulted_null_accepted_control) {
    auto r = run(
        "var f: () -> Array<IHolder?> := lambda() => "
        "  [new Widget3Ok(\"ab\"), null]; "
        "var outer := f(); "
        "return (if outer[1] = null then 1 else 0) + outer[0]!.GetV().Length;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}

// A class element and a RECORD element, both genuinely conforming, mixed in
// the SAME heap `Array<IHolder>` lambda-target literal (§9.6 permits a
// record to implement an interface with no settable member).
BEZEL_TEST(lambda_body_collection_literal_element_interface_conformance_4323,
           class_and_record_element_mixed_accepted_control) {
    auto r = run(
        "var f: () -> Array<IHolder> := lambda() => "
        "  [new Widget3Ok(\"ab\"), new RecHolderOk(\"cde\")]; "
        "var outer := f(); "
        "return outer[0].GetV().Length + outer[1].GetV().Length;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}
