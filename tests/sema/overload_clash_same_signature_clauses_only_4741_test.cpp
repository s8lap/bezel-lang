// Spec-derived regression test for issue #4741 (diagnostic-phrasing
// half) — `Resolver::checkTypeOverloads` in
// `src/sema/TypeResolution_Checks.cpp` correctly REJECTED the
// clause-only-difference overload pair (the verdict is `reject`, and
// has been since the `signaturesIndistinguishable` predicate was
// introduced) but the diagnostic text did NOT name §22.16's "same
// signature" wording, leaving the corpus row
// `tests/conformance/rules/cases/s22_16_0026.bzl` (`//! diagnostic:
// same signature`) fail-of-text-pass-of-verdict (a class-(b)
// rule-fires-compiler-wrong case per the four-way classification).
//
// THE RULE. Spec §22.16 (Contract clauses, v0.250, #3038):
//   "Clauses never distinguish overloads and are not part of §22.6
//    resolution; two members differing only in clauses are the same
//    signature and clash."
//
// THE DEFECT. The existing diagnostic
// (`overload of `M` is indistinguishable from another overload …`)
// was correct for the §22.6/§11.3 modifier-collapsing case
// (`owned`/`scoped` on parameters), but for the §22.16
// clause-only-difference case it named the symptom ("indistinguishable")
// without naming the rule's mechanism ("same signature"). The corpus
// row's `//! diagnostic: same signature` substring therefore failed
// substring-match, and the row showed up in every
// `runner.py … --filter s22_16` output as a "diagnostic does not carry
// the rule's wording" warning alongside 25 OK rows.
//
// FAIL-WITHOUT (measured on r19-c23 base b5b4d338): the diagnostic
// substring "same signature" is NOT present in the compile error
// message (the message names "indistinguishable" instead). FAIL-WITH:
// the §22.16-specific clause-only path emits a diagnostic whose text
// contains "same signature" (the case's `//! diagnostic:` substring)
// and the existing `signaturesIndistinguishable`/modifier-collapse path
// keeps emitting "indistinguishable" for §22.6/§11.3 (so the
// neighbouring cases `s22_6_0001` and `s22_6_0016` continue to pass
// unchanged).

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

// =========================================================================
// §22.16 — the corpus cluster row s22_16_0026's exact repro: two methods
// differing ONLY in their `requires(...)` clause clash (the signature is
// otherwise identical), and the diagnostic MUST name §22.16's "same
// signature" wording (NOT the §22.6/§11.3 modifier-collapsing
// "indistinguishable" wording — the clause-only case is a §22.16
// verdict even though it surfaces through §22.6's overload-resolution
// machinery).
// =========================================================================
BEZEL_TEST(overload_clash_same_signature_clauses_only_4741,
           clause_only_overload_clash_diagnostic_spec_22_16) {
    auto r = runAot(
        "module M;\n"
        "class Ops\n"
        "  public static method Check(x: int): bool\n"
        "    requires(x > 0)\n"
        "    return true;\n"
        "  end Check;\n"
        "  public static method Check(x: int): bool\n"
        "    requires(x < 100)\n"
        "    return false;\n"
        "  end Check;\n"
        "end Ops;\n");
    // Correct verdict — rejected, never ran.
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    // The §22.16 rule's wording — the case's `//! diagnostic: same
    // signature` substring pin.
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "same signature"));
    // §22.16 citation — the spec section the clause-only rejection
    // belongs to (the overload machinery is §22.6/§11.3; the rule for
    // why clauses don't distinguish is §22.16).
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "22.16"));
    // Negative control: the clause-only diagnostic must NOT claim the
    // §22.6/§11.3 `scoped` modifier story (a method has no `scoped`
    // parameters here — that wording belongs to the OTHER overload-
    // clash path).
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "`scoped`"));
    BEZEL_EXPECT_FALSE(
        diagnosticMessageContains(r, "neither `owned`"));
}

// =========================================================================
// Companion (same-fix coverage, §22.6 sibling): two methods differing
// in a `scoped` parameter modifier STILL produce the §22.6/§11.3
// "indistinguishable" wording (the diagnostic that neighbouring corpus
// rows `s22_6_0001` / `s22_6_0016` pin). The fix splits the diagnostic
// by clause-only vs other-difference; this row proves the other arm
// is unchanged.
// =========================================================================
BEZEL_TEST(overload_clash_same_signature_clauses_only_4741,
           scoped_modifier_overload_clash_keeps_indistinguishable_wording) {
    auto r = runAot(
        "module M;\n"
        "class Sub\n"
        "  public method Register(cb: () -> ())\n"
        "  end Register;\n"
        "  public method Register(scoped cb: () -> ())\n"
        "  end Register;\n"
        "end Sub;\n");
    // Correct verdict — rejected.
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    // The §22.6/§11.3 wording — the case `s22_6_0016`'s
    // `//! diagnostic: scoped` substring pin.
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "indistinguishable"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "scoped"));
    // And the §22.16 clause-only wording must NOT appear (this is the
    // §22.6/§11.3 arm, not the §22.16 arm).
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "same signature"));
}