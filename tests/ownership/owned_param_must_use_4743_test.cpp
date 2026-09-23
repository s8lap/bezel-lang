// Spec-derived tests for issue #4743 — §28.9's trivial-dispose exemption from
// the §28.3 ownership-discharge (auto-drop must-use) WARNING reaches the
// OWNED-PARAMETER must-use site. Spec v0.307.
//
// THE RULE. §28.9 (v0.307) carries the ceremony exemption with §28.3's
// must-use lint; Appendix C.3 item 14 spells the scope: "The exemption covers
// every holder form the warning attaches to". An undischarged `owned` local
// of trivial-dispose type fires no warning, and so does an undischarged
// `owned` parameter of trivial-dispose type. The auto-drop is unconditional
// at every one of them — only the diagnostic is suppressed — exactly as
// #2654 / #2667 / #4400 / C.3 item 14 prescribe for the other holder forms.
//
// THE BUG. `MethodOwnership` had the trivial-dispose exemption gate at the
// OWN-LOCAL site (`Ownership.cpp:1011`, #2654) but NOT at the OWN-PARAMETER
// twin at `Ownership.cpp:1073` (synchronous non-iterator method). That twin
// emitted the §28.3 must-use warning unconditionally — for every owned
// parameter the body never discharges, with no trivial-dispose check. The
// disclosure row is `tests/conformance/rules/cases/s28_9_0032.bzl`, an owned
// `Marker` parameter the body reads exactly once (`m.Tag()`) and returns the
// string; pre-fix, the warning fires for the parameter even though Marker is
// trivial-dispose (no destructor, no owned fields, no non-trivial base).
//
// THE FIX. A one-line gate mirroring L1011 — `if (common::isNonTrivial
// DisposeClass(pt, ctx_)) { … emit warning … }`, where `pt` is the parameter's
// declared type resolved through the canonical L627-629 accessor. The
// `autoDropAtScopeEnd = true` line stays above the gate unchanged; WARNING-
// only suppression, same separation as L1011 / L6927.
//
// FAIL-WITHOUT is not vacuous here: the pre-fix compiler emits the warning
// unconditionally, the post-fix compiler suppresses it ONLY at trivial-
// dispose, and the negative control at the bottom of this file pins that
// a non-trivial-dispose `owned` parameter KEEPS the warning — so the gate
// is exercised in both directions. The two-parameter differentiation
// (`Marker` vs `Logger`) is what makes this test pin the FIX rather than
// a regression that swallows the warning for every owned parameter.
//
// ⚠ SCOPE OF THE FIX — same separation as #2654 / #2667 / #4400:
// the fix suppresses the WARNING ONLY. `ParameterSymbol::autoDropAtScopeEnd`
// is still set (line above the new gate) so codegen still emits exactly the
// same scope-end free — including for trivial-dispose owned parameters,
// whose chain-family arms (registerUsingDisposal, LlvmBackend_Setup.cpp)
// reclaim the backing. Suppressing the free instead of the diagnostic would
// leak the instance's memory, which §28.9 still requires be reclaimed; the
// one-line comment at the fix site says so.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasAutoDropWarning;
using bezel::test::parseAndCheckOwnership;

namespace {

// §28.9 v0.307 / fix #4743: a no-destructor, no-owned-field, no-non-trivial-
// base user CLASS — every §28.9 disqualifier is absent, so the
// `common::isNonTrivialDisposeClass` walk returns false and the §28.3
// must-use warning is suppressed at the OWN-PARAMETER site. Same shape as
// `tests/conformance/rules/cases/s28_9_0032.bzl`'s `Marker`.
const char* kMarkerClass =
    "public class Marker "
    "  public constructor() end constructor; "
    "  public method Tag(): string => \"m\"; "
    "end Marker; ";

// Negative control: §28.9 disqualifier #1 — declaring a destructor makes the
// class non-trivial-dispose regardless of its other fields. The §28.3
// must-use warning MUST keep emitting at an owned parameter of this shape;
// the fix's `isNonTrivialDisposeClass` walk returns true (`hasDtor` arm)
// and the warning gate fires.
const char* kLoggerClass =
    "public class Logger "
    "  public constructor() end constructor; "
    "  destructor end destructor; "
    "  public method Stamp(): string => \"s\"; "
    "end Logger; ";

// `Marker` body that returns the string without ever moving `m` (`return m`
// would transfer ownership out and skip the warning — the test is precisely
// that `m` is undischarged and yet silent).
const char* kUseMarker = "return m.Tag(); ";

// `Logger` body, same shape. The test name carries `keep_warns` because the
// expected post-fix behaviour is that the gate's other arm fires; the
// negative control sits below this on purpose.
const char* kUseLogger = "return w.Stamp(); ";

// Wrap a parameterless declaration string and a method-body string in the
// minimum module/class/method shell. The parameter is added directly by the
// test, since the body of every case is one method (no closures, no async,
// no iterator), the same shell works for both.
std::string wrap(const std::string& decls, const std::string& parm,
                 const std::string& body) {
    return std::string("module M; ") + decls +
           "class C public static method Take(" + parm + "): string " +
           body + " end Take; end C;";
}

}  // namespace

// =============================================================================
// POSITIVE — `Marker` is trivial-dispose, so the §28.3 must-use warning is
// suppressed at the OWNED-PARAMETER site.
// =============================================================================

// §28.9 v0.307 / fix #4743: an undischarged `owned` parameter of
// trivial-dispose class type draws no must-use warning. Mirrors `s28_9_0032`.
BEZEL_TEST(owned_param_must_use_4743,
           trivial_dispose_user_class_param_is_silent_spec_28_9) {
    auto r = parseAndCheckOwnership(wrap(
        std::string(kMarkerClass), "owned m: Marker", std::string(kUseMarker)));
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r, "m"));
}

// §28.9 also names `Random` (no destructor, no owned fields) as
// trivial-dispose in its own worked example sentence. Pinned here so a
// regression that re-classifies builtins (the §28.3 builtin-borrowing fix
// at #2667 is the sibling precedent) cannot leave the parameter site
// silently-warning on a builtin either.
BEZEL_TEST(owned_param_must_use_4743,
           trivial_dispose_builtin_param_random_is_silent_spec_28_9) {
    auto r = parseAndCheckOwnership(
        std::string("module M; ") +
        "class C public static method Take(owned r: Random): int " +
        "  return r.Next(); end Take; end C;");
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r, "r"));
}

// =============================================================================
// NEGATIVE CONTROL — pin that the new gate does NOT eat the warning for a
// genuinely non-trivial-dispose parameter (the same `isNonTrivialDisposeClass`
// table that L1011 consults at the OWN-LOCAL site, with the OWNED-PARAMETER
// twin now reading from it).
// =============================================================================

// Adding a destructor makes `Logger` non-trivial-dispose (§28.9's first
// disqualifier), so the §28.3 must-use warning MUST keep firing at the
// owned parameter — this is the test that catches a regression that
// swallowed the warning for every owned parameter (the more dangerous
// direction: an undischarged non-trivial-dispose `owned` parameter still
// needs to be discharged explicitly, and the warning is the only signal).
BEZEL_TEST(owned_param_must_use_4743,
           non_trivial_dispose_param_keeps_warning_spec_28_3) {
    auto r = parseAndCheckOwnership(wrap(
        std::string(kLoggerClass), "owned w: Logger", std::string(kUseLogger)));
    BEZEL_EXPECT_TRUE(hasAutoDropWarning(r, "w"));
}
