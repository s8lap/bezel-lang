// Spec-derived regression tests for issue #7092 (case 1 — the ASSIGNMENT
// spelling of §28.3's producers row for a collection literal).
//
// THE DEFECT (issue #7092, measured by the coordinator on the staging build
// 9f1d7603 and re-measured on this lane):
//
//   var a: Array<int> := new Array<int>(2);
//   a := [1,2,3];
//   var il: IList<int> := a;
//   il.Dispose();
//
// compiled with NO diagnostic and then aborted under generated-code
// AddressSanitizer with "attempting double-free on 0x…: #1 bz_array_free
// src/runtime/bezel_runtime_collections.cpp:741:5 #2 main" — the explicit
// `il.Dispose()` (the interface arm of the explicit-dispose chain) and `a`'s
// scope-end pass, on the same 8-byte region.
//
// THE SPEC SENTENCES THAT DECIDE THE OUTCOME.
//
// §28.3, "Ownership state of locals", the producers table (v0.340):
//
//   "Collection literal (`[1, 2, 3]`) targeting a reference type (`Array<T>`,
//    `OwningArray<T>`, `Set<T>`, `Dictionary<K, V>`, etc.) | **Owned**"
//
// §28.3, *Consuming positions for an owned-local read* (v0.340):
//
//   "an **unmarked** local binding or assignment RHS over a **class- or
//    interface-typed** binding — `var y := x;`, `y := x;`: a plain alias
//    binding is a borrow of `x`, source `x` itself"
//
// §28.6, "Who may call Dispose()" (v0.340):
//
//   "`Dispose()` or a direct `DisposeAsync()` call through a **borrow** is a
//    compile error. The borrow receivers: a borrowed local or parameter ..."
//
// ⇒ `a` owns the literal it was assigned, `il` is a borrow of `a`, and
// `il.Dispose()` is the §28.6 compile error. The program must not run.
//
// WHY THE OWNERSHIP PASS COULD NOT SEE IT. A collection literal is
// CONTEXT-TYPED (§15.4: "A collection literal has no standalone type. It is
// well-typed only in a context that provides the target type"), so sema
// records NO `exprType` for the literal node, and `producerState`'s
// collection-literal arm (fix #4338's) — which reads `ctx_.exprType(e)` —
// never fires. At a DECLARATION the context is the local's declared type,
// which fix #7073's `isOwnedCollectionLiteralLocal` helper reads; at an
// ASSIGNMENT the context is the LHS's declared type, and there was no such
// helper, so the §28.6 borrow-receiver check never learned that `a` owns
// the value `il` borrows.
//
// THE FIX (narrow, and deliberately NOT a state change). The local's CURRENT
// value being an owned collection literal is recorded as its OWN fact
// (`LocalOwnership::ownsCollectionLiteral`, set and cleared on every
// reassignment), and the §28.6 alias-source gate consults it beside the two
// owner predicates it already had. The local's ownership STATE is untouched:
// the §28.3 state-match rule and the must-use lint must not see a change
// from this fix (issue #7092's own note), and no second scope-end free path
// is added — codegen's free-eligibility decisions are not this lane's to
// change.
//
// ORACLES. Every REJECT row asserts the SPECIFIC diagnostic (the outcome, not
// "did not crash") and runs through `runAotChecked`, so the generated-code
// ASan/LSan tier is armed: on the pre-fix compiler the same program RUNS and
// the sanitizer catches the double free, which is not a compile-time
// rejection, so the row fails on the pre-fix code (fail-without, proved
// mechanically in the commit body).
//
// THE ACCEPT-SIDE CONTROLS use the plain `runAot` oracle, NOT
// `runAotChecked`: the assignment-path literal's SCOPE-END free is a
// SEPARATE, codegen-side defect (the literal's producers row is invisible
// to codegen's free-eligibility scan exactly as it was invisible to the
// ownership pass), so those two programs LEAK today — a known defect, filed
// as its own issue, and NOT this fix's to repair. Arming the leak oracle on
// them would fail the rows for a reason outside the fix. The rows still
// assert that the programs COMPILE and RUN (no crash, no double free).
//
// DEFERRED (issue #7092's cases 2 and 3, filed as their own issues):
//   * the CONCRETE-typed alias's leak (`var a: Array<int> := [1,2,3]; var b:
//     Array<int> := a;`) — decided by codegen's borrow-escape scan;
//   * the `using var` alias's "vacuous" warning and §28.10's missing
//     compile error for an unmarked `using var` over a named owned binding —
//     unimplemented for ALL sources, not a literal blind spot.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

