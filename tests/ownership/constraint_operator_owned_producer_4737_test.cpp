// fix #4737 — §11.8 "Dispatch through the type parameter" (spec v0.307):
// a CONSTRAINT-SOURCED `owned T` OPERATOR result is an owned producer.
//
// THE RULE. §11.8 (v0.307) states it for the section's own `Sum<T>` example
// (`tests/conformance/cases/spec_0049.bzl`), in one sentence that names both
// producer shapes together:
//
//   "At a class or interface substitution of `T`, `total` is an owned local:
//    `T.Zero()` AND EACH OPERATOR RESULT transfer per stdlib §2.19's `owned`
//    producer signatures, the loop's reassignment disposes the displaced
//    previous total by drop-on-overwrite (§28.7), and the `return` transfers
//    to the caller. At value and runtime-managed substitutions the markers
//    are erased and every step is a plain copy (§28.1, §28.9)."
//
// THE BUG. `MethodOwnership::producerState` (src/ownership/Ownership.cpp) DID
// have a BinaryExpr arm for owned-returning operators (#965, widened by
// #3189) — the issue's "no BinaryExpr case at all" measurement came from an
// `awk` window that stopped 390 lines short of the end of the function. What
// the arm lacked was the type-parameter half of its result-type gate:
//
//     if (rt->kind == Named && rt->declaration &&
//         (rt->declaration->kind == Class || Interface) &&
//         !rt->declaration->isValueBuiltin)  ->  Owned
//
// A BARE TYPE PARAMETER can never satisfy `rt->declaration` — an
// unsubstituted `T` is a Named TypeRef with `declaration == nullptr`, because
// sema type-checks a generic body ONCE, before monomorphization, so
// `ctx_.exprType` never resolves it. So `total + x` over a constraint's
// `static operator +(a: T, b: T): owned T` classified **Borrowed**, while
// `T.Zero()` on the SAME constraint interface with the SAME `owned T`
// declared return classified **Owned** — the sibling CallExpr arm consults
// `md->returnOwned` and then `isValueSubstitutionErased` (§28.1, #1817),
// which is deliberately conservative for an unsubstituted bare `T`.
//
// Measured on 921dbc45 with `build/bezelc`, one generic body, two producers:
//
//     var z := T.Zero();   ->  §28.3 auto-drop warning       (Owned)
//     var r := a + b;      ->  NOTHING                       (Borrowed)
//     const z := T.Zero(); ->  §21.1 rejected                (Owned)
//     const r := a + b;    ->  ACCEPTED                      (Borrowed)
//
// Two producers, one declaration shape, two classifications.
//
// THE FIX. The BinaryExpr arm now runs the same two lines the `md->returnOwned`
// CallExpr arm runs — `isValueSubstitutionErased(ctx_.exprType(e))` -> Borrowed,
// else Owned — which is precisely §11.8's class/interface-vs-value dichotomy
// and is the predicate the repo already settled on for this question. Codegen
// had ALREADY made the same move for operators: both of its owned-operator
// classifiers (`unwindOwnedLocalClass` LlvmBackend_Setup.cpp:21079 and
// `usingVarOwnsClassObjectDynamically` :38001) gate on
// `isClassOrInterfaceRef(initTy) || isClassOrInterfaceRef(resolveTypeParamRef(
// initTy))`. So this closes a sema/codegen divergence rather than opening one.
//
// WHY IT MATTERS BEYOND THE CLASSIFICATION. #1538 added the §28.3 state-match
// reject (`Owned` LHS requires an `Owned` RHS) and it fired on §11.8's own
// example, because `total + x` read Borrowed. #1538 was reverted for breaking
// the gate's spec-example conformance phase; #4737 is its precondition.
//
// BUILT-IN OPERATORS MUST STAY BORROWED — `a + b` over int/string/decimal
// carries no disposal obligation, and manufacturing one would put an
// obligation on every arithmetic expression in the language. The control row
// below pins that, and RUNS the program rather than only compiling it.
//
// per §11.8, §28.1, §28.3, §28.7, §21.1, v0.307, #4737

