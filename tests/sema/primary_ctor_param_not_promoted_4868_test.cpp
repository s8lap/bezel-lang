// Spec-derived regression test for issue #4868 (§22.10, v0.307):
// "primary constructor parameters are not automatically promoted to public
// properties".
//
// spec §22.10 (v0.307) "Primary constructors": "Primary-constructor
// parameters are not automatically promoted to public properties. To expose
// one, declare a property explicitly." The compiler shared SymbolCollection's
// record-vs-class branch — both set `fs->visibility = ast::Visibility::Public`
// on the synthesized `FieldSymbol` carrying `posParam` — so a class's primary-
// constructor parameter looked public to outside-class `p.x` member access,
// even though §22.10 is unambiguous that the parameter does not appear on the
// class's public surface. `resolveMemberAccess` accepted the access, the call
// lowered, and the emitted program referenced a private-or-absent slot at
// runtime (compile clean, exit 0, wrong value — silent-wrong-code defect
// class; see the §22.10 case file `s22_10_0012.bzl` whose `expect: reject`
// was banked as `fail` divergence for the same reason).
//
// WHAT THE FIX DOES. `resolveMemberAccess` now detects the shape — `member`
// is a `FieldSymbol` with `posParam != null` whose parent is a `Class` and
// whose access site is outside the declaring class — and emits "no member
// `x` on type `Point` from outside the declaring class — primary-constructor
// parameters are not auto-promoted to public properties (§22.10); declare a
// property explicitly to expose one" (the §11.3 / §12.1 "no member" wording
// rather than the §13.1 "is private" wording, because per §22.10 the
// parameter is not on the public surface at all — even a `protected`
// subclass declaration would not make `p.x` legal). In-class access keeps
// working: `currentType_ == fs->parent` makes the new arm inert, and the
// existing scope-based parameter binding inside a method body (which does
// not consult the FieldSymbol) is unchanged.
//
// FAIL-WITHOUT / PASS-WITH. Without the fix, every `rejects_*` row below
// compiles CLEAN on HEAD (no diagnostic at any severity); with the fix each
// one reports the new §22.10 "no member" message. The two `accepts_*` rows
// are the false-rejection guard — record positional access from outside
// (§9.1 still applies) and in-class `this.x` (§22.10 in-scope) must both
// keep compiling — without them a too-aggressive fix would over-reject the
// §9.1 record surface or block in-body access to a captured parameter.

#include <cstdio>
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// True iff SOME error diagnostic carries the §22.10 "no member" wording.
// Rejection rows assert on the SPECIFIC message — `runAot` synthesizes an
// Error diagnostic for a runtime SIGSEGV/ICE too, so a coarse `hasErrors()`
// cannot tell a genuine reject from a downstream crash.
bool hasNoMemberDiagnostic(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("no member") == std::string::npos) continue;
        if (d.message.find("22.10") == std::string::npos) continue;
        return true;
    }
    return false;
}

void dumpDiagnostics(const JitResult& r) {
    std::fprintf(stderr, "  diagnostics (count=%zu):\n",
                 r.diagnostics.diagnostics().size());
    for (const auto& d : r.diagnostics.diagnostics())
        std::fprintf(stderr, "    [%s] %s\n",
                     d.severity == bezel::Severity::Error ? "E" : "W",
                     d.message.c_str());
    if (!r.stdoutText.empty())
        std::fprintf(stderr, "  stdout: %s\n", r.stdoutText.c_str());
}

}  // namespace

// ---------------------------------------------------------------------------
// Rejection rows. Each pins a SPECIFIC §22.10 diagnostic — the `no member`
// wording rather than the §13.1 `is private` one, because §22.10 says the
// parameter is not on the public surface at all (not "private to declaring
// class", which would invite a `protected` subclass workaround that §22.10
// also rejects).
// ---------------------------------------------------------------------------

// #4868 / §22.10 (v0.307): the case file's exact body — `class Point(x, y)`,
// `class Coordinate` accesses `p.x` and `p.y` from outside `Point`. The
// params are referenced only in initializer positions (`_x := x`, `_y := y`)
// so they are NOT captured into fields per §22.10's capture-iff-body-
// referenced rule; they exist solely as primary-constructor parameters, and
// §22.10 says those do not appear on the public surface.
BEZEL_TEST(primary_ctor_param_not_promoted_4868,
           rejects_uncaptured_outside_access_spec_22_10) {
    JitResult r = runAot(
        "module M; "
        "class Point(x: int, y: int) "
        "  private _x: int := x; "
        "  private _y: int := y; "
        "end Point; "
        "class Coordinate "
        "  public static method Sum(p: Point): int => p.x + p.y; "
        "end Coordinate; "
        "class C public static method Main(): int "
        "  return 0; "
        "end Main; end C;");
    if (!hasNoMemberDiagnostic(r)) dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(hasNoMemberDiagnostic(r));
}

