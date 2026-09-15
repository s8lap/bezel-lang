// Spec-derived test for fix #4111 — cleanup of a bare enum/choice indexer
// double-diagnostic left behind by fix #4101, plus a stale
// `checkTypeMethodBodies` header comment (TypeResolution_impl.hpp).
//
// `Resolver::checkEnumRestrictions` / `Resolver::checkChoiceRestrictions`
// (TypeResolution_Checks.cpp) already unconditionally reject ANY indexer on
// an enum or choice body — §10.5: "An enum may not declare: ... Indexers.";
// §35.1/§35.2 carry the identical rule for choices — independent of body
// presence. Fix #4101 added a SEPARATE, unrelated check in
// `checkTypeMethodBodies`: a non-abstract indexer with NO body
// (`PropertyBodyKind::Auto`) must be rejected (§12.5/§23.4) — but that new
// arm did not exclude enum/choice bodies, so a BARE (bodyless, non-abstract)
// indexer on either kind tripped BOTH checks: two diagnostics, one real
// member. Both are individually correct (no false accept, no false reject)
// but this codebase has explicit precedent for caring about the class —
// #3601's changelog entry records retiring an earlier duplicate enum loop
// "to avoid double-reporting".
//
// MEASURED on R17 cycle-3 base `3494cd1` (pre-fix), `./build/bezelc -c`:
//   enum Color Red, Green, Blue; public property [i: int]: int; end Color;
// printed BOTH
//   "enum `Color` may not declare an indexer (§10.5)"
//   "non-abstract indexer must have a body (§12.5/§23.4)"
// and the equivalent choice shape printed the analogous pair. This fix adds
// an enum/choice short-circuit to `checkTypeMethodBodies`'s indexer arm
// (mirroring the interface exclusion already there) so the member-kind
// rejection alone survives — proven below by asserting the SURVIVING
// diagnostic's exact text AND that the body-presence diagnostic's text is
// ABSENT, not merely that "some" diagnostic exists.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

int runProg(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

int countDiagnosticsContaining(const bezel::test::JitResult& r,
                                const char* sub) {
    int n = 0;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) ++n;
    return n;
}

}  // namespace

// =====================================================================
// REJECT — a bare enum/choice indexer must produce EXACTLY ONE diagnostic:
// the member-kind rejection, never the body-presence one.
// =====================================================================

BEZEL_TEST(enum_choice_indexer_double_diag_4111,
           enum_bare_indexer_single_diagnostic_spec_10_5) {
    auto r = runAot(
        "module M; "
        "enum Color4111 "
        "  Red, Green, Blue; "
        "  public property [i: int]: int; "
        "end Color4111; "
        "class C4111 public static method Main(): int "
        "  return 0; "
        "end Main; end C4111;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    // The surviving diagnostic: exactly one copy of the correct rejection.
    BEZEL_EXPECT_EQ(
        countDiagnosticsContaining(
            r, "enum `Color4111` may not declare an indexer"),
        1);
    // The would-be duplicate must be entirely absent — not merely
    // outnumbered.
    BEZEL_EXPECT_EQ(
        countDiagnosticsContaining(
            r, "non-abstract indexer must have a body"),
        0);
    // Total diagnostic count is exactly one (belt-and-braces: rules out an
    // unrelated third diagnostic masking the count above).
    BEZEL_EXPECT_EQ(static_cast<int>(r.diagnostics.diagnostics().size()), 1);
}

BEZEL_TEST(enum_choice_indexer_double_diag_4111,
           choice_bare_indexer_single_diagnostic_spec_35_1_35_2) {
    auto r = runAot(
        "module M; "
        "choice Shape4111 "
        "  Circle(radius: double), Square(side: double); "
        "  public property [i: int]: int; "
        "end Shape4111; "
        "class C4111b public static method Main(): int "
        "  return 0; "
        "end Main; end C4111b;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_EQ(
        countDiagnosticsContaining(
            r, "choice `Shape4111` may not declare an indexer"),
        1);
    BEZEL_EXPECT_EQ(
        countDiagnosticsContaining(
            r, "non-abstract indexer must have a body"),
        0);
    BEZEL_EXPECT_EQ(static_cast<int>(r.diagnostics.diagnostics().size()), 1);
}

// =====================================================================
// ACCEPT / CONTROL — the body-presence rule (#4101) must still function
// normally on the kinds it was written for (class, interface); the
// enum/choice member-kind rejection must still fire on OTHER indexer body
// shapes (abstract, real body) exactly as before this fix, since #4101's
// arm never reached those anyway.
// =====================================================================

// CONTROL — a bare indexer on a CLASS (not enum/choice) must still be
// rejected by #4101's body-presence rule alone, with exactly ONE
// diagnostic — proving the new enum/choice short-circuit does not touch
// the class path at all.
BEZEL_TEST(enum_choice_indexer_double_diag_4111,
           class_bare_indexer_single_diagnostic_control_spec_12_5_23_4) {
    auto r = runAot(
        "module M; "
        "class Grid4111 "
        "  public property [i: int]: int; "
        "end Grid4111; "
        "class C4111c public static method Main(): int "
        "  return 0; "
        "end Main; end C4111c;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_EQ(
        countDiagnosticsContaining(
            r, "non-abstract indexer must have a body"),
        1);
    BEZEL_EXPECT_EQ(static_cast<int>(r.diagnostics.diagnostics().size()), 1);
}

// ACCEPT — a class indexer with a real body: entirely unaffected.
BEZEL_TEST(enum_choice_indexer_double_diag_4111,
           class_indexer_with_body_accepted_control_spec_23_4) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "class Grid4111b "
        "  private owned _data: Array<int> := new Array<int>(); "
        "  public property [i: int]: int => 7; "
        "end Grid4111b; "
        "class C4111d public static method Main(): int "
        "  var g := new Grid4111b(); "
        "  return g[0]; "
        "end Main; end C4111d;"),
        7);
}