#include <cstddef>
#include <string>

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains
#include "support/aot_support.hpp"         // runAot / runAotChecked
#include "support/ownership_support.hpp"   // parseAndCheckOwnership
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// A checked run is SKIPPED (not failed) when the ASan link toolchain is
// absent — mirrors tests/ownership/static_interface_constraint_disposal_1425_test.cpp.
bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

int countOccurrences(const std::string& hay, const std::string& needle) {
    int n = 0;
    for (std::size_t p = hay.find(needle); p != std::string::npos;
         p = hay.find(needle, p + needle.size()))
        ++n;
    return n;
}

bool ownDiagContains(const bezel::test::OwnershipResult& r,
                     const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

bool ownWarnContains(const bezel::test::OwnershipResult& r,
                     const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Warning) continue;
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

// The §11.8 constraint interface and a user CLASS conformer, verbatim in
// shape from the spec's own `spec_0049.bzl` / `s11_8_0001.bzl`. `Meters`
// carries a monotonic instance id printed by its destructor so a disposal
// can be counted EXACTLY once per instance (two Meters can legitimately
// share a `Value`, so the numeric value is not an identity).
const char* kINumberAndMeters =
    "module M; "
    "public interface INumber<T> where T: INumber<T> "
    "  static method Zero(): owned T; "
    "  static operator +(a: T, b: T): owned T; "
    "end INumber; "
    "public class Meters: INumber<Meters> "
    "  public Value: int; "
    "  public Id: int; "
    "  private static _next: int := 0; "
    "  public constructor(v: int) "
    "    Value := v; Id := _next; _next := _next + 1; "
    "  end constructor; "
    "  destructor Console.WriteLine(\"D\" + Id.ToString()); end destructor; "
    "  public static method Zero(): owned Meters => new Meters(0); "
    "  public static method operator +(a: Meters, b: Meters): owned Meters => "
    "    new Meters(a.Value + b.Value); "
    "end Meters; ";

}  // namespace

// ===========================================================================
// THE BUG, pinned through §21.1. `producerState`'s verdict is not directly
// observable from a `.bzl`, but `MethodOwnership::constInitializerRejected`
// opens with `if (producerState(e) == OwnershipState::Owned) return true;`,
// so a `const` binding over the producer IS a faithful, one-bit read of it.
//
// FAIL-WITHOUT is strong here, not vacuous: on the pre-fix compiler this
// program COMPILES CLEAN and runs (exit 0) — `failedAtCompileTimeWithReal
// Location` would be false, and the specific §21.1 text is absent. There is
// no earlier check that could satisfy this oracle for another reason.
// ===========================================================================

BEZEL_TEST(constraint_operator_owned_producer_4737,
           const_binding_over_constraint_owned_operator_rejected_spec_11_8) {
    JitResult jr = runAot(
        std::string(kINumberAndMeters) +
        "static class F "
        "  public static method Add<T>(a: T, b: T): owned T where T: INumber<T> "
        "    const r := a + b; "
        "    return r; "
        "  end Add; "
        "end F; "
        "class C public static method Main(): int "
        "  using var m0 := new Meters(1); using var m1 := new Meters(2); "
        "  using var t := F.Add(m0, m1); "
        "  return t.Value; "
        "end Main; end C;");
    // The §21.1 owned-production rejection, pinned by its OWN wording — not
    // merely "it was rejected", which any unrelated earlier check would also
    // satisfy (lane-brief §18 radius 2).
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        jr, "must not take an owned production"));
    BEZEL_EXPECT_FALSE(jr.ran);
}

