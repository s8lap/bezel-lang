// fix #6416 (spec §12.1, v0.338) — the §12.1 base-type-path walk is TOTAL;
// `baseTypePathTo` used to carry a magic `depth > 64` cap.
//
// THE DEFECT, AND ITS DIRECTION. §12.1 states override matching with no depth
// qualifier — "Visibility, return type, parameter types, and parameter
// modifiers must match the base member exactly" — and §11.1/§11.2 put no limit
// on how long a base chain may be. `baseTypePathTo` nevertheless returned
// "not reachable" once the walk passed `depth > 64`. That made
// `buildBaseClassGenericSubst` compose an EMPTY substitution map, so the base
// member's `P(a: T)` was read UNSUBSTITUTED, and a chain 65 classes deep
// produced:
//
//   "override of `P` does not match the base member's parameter types (§12.1)"
//
// for a program that matches at every level. This is a FALSE REJECTION — the
// mirror of #6391's and #6408's silent acceptances, and worth its own issue
// because the diagnostic names a rule the program satisfies and gives the
// reader nothing to act on.
//
// THE BOUNDARY, measured. With `n` intermediate generic classes between
// `B<T>` and `E`, the walk enters `C1` at parameter `n` and tests `B` by
// pointer BEFORE recursing into it, so `n <= 64` composed a path and `n >= 65`
// did not. Depth 64 is kept here as the below-boundary CONTROL: it must behave
// identically before and after the fix, which is what shows the rows above it
// fail because of the cap and not for some other reason.
//
// THE FIX removes the count and replaces it with a per-path cycle guard
// (`std::unordered_set<const TypeSymbol*> onChain`, inserted on entry, erased
// on every exit so a class reachable by two base paths is still walked on
// both) — the same instrument #6391 used for the §12.9 nesting walk and #6417
// for the §32.13 record walk. The bound is REMOVED rather than raised: a
// larger literal leaves the same hole one constant higher, and a cap that
// DIAGNOSED would reject a program §12.1 admits for a reason Appendix C.1 does
// not list.
//
// WHY THE MISMATCH ROWS ARE HERE AND WHY THEY ARE NOT THE PIN. This fix moves
// the walk from "stops composing" to "composes", so the walk now reaches
// depths it never reached. Every one of those depths must still REJECT an
// override that genuinely mismatches. A fix that only widened acceptance —
// dropping the substitution, bailing out of the override check past the old
// bound, or deleting the diagnostic — would turn this issue's false rejection
// into a silent acceptance, which is strictly worse. Those rows are green
// pre-fix (the cap rejected everything at depth >= 65, for the wrong reason,
// by the same door), so they are guards, not the regression pin; the pin is the
// accept rows.
//
// THE CYCLE ROW is the ordering question the fix had to answer rather than
// assume: can a cycle guard that fires inside `baseTypePathTo` pre-empt
// §12.10's own diagnostic and turn a rejected program into a silently accepted
// one? MEASURED: no — §12.10's check runs during class-graph construction,
// before any override check reaches this walk, so a three-class cycle still
// arrives as `circular inheritance detected`, with no §12.1 override
// diagnostic anywhere in the output.
//
// BEHAVIOUR FLIP (named, with its new expectation). The only verdict that
// changes is the §12.1 override match at 65 or more levels of base-class
// nesting: a CORRECT override at that depth was REJECTED and is now ACCEPTED.
// An incorrect override at that depth still rejects, with the same diagnostic
// as before (rows 9–11). No existing gtest or lit test pinned the old verdict
// — `git grep 'does not match the base member' tests/` reaches §12.1 conformance
// rows whose programs are all shallow, and no row in `tests/sema/` builds a
// base chain past 64. So no other test changes expectation.
//
// THE REVISION CITED. v0.338 is the spec's current revision
// (`docs/Bezel_Language_Spec.md` header) and is the revision of the §12.1
// override-matching sentence read above.