// ACCEPT — an abstract class's abstract indexer (no body, `abstract`
// modifier): the legitimate §12.5 shorthand, must stay accepted — the
// enum/choice short-circuit runs unconditionally on THOSE two kinds only,
// so it cannot affect this class shape either way.
BEZEL_TEST(enum_choice_indexer_double_diag_4111,
           class_abstract_indexer_no_body_accepted_control_spec_12_5) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "abstract class GridBase4111 "
        "  public abstract property [i: int]: int; "
        "end GridBase4111; "
        "class GridImpl4111 : GridBase4111 "
        "  public override property [i: int]: int => 9; "
        "end GridImpl4111; "
        "class C4111e public static method Main(): int "
        "  var g: GridBase4111 := new GridImpl4111(); "
        "  return g[0]; "
        "end Main; end C4111e;"),
        9);
}

// ACCEPT — an interface indexer signature (body-less, no `abstract`
// modifier): already excluded by `isInterface` before this fix's new
// short-circuit is ever reached; unaffected.
BEZEL_TEST(enum_choice_indexer_double_diag_4111,
           interface_indexer_signature_accepted_control_spec_23_4) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "interface IGrid4111 "
        "  property [i: int]: int; "
        "end IGrid4111; "
        "class GridImpl4111b : IGrid4111 "
        "  private _v: int := 11; "
        "  public property [i: int]: int "
        "    get => _v; "
        "    set => do _v := value; end; "
        "  end; "
        "end GridImpl4111b; "
        "class C4111f public static method Main(): int "
        "  var g: IGrid4111 := new GridImpl4111b(); "
        "  return g[0]; "
        "end Main; end C4111f;"),
        11);
}

// CONTROL — an enum indexer that is NOT bare (carries a real body): still
// rejected, exactly once, by the member-kind check alone — #4101's arm
// never fired on this shape even before this fix (`noBody` is false for a
// real body), so this row confirms the short-circuit did not change
// anything here either.
BEZEL_TEST(enum_choice_indexer_double_diag_4111,
           enum_indexer_with_body_still_single_diagnostic_spec_10_5) {
    auto r = runAot(
        "module M; "
        "enum Color4111g "
        "  Red, Green, Blue; "
        "  public property [i: int]: int => i; "
        "end Color4111g; "
        "class C4111g public static method Main(): int "
        "  return 0; "
        "end Main; end C4111g;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_EQ(
        countDiagnosticsContaining(
            r, "enum `Color4111g` may not declare an indexer"),
        1);
    BEZEL_EXPECT_EQ(static_cast<int>(r.diagnostics.diagnostics().size()), 1);
}