// The CALL sibling on the SAME constraint interface with the SAME `owned T`
// declared return. This row passes on BOTH sides of the fix — it is the
// reference the row above is measured against, and pinning it keeps a future
// change from "fixing" the asymmetry in the wrong direction (by silencing
// the call arm instead of teaching the operator arm).
BEZEL_TEST(constraint_operator_owned_producer_4737,
           const_binding_over_constraint_owned_call_rejected_spec_11_8) {
    JitResult jr = runAot(
        std::string(kINumberAndMeters) +
        "static class F "
        "  public static method Z<T>(): owned T where T: INumber<T> "
        "    const z := T.Zero(); "
        "    return z; "
        "  end Z; "
        "end F; "
        "class C public static method Main(): int "
        "  using var t := F.Z<Meters>(); return t.Value; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        jr, "must not take an owned production"));
    BEZEL_EXPECT_FALSE(jr.ran);
}

// ===========================================================================
// THE ASYMMETRY ITSELF — both producers in ONE generic body, so the row
// cannot pass by the two shapes being compiled differently. §28.3's auto-drop
// warning names the binding, so each producer is pinned by NAME rather than
// by a warning count (a count would also be satisfied by two warnings about
// the same binding).
//
// FAIL-WITHOUT: pre-fix only the `z` warning is emitted; the `r` assertion
// fails. Post-fix both are.
// ===========================================================================

BEZEL_TEST(constraint_operator_owned_producer_4737,
           constraint_operator_and_call_classify_alike_spec_11_8) {
    auto r = parseAndCheckOwnership(
        std::string(kINumberAndMeters) +
        "static class F "
        "  public static method M<T>(a: T, b: T): int where T: INumber<T> "
        "    var z := T.Zero(); "
        "    var q := a + b; "
        "    return 0; "
        "  end M; "
        "end F;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    // The CALL producer — Owned before and after the fix.
    BEZEL_EXPECT_TRUE(ownWarnContains(
        r, "owned value in `z` is implicitly disposed at the end of its scope"));
    // The OPERATOR producer — Borrowed before the fix (NO warning), Owned
    // after. This is the assertion that flips.
    BEZEL_EXPECT_TRUE(ownWarnContains(
        r, "owned value in `q` is implicitly disposed at the end of its scope"));
}

// ===========================================================================
// CONTROL — BUILT-IN OPERATORS STAY BORROWED. `a + b` over int / string /
// decimal is a compiler intrinsic (§3.5) with no operator MethodSymbol, so
// it never reaches the arm at all. If this ever regresses, every arithmetic
// expression in the language acquires a disposal obligation.
//
// This row RUNS the program (lane-brief §13): a `compile_rc == 0` oracle
// would not distinguish "accepted and correct" from "accepted and miscompiled".
// It passes on BOTH sides of the fix, by design — it is a control.
// ===========================================================================

BEZEL_TEST(constraint_operator_owned_producer_4737,
           builtin_operators_carry_no_obligation_spec_28_3) {
    JitResult jr = runAot(
        "module M; class C public static method Main(): int "
        "  var a := 17; var b := 25; "
        "  const i := a + b; "                 // int
        "  const s := \"ab\" + \"cde\"; "      // string (runtime-managed)
        "  const d := 1.5m + 2.5m; "           // decimal
        "  const l := a * b - a; "             // nested builtin operators
        "  Console.WriteLine(i.ToString()); "
        "  Console.WriteLine(s); "
        "  Console.WriteLine(d.ToString()); "
        "  Console.WriteLine(l.ToString()); "
        "  return i; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 42);
    BEZEL_EXPECT_TRUE(jr.stdoutText.find("abcde") != std::string::npos);
    // Nothing about ownership may be said of a builtin arithmetic result.
    BEZEL_EXPECT_FALSE(
        diagnosticMessageContains(jr, "must not take an owned production"));
    BEZEL_EXPECT_FALSE(
        diagnosticMessageContains(jr, "is implicitly disposed"));
}

// The ownership-pass view of the same control, so a future regression is
// caught at the classifier rather than only at the §21.1 consumer.
BEZEL_TEST(constraint_operator_owned_producer_4737,
           builtin_operators_emit_no_ownership_diagnostic_spec_28_3) {
    auto r = parseAndCheckOwnership(
        "module M; class C public static method M2() "
        "  var a := 3; var b := 4; "
        "  var i := a + b; "
        "  var s := \"x\" + \"y\"; "
        "  var d := 1.5m * 2.0m; "
        "end M2; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(ownDiagContains(r, "is implicitly disposed"));
    BEZEL_EXPECT_FALSE(ownDiagContains(r, "owned production"));
}

// ===========================================================================
// SOUNDNESS — the newly-Owned population must dispose EXACTLY once. A
// classification change that manufactures an obligation the value does not
// have is a double-free, not a leak, so leak-freedom alone is not the oracle
// here: `Meters`' destructor prints a per-INSTANCE id and every id is
// asserted to appear exactly once, under the AOT + ASan/LSan checked runner.
//
// This is §11.8's own `Sum<T>` example (spec_0049.bzl) over a class
// substitution — the shape whose operator results this fix newly classifies
// Owned. It passes on both sides (codegen's half landed in #1425); its job
// is to prove the sema half did not break it.
// ===========================================================================

BEZEL_TEST(constraint_operator_owned_producer_4737,
           sum_over_class_substitution_disposes_exactly_once_spec_11_8) {
    JitResult r = runAotChecked(
        std::string(kINumberAndMeters) +
        "static class F "
        "  public static method Sum<T>(items: Array<T>): owned T "
        "      where T: INumber<T> "
        "    var total := T.Zero(); "
        "    for var x in items do total := total + x; end; "
        "    return total; "
        "  end Sum; "
        "end F; "
        "class C public static method Main(): int "
        "  using var m0 := new Meters(10); using var m1 := new Meters(13); "
        "  using var m2 := new Meters(19); "
        "  using var xs := new Array<Meters>(); "
        "  xs.Add(m0); xs.Add(m1); xs.Add(m2); "
        "  using var t := F.Sum(xs); "
        "  return t.Value; "
        "end Main; end C;");
    // NO `if (wasSkipped(r)) return;` guard here, deliberately (#3733). That
    // guard makes the row ABSORBING: on a box without the checked toolchain it
    // returns early and the whole exactly-once oracle below evaporates while
    // the row still reports PASS. This row's entire value is the dispose-count
    // assertion, so a silent skip is worse than a failure. `r.ran` false fails
    // loudly instead, which is the burn-down direction the ratchet enforces.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);  // 10 + 13 + 19
    // 7 instances: m0(0), m1(1), m2(2), Zero()(3), and the three `+` results
    // (4, 5, 6). Each disposed exactly once — no leak AND no double-free.
    for (int id = 0; id <= 6; ++id)
        BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D" + std::to_string(id)),
                        1);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D7"), 0);
}

// §11.8's other half: "At value and runtime-managed substitutions the markers
// are erased and every step is a plain copy (§28.1, §28.9)." The identical
// `Sum<T>` at `T = int` must still compile and produce the right answer —
// the conservative bare-`T` verdict must not leak an obligation into a value
// instantiation's generated code.
BEZEL_TEST(constraint_operator_owned_producer_4737,
           sum_over_value_substitution_still_runs_spec_11_8) {
    JitResult jr = runAot(
        "module M; static class F "
        "  public static method Sum<T>(items: Array<T>): owned T "
        "      where T: INumber<T> "
        "    var total := T.Zero(); "
        "    for var x in items do total := total + x; end; "
        "    return total; "
        "  end Sum; "
        "end F; "
        "class C public static method Main(): int "
        "  using var xs := new Array<int>(); "
        "  xs.Add(10); xs.Add(13); xs.Add(19); "
        "  return F.Sum(xs); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 42);
}