#include <string>

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains,
                                            // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAot
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// §12.1's override-mismatch diagnostic, verbatim from `checkOverrideSignatureMatch`'s
// `err` lambda. On the deep-but-correct rows this string is what the depth cap
// produced for a program that matches, so its ABSENCE is the regression pin.
const char* kParamMismatch = "does not match the base member's parameter types";

// §12.10's own cycle diagnostic, from `checkCycleFrom`. The cycle row asserts
// it is present and kParamMismatch is ABSENT, which together show the new guard
// did not pre-empt §12.10 and did not mis-report the cycle as an override
// problem.
const char* kCycle = "circular inheritance detected";

const char* kMain = "class C public static method Main(): int => 0; end C; ";

// n intermediate generic classes `C1<T> … Cn<T>` between `B<T>` and `E`, each
// passing its own parameter straight through, and `E: Cn<ARG>`. The override
// is written with PARAM, so the same generator produces both the correct
// program (PARAM == ARG) and the genuinely mismatched one.
std::string chain(int n, const std::string& arg, const std::string& param) {
    std::string r = "module M; ";
    r += "public class B<T> public virtual method P(a: T): int => 1; end B; ";
    for (int i = 1; i <= n; ++i) {
        const std::string prev = (i == 1) ? "B" : ("C" + std::to_string(i - 1));
        r += "public class C" + std::to_string(i) + "<T>: " + prev + "<T> end C" +
             std::to_string(i) + "; ";
    }
    r += "public class E: C" + std::to_string(n) + "<" + arg + "> ";
    r += "public override method P(a: " + param + "): int => 4; end E; ";
    r += kMain;
    return r;
}

// The same n-level chain with `E` GENERIC: the base member's `T` is bound to
// the derived class's own parameter `Z` rather than to a literal. See the note
// above these rows — this shape is GREEN pre-fix (the parameter-scope
// comparison pairs `T` against `Z` even with no substitution composed), so it
// is a narrowing control, not a pin.
std::string chainGenericParam(int n) {
    std::string r = "module M; ";
    r += "public class B<T> public virtual method P(a: T): int => 1; end B; ";
    for (int i = 1; i <= n; ++i) {
        const std::string prev = (i == 1) ? "B" : ("C" + std::to_string(i - 1));
        r += "public class C" + std::to_string(i) + "<T>: " + prev + "<T> end C" +
             std::to_string(i) + "; ";
    }
    r += "public class E<Z>: C" + std::to_string(n) + "<Z> ";
    r += "public override method P(a: Z): int => 4; end E; ";
    r += kMain;
    return r;
}

// §12.10's own shape (the same one tests/lit/sema/sema_cycle__*_diagnoses.bzl
// pins): A → B → C → A. The entry-point class is named `Main` here, not the
// `C` kMain uses, because this program already declares a top-level `C`
// (§11.3: top-level type names must be unique in a module).
std::string threeClassCycle() {
    return "module M; class A : B end A; class B : C end B; class C : A end C; "
           "class Main public static method Main(): int => 0; end Main; ";
}

}  // namespace

// ===========================================================================
// THE REGRESSION PIN — a correct override at 65+ levels of base nesting is
// ACCEPTED. These rows fail pre-fix: the compiler rejected them with
// kParamMismatch for a program that matches §12.1 at every level.
// ===========================================================================

BEZEL_TEST(sema_base_type_path_depth_total_6416,
           depth_5_correct_override_accepted) {
    auto r = runAot(chain(5, "string", "string"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kParamMismatch));
}  // shallow control — identical pre- and post-fix

BEZEL_TEST(sema_base_type_path_depth_total_6416,
           depth_64_correct_override_accepted) {
    auto r = runAot(chain(64, "string", "string"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kParamMismatch));
}  // boundary control — the last depth the old cap composed