// #4868 / §22.10 (v0.307): the CAPTURED case — `start` is referenced in a
// body position (the `Value()` method), so per §22.10 it IS captured as a
// compiler-generated private field. The spec's "not auto-promoted to public
// properties" rule applies regardless: an outside-class `c.start` still has
// no public member to expose. Pre-fix the compiler accepted (the FieldSymbol
// had `Visibility::Public`); post-fix it rejects with the §22.10 message.
// This is the second row the `accepts_*` `Value()`-call control guards —
// the method call on the captured parameter is legal and must keep working.
BEZEL_TEST(primary_ctor_param_not_promoted_4868,
           rejects_captured_outside_access_spec_22_10) {
    JitResult r = runAot(
        "module M; "
        "class Counter(start: int) "
        "  public method Value(): int => start; "
        "end Counter; "
        "class C "
        "  public static method Probe(c: Counter): int => c.start; "
        "end C; "
        "class Program public static method Main(): int "
        "  return 0; "
        "end Main; end Program;");
    if (!hasNoMemberDiagnostic(r)) dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(hasNoMemberDiagnostic(r));
}

// ---------------------------------------------------------------------------
// False-rejection guards. Each pins a SHAPE — record positional access from
// outside (§9.1 still applies), and in-class `this.x` access on a captured
// primary-constructor parameter (§22.10 in-scope). Without these the fix
// would over-reject the §9.1 record surface or block in-body access to a
// captured parameter.
// ---------------------------------------------------------------------------

// #4868 / §9.1 (v0.307): a RECORD's positional fields are public read-only
// members (§9.1) — `r.x` from outside `R` is legal and stays legal. The fix
// must not generalize to records. Without this control row, a too-aggressive
// fix (e.g. `posParam` is rejected wholesale) would silently flip the
// rejection behaviour onto record surfaces and reject `r.x` here too.
BEZEL_TEST(primary_ctor_param_not_promoted_4868,
           accepts_record_outside_access_spec_9_1) {
    JitResult r = runAot(
        "module M; "
        "record R(x: int, y: int) "
        "end R; "
        "class C "
        "  public static method Sum(r: R): int => r.x + r.y; "
        "end C; "
        "class Program public static method Main(): int "
        "  return 0; "
        "end Main; end Program;");
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_FALSE(hasNoMemberDiagnostic(r));
}

// #5403 / §22.10 (v0.315): in-class `this.x` on a captured primary-
// constructor parameter is now a COMPILE ERROR. The capture field is not
// a declared member: `this.name` is member access (§2.7) and reaches
// declared members only, never a primary-constructor parameter or its
// capture (§22.10). This row was previously accepts_in_class_this_access
// (the pre-v0.315 rule let `this.x` reach the capture), but v0.315's
// ruling on #5383 makes this a reject.
BEZEL_TEST(primary_ctor_param_not_promoted_4868,
           rejects_in_class_this_capture_access_spec_22_10) {
    JitResult r = runAot(
        "module M; "
        "class Counter(start: int) "
        "  public method Value(): int => this.start; "
        "end Counter; "
        "class Program public static method Main() "
        "  using var c := new Counter(5); "
        "  Console.WriteLine(c.Value()); "
        "end Main; end Program;");
    // Must NOT run — the program is rejected at compile time.
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    // Verify the specific diagnostic mentions the §22.10 capture rule.
    bool sawCaptureRule = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find("capture field") != std::string::npos &&
            d.message.find("22.10") != std::string::npos) {
            sawCaptureRule = true;
        }
    }
    BEZEL_EXPECT_TRUE(sawCaptureRule);
}

// #5403 / §22.10 (v0.315): control row — a class where a captured
// primary-constructor parameter coexists with a same-named member parameter.
// The bare name must resolve without error (whether to member param or
// capture is spec-question #5404 — this lane does NOT decide it).
// This row only verifies the compile succeeds.
BEZEL_TEST(primary_ctor_param_not_promoted_4868,
           bare_name_resolves_with_shadowed_capture_spec_22_10) {
    JitResult r = runAot(
        "module M; "
        "class Counter(p: int) "
        "  private method Use(): int => p; "  // p captured as field
        "  public method Value(p: int): int => p; "  // member param `p`
        "end Counter; "
        "class Program public static method Main() "
        "  using var c := new Counter(99); "
        "  Console.WriteLine(c.Value(5)); "
        "end Main; end Program;");
    // Just verify it compiles and runs — the value depends on bare-name
    // resolution (§22.10, v0.315, spec-question #5404).
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