bool diagContains(const JitResult& r, const char* sub) {
    return diagnosticMessageContains(r, sub);
}

}  // namespace

// =====================================================================
// REJECT arm — §28.6: a `Dispose()` through a BORROW is a compile error,
// and the borrow here is of a local that holds an assigned collection
// literal.
// =====================================================================

// The issue's own repro, verbatim: an IList-typed alias of a local that was
// ASSIGNED a collection literal. The diagnostic names the owner `a`.
BEZEL_TEST(ownership_collection_literal_assignment_7092,
           assignment_literal_alias_dispose_rejected_28_6) {
    auto r = runAotChecked(
        "module M; import System; class C public static method Main(): int "
        "  var a: Array<int> := new Array<int>(2); "
        "  a := [1,2,3]; "
        "  var il: IList<int> := a; "
        "  il.Dispose(); return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `il`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `a`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§28.6"));
}

// The CONCRETE-typed spelling: §28.6's rule is about the RECEIVER's
// ownership, not its static type.
BEZEL_TEST(ownership_collection_literal_assignment_7092,
           assignment_literal_concrete_alias_dispose_rejected_28_6) {
    auto r = runAotChecked(
        "module M; import System; class C public static method Main(): int "
        "  var a: Array<int> := new Array<int>(2); "
        "  a := [1,2,3]; "
        "  var b: Array<int> := a; "
        "  b.Dispose(); return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `b`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `a`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§28.6"));
}

// The LAMBDA spelling: the assignment happens inside a lambda body, on a
// local declared there — the lambda-body assignment arm is the sibling of
// the method-scope one, and it must record the same fact. (MEASURED at the
// base: this program compiled with NO diagnostic — the method-scope arm's
// fact was invisible to the lambda arm — so the row is a fail-without row
// too.)
BEZEL_TEST(ownership_collection_literal_assignment_7092,
           assignment_literal_alias_in_lambda_dispose_rejected_28_6) {
    auto r = runAotChecked(
        "module M; import System; class C public static method Main(): int "
        "  var f := lambda(): int => do "
        "    var a: Array<int>; "
        "    a := [1,2,3]; "
        "    var il: IList<int> := a; "
        "    il.Dispose(); "
        "    return 0; "
        "  end; "
        "  return f(); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `il`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `a`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§28.6"));
}

// =====================================================================
// ACCEPT-side controls (LANE_RULES §15) — programs that MUST keep
// compiling and running. A false-reject here would be worse than the
// defect.
// =====================================================================

// (a) The local is REBOUND to a fresh owned value after the literal
//     assignment — the recorded fact must be re-derived (cleared), and the
//     one-hop §28.6 rejection (the local is an owner again) must stand.
//     This row is rejected at the BASE too: it pins that the fix does not
//     change this verdict.
BEZEL_TEST(ownership_collection_literal_assignment_7092,
           assignment_literal_rebound_to_new_owner_dispose_rejected_28_6) {
    auto r = runAotChecked(
        "module M; import System; class C public static method Main(): int "
        "  var a: Array<int> := new Array<int>(2); "
        "  a := [1,2,3]; "
        "  a := new Array<int>(4); "
        "  var il: IList<int> := a; "
        "  il.Dispose(); return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "cannot call `Dispose()` on `il`"));
    BEZEL_EXPECT_TRUE(diagContains(r, "borrow (an alias) of `a`"));
}

// (b) The OWNER's own dispose — `a` owns the assigned literal, so
//     `a.Dispose()` is §28.6's "an owned local" receiver: legal, and the
//     program must still compile and run. (Plain `runAot`: this program's
//     scope-end free of the DISPLACED array is the separately-filed
//     codegen-side leak, so the leak oracle is not armed here — see the
//     file header.)
BEZEL_TEST(ownership_collection_literal_assignment_7092,
           assignment_literal_owner_dispose_still_runs_28_6) {
    auto r = runAot(
        "module M; import System; class C public static method Main(): int "
        "  var a: Array<int> := new Array<int>(2); "
        "  a := [1,2,3]; "
        "  a.Dispose(); return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// (c) The literal assigned and the local only READ — no alias, no dispose:
//     the assignment itself must stay admitted.
BEZEL_TEST(ownership_collection_literal_assignment_7092,
           assignment_literal_without_alias_still_runs_28_6) {
    auto r = runAot(
        "module M; import System; class C public static method Main(): int "
        "  var a: Array<int> := new Array<int>(2); "
        "  a := [1,2,3]; "
        "  var n := a.Count; "
        "  Console.WriteLine(n); return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 0);
}