BEZEL_TEST(sema_base_type_path_depth_total_6416,
           depth_65_correct_override_accepted) {
    auto r = runAot(chain(65, "string", "string"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kParamMismatch));
}  // first depth the cap refused to compose

BEZEL_TEST(sema_base_type_path_depth_total_6416,
           depth_70_correct_override_accepted) {
    auto r = runAot(chain(70, "string", "string"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kParamMismatch));
}

BEZEL_TEST(sema_base_type_path_depth_total_6416,
           depth_200_correct_override_accepted) {
    auto r = runAot(chain(200, "string", "string"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kParamMismatch));
}  // far past the old budget

// THE SAME ROWS WITH A PARAMETER BINDING — and an honest note on what they
// are worth. MEASURED fail-without on the base: `depth_5`/`depth_65`/
// `depth_200_parameter_override_accepted` are GREEN pre-fix, so they are NOT
// regression pins. The reason is §12.1's parameter-scope comparison
// (`overrideParamTypeMatches` with `tparamScope`, #6000): when the base member
// is read UNSUBSTITUTED as `P(a: T)` and the override declares `P(a: Z)`,
// that comparison already pairs the two PARAMETERS and accepts, so the missing
// substitution is invisible in this shape. They are kept as narrowing
// CONTROLS — they exercise a different comparison than the literal rows
// (`overrideSpellingDiffers` against a composed `string`) at the same depths,
// and the fix must not disturb them. Nothing here is claimed to be broken by
// the cap.

BEZEL_TEST(sema_base_type_path_depth_total_6416,
           depth_5_parameter_override_accepted) {  // narrowing control — green pre-fix
    auto r = runAot(chainGenericParam(5));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kParamMismatch));
}  // shallow control — identical pre- and post-fix

BEZEL_TEST(sema_base_type_path_depth_total_6416,
           depth_65_parameter_override_accepted) {  // narrowing control — green pre-fix
    auto r = runAot(chainGenericParam(65));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kParamMismatch));
}

BEZEL_TEST(sema_base_type_path_depth_total_6416,
           depth_200_parameter_override_accepted) {  // narrowing control — green pre-fix
    auto r = runAot(chainGenericParam(200));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kParamMismatch));
}

// ===========================================================================
// THE ANTI-INVERSION GUARD. The walk now reaches depths it never reached, so
// it must reach them and still REJECT. `int` is not `string`, so §12.1's
// "parameter types … must match the base member exactly" applies at every
// depth, with the specific diagnostic. Pre-fix these also reject (the cap
// rejected everything past 64, for the wrong reason, through the same door),
// so they are guards against a silent-acceptance inversion, not the pin.
// ===========================================================================

BEZEL_TEST(sema_base_type_path_depth_total_6416,
           depth_5_mismatched_override_rejected) {
    auto r = runAot(chain(5, "string", "int"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kParamMismatch));
}  // shallow control — identical pre- and post-fix

BEZEL_TEST(sema_base_type_path_depth_total_6416,
           depth_65_mismatched_override_rejected) {
    auto r = runAot(chain(65, "string", "int"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kParamMismatch));
}

BEZEL_TEST(sema_base_type_path_depth_total_6416,
           depth_200_mismatched_override_rejected) {
    auto r = runAot(chain(200, "string", "int"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kParamMismatch));
}

// ===========================================================================
// THE ORDERING MEASUREMENT. §12.10 requires the cycle to be REPORTED, not
// "silently broken or otherwise tolerated", so the guard that replaced the
// depth cap must not be the thing that terminates a cycle walk. This row
// pins: §12.10's own diagnostic arrives, the §12.1 override diagnostic does
// NOT, and the compiler neither crashed nor hung.
// ===========================================================================

BEZEL_TEST(sema_base_type_path_depth_total_6416,
           inheritance_cycle_reports_12_10_not_override_mismatch) {
    auto r = runAot(threeClassCycle());
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kCycle));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kParamMismatch));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "compiler CRASHED"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "terminated by signal"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "did not finish within"));
}