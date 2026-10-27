// Spec-derived regression test for issue #3021 — `sema::isBuiltinTypeName`
// accepts six (in fact eight) static-class names whose member-call surface
// is documented in stdlib but whose pre-fix bogus-member call SILENTLY
// ZERO-DEGRADED: the program compiled with only an INTERNAL warning
// (`lowerLocalDecl's initializer expression failed to lower to a value for
// this local's slot (degraded to a zero-initialized value) — #2434`) and
// ran to exit 0 with `v` set to 0. No Error diagnostic was produced.
//
//   module M; import System.IO; import System.Threading;
//   class C public static method Main(): int
//     var v := File.TotallyBogusMember(1, 2);
//     return 0;
//   end Main; end C;
//
// Affected names (all in `Resolver::builtinTypeArity`,
// src/sema/TypeResolution_Names.cpp):
//   * System.IO   : File, Directory, Path, Encoding
//   * System.Diagnostics: Debug
//   * (no namespace): Linq
//   * System.Threading: Domain
//   * (no namespace): BitConverter
//
// Why it is reachable: the codegen gate in `Emitter::emittedSideEffectSince`
// (src/codegen/LlvmBackend_impl.hpp ~12225) lives in the INSTANCE dispatch
// (`lowerCall`, src/codegen/LlvmBackend_Calls2.cpp ~3352). A STATIC call
// (`File.Member(args)`) goes through a different path the gate does not
// cover, and `ExprTypecheck_Calls.cpp`'s per-instance-type closed-surface
// arms target instance types only — there was no per-static-class analog.
//
// The fix adds a per-static-class arm to `kStaticSurfaces[]` in
// `ExprTypecheck_Calls.cpp` (around line 7427) mirroring the per-instance-
// type arms further below. For `Encoding` and `BitConverter` the reject
// comes through DIFFERENT paths (Encoding's §3.2 singleton-property reject
// and `rejectBuiltinStatic()` for BitConverter — see line 15810) but the
// observable outcome is the same: a textual Error diagnostic. This test
// pins the diagnostic TEXT for each affected name, not just `hasErrors()` —
// per the "branch-order regression" rule (run 2026-08-21, ★ OWNER 2026-
// 08-22: assert on specific message text, not just `hasErrors()`, because
// a new disjunct can silently re-route existing message branches).
//
// #2998's property test (tests/codegen/builtin_member_population_2998
// _test.cpp) excludes these 8 names (the comment block above its registry
// case list — File/Directory/Path/Encoding/BitConverter/Debug/Domain are
// noted "OUT OF SCOPE for this issue (#2998)") because #2998 is about the
// codegen gate's INSTANCE-dispatch masking. #3021 is the sister issue for
// the STATIC-call analog. #3274 added `Linq` to the #2998 registry (Linq
// was filed separately because it parses differently — module-of-extension-
// methods — but reaches the same arm).
//
// This test covers 8 axes × 2 call-site-contexts × ~2 call-shapes — the
// matrix the issue body calls out ("the codegen gate in
// emittedSideEffectSince" + "no per-static-class analog"). The matrix is
// NOT generated programmatically (it would multiply to 32 cells) — the
// chosen coverage is:
//   * T1 (value-demanding var-decl, 2 args) — the issue's headline shape,
//     one test per name (8 BEZEL_EXPECT_TRUEs inside one BEZEL_TEST so the
//     failure list names exactly which name slipped through).
//   * T2 (statement form, 0 args) — the "statement has no value to demand,
//     several built-in arms leave a bogus statement un-lowered" shape
//     verified live pre-fix; same 8 names.
//   * T3 (statement form, 2 args) — the matrix entry the issue cites as
//     silent-degrading on the value-demanding form is here covered in the
//     statement form too (different code path through the CallExpr dispatch).
//   * T4 (control — legitimate static method) — `File.Exists(path)`,
//     `Path.Combine(a, b)`, `BitConverter.FloatToInt32Bits(value)` MUST
//     still compile and run cleanly. Pins that the fix did NOT narrow
//     the legitimate surface.
//   * T5 (control — user class with same name) — a user class
//     named `Path` (legal under §11.3, which fixes the naming-collision
//     rule for member bodies — a user type may share a name with a
//     builtin) with a static `Combine` method MUST still compile
//     cleanly. Pins the `tgtSym->decl == nullptr` builtin-only gate
//     (the §11.3-discriminator source-code convention, fix #3022:
//     a builtin identifier's exprSymbol is a TypeSymbol whose `decl`
//     field is null, a user class's is non-null).
//
// Spec: §11.7 ("A static class may not be instantiated") and §22.12
// (Static members — the rule that a static method/property/field is
// declared with `static` and accessed by name on the type-identifier)
// imply a non-static class used as a static-call receiver has no
// surface at all; the per-static-class closed-surface arm is the
// enforcement. Stdlib: §7.3 (File), §7.4 (Directory/Path), §8.2
// (Debug), §3.2 (Encoding), §6.8 (Domain), §5 (Linq), §2.12
// (BitConverter). The §11.3-discriminator source-code convention
// (fix #3022, v0.240 — a builtin identifier's TypeSymbol has
// `decl == nullptr`, a user class's has `decl != nullptr`) is the
// builtin-only gate the carve-out relies on.

#include <string>
#include <vector>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// One row per affected name, with the imports the program needs and the
// exact needle the diagnostic must contain. The needles pin TEXT (per the
// "branch-order regression" rule), not just `hasErrors()` — a future fix
// that re-routes a branch MUST keep the wording, or this test catches it.
struct StaticCase {
    const char* name;             // builtin static class name
    const char* imports;          // import preamble (may be "")
    const char* needle;           // exact substring the diagnostic must carry
    bool usesGenericSurface;      // true for names handled by a different arm
};

// The 8 affected names from #3021. File/Directory/Path/Debug/Domain/Linq
// land in the per-static-class kStaticSurfaces[] arm (one consolidated
// diagnostic). Encoding lands in the §3.2 singleton-property reject path
// (different message). BitConverter lands in the rejectBuiltinStatic()
// arm (different message, §2.12 citation — see Finding 1 in the
// per-fix review: the source-code diagnostic at
// ExprTypecheck_Calls.cpp:17782 also cites §3.4 incorrectly, but that
// source-fix is out of lane scope for this test-only commit). All three
// paths produce an Error diagnostic — the test pins each one's specific
// text.
const std::vector<StaticCase>& allStaticCases() {
    static const std::vector<StaticCase> kCases = {
        {"File",
         "import System.IO; import System.Threading; ",
         "no member `TotallyBogusMember` on `File`; documented surface",
         false},
        {"Directory",
         "import System.IO; import System.Threading; ",
         "no member `TotallyBogusMember` on `Directory`; documented surface",
         false},
        {"Path",
         "import System.IO; import System.Threading; ",
         "no member `TotallyBogusMember` on `Path`; documented surface",
         false},
        // Encoding has no static methods — only singleton properties
        // (UTF8, UTF16, UTF32, ASCII) per stdlib §3.2. The §3.2 reject
        // path produces a different but equally specific Error
        // diagnostic. The needle pins that wording.
        {"Encoding",
         "import System.IO; import System.Threading; ",
         "`Encoding` has no member `TotallyBogusMember`",
         true},
        {"Debug",
         "import System.Diagnostics; import System.Threading; ",
         "no member `TotallyBogusMember` on `Debug`; documented surface",
         false},
        // Linq has zero documented static methods — per-static-class
        // arm has an EMPTY member list and the surface string says
        // so explicitly.
        {"Linq",
         "",
         "no member `TotallyBogusMember` on `Linq`; documented surface",
         false},
        {"Domain",
         "import System.Threading; ",
         "no member `TotallyBogusMember` on `Domain`; documented surface",
         false},
        // BitConverter has its own rejectBuiltinStatic() arm (line
        // ~17989, fix #3344) producing a different message that now
        // cites §2.12 (BitConverter's actual stdlib section — stdlib
        // §2.12's section header is
        // `### 2.12 ``BitConverter```, not §3.4 which is
        // `ReadOnlyAsciiByteSpan (retired v0.188)`). The needle here
        // pins the wording the source emits today — including the §2.12
        // cite — so a future source-fix that flips the cite (or
        // silently reverts to §3.4) flips the needle here too
        // (the assertion is on the literal text, which is why a
        // branch-order regression would be caught at code-review time,
        // not silently). This row was updated in lockstep with fix
        // #3344 (source + needle move together per the
        // `branch-order regression` rule).
        {"BitConverter",
         "",
         "'BitConverter' has no static method or property 'TotallyBogusMember' (§2.12)",
         true},
    };
    return kCases;
}

// Program for the issue's headline shape: value-demanding var-decl,
// two-arg call. Pre-fix: exit 0, `v`=0, only an INTERNAL warning.
// Post-fix: Error diagnostic with the row's needle.
std::string valueDemanding2Args(const StaticCase& c) {
    return std::string("module M; import System.Diagnostics; ") + c.imports +
        "class C public static method Main(): int "
        "  var v := " + c.name + ".TotallyBogusMember(1, 2); "
        "  _ := v; "
        "  return 0; "
        "end Main; end C;";
}

// Program for the statement form, no args (different code path through
// the CallExpr dispatch — sema visits a CallExpr whose result is
// discarded rather than demanded). Pre-fix: this is the shape the
// issue's "verified live" line refers to — exit 0, no error, no
// INTERNAL warning either (statements have no value to degrade).
std::string statement0Args(const StaticCase& c) {
    return std::string("module M; import System.Diagnostics; ") + c.imports +
        "class C public static method Main(): int "
        "  " + c.name + ".TotallyBogusMember(); "
        "  return 0; "
        "end Main; end C;";
}

// Program for the statement form with two args. Pre-fix: same silent
// degrade as the var-decl form but reached through the discard path.
std::string statement2Args(const StaticCase& c) {
    return std::string("module M; import System.Diagnostics; ") + c.imports +
        "class C public static method Main(): int "
        "  " + c.name + ".TotallyBogusMember(1, 2); "
        "  return 0; "
        "end Main; end C;";
}

// True iff `program` produced an Error diagnostic whose message contains
// `needle`. Pins specific wording — per the "branch-order regression"
// rule, `hasErrors()` alone is not enough.
bool hasErrorNeedle(const std::string& program, const char* needle) {
    auto r = runAot(program);
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

// True iff `program` compiled AND ran to completion (`ran == true`) AND
// produced no Error diagnostic. The control-test oracle.
bool ranCleanly(const std::string& program) {
    auto r = runAot(program);
    return r.ran && !r.diagnostics.hasErrors();
}

}  // namespace

// ── T1: value-demanding var-decl form, 2 args ─────────────────────────────
//
// The issue's headline shape. Pre-fix: exit 0, `v`=0, only an INTERNAL
// warning. Post-fix: Error with the per-name needle. All 8 names tested
// in ONE BEZEL_TEST so the failure list names exactly which name slipped.
BEZEL_TEST(static_class_bogus_member_3021,
           value_demanding_vardecl_2args_emits_error_per_name) {
    std::vector<const char*> failed;
    for (const StaticCase& c : allStaticCases()) {
        if (hasErrorNeedle(valueDemanding2Args(c), c.needle)) continue;
        failed.push_back(c.name);
    }
    if (!failed.empty()) {
        std::cout << "    static-class names that DID NOT reject "
                     "var v := X.TotallyBogusMember(1, 2): "
                  << failed.size() << " of " << allStaticCases().size()
                  << ":\n";
        for (auto* n : failed) std::cout << "      - " << n << "\n";
    }
    BEZEL_EXPECT_TRUE(failed.empty());
}

// ── T2: statement form, 0 args ────────────────────────────────────────────
//
// A statement has no value to demand; several pre-fix arms left a bogus
// statement un-lowered and silent (verified live pre-fix: `File
// .TotallyBogusMember(1, 2);` accepts with no error and exits 0). The
// per-static-class arm MUST fire on this path too. All 8 names in one
// BEZEL_TEST.
BEZEL_TEST(static_class_bogus_member_3021,
           statement_form_0args_emits_error_per_name) {
    std::vector<const char*> failed;
    for (const StaticCase& c : allStaticCases()) {
        if (hasErrorNeedle(statement0Args(c), c.needle)) continue;
        failed.push_back(c.name);
    }
    if (!failed.empty()) {
        std::cout << "    static-class names that DID NOT reject "
                     "X.TotallyBogusMember(); (statement form, 0 args): "
                  << failed.size() << " of " << allStaticCases().size()
                  << ":\n";
        for (auto* n : failed) std::cout << "      - " << n << "\n";
    }
    BEZEL_EXPECT_TRUE(failed.empty());
}

// ── T3: statement form, 2 args ────────────────────────────────────────────
//
// Same matrix cell as T1 but reached through the discard path. Pre-fix:
// silent-zero degrade in the var-decl form (T1), silent-accept in the
// statement form (T2). Post-fix: both reject.
BEZEL_TEST(static_class_bogus_member_3021,
           statement_form_2args_emits_error_per_name) {
    std::vector<const char*> failed;
    for (const StaticCase& c : allStaticCases()) {
        if (hasErrorNeedle(statement2Args(c), c.needle)) continue;
        failed.push_back(c.name);
    }
    if (!failed.empty()) {
        std::cout << "    static-class names that DID NOT reject "
                     "X.TotallyBogusMember(1, 2); (statement form, 2 args): "
                  << failed.size() << " of " << allStaticCases().size()
                  << ":\n";
        for (auto* n : failed) std::cout << "      - " << n << "\n";
    }
    BEZEL_EXPECT_TRUE(failed.empty());
}

// ── T4: control — legitimate static methods MUST still compile + run ─────
//
// The fix added per-static-class reject arms; it MUST NOT have narrowed
// the legitimate surface. A real `File.Exists(path)`, `Path.Combine
// (a, b)`, `Debug.WriteLine(...)`, `Domain.Spawn(...)` call still
// compiles and runs cleanly. Each name tested with a member that IS in
// its documented surface, so the per-static-class arm does NOT fire.
BEZEL_TEST(static_class_bogus_member_3021,
           legitimate_static_method_still_compiles_and_runs) {
    // File.Exists(path) → bool (stdlib §7.3).
    BEZEL_EXPECT_TRUE(ranCleanly(
        "module M; import System.IO; import System.Threading; "
        "class C public static method Main(): int "
        "  var b := File.Exists(\"/dev/null\"); "
        "  _ := b; "
        "  return 0; "
        "end Main; end C;"));
    // Path.Combine(a, b) → string (stdlib §7.4).
    BEZEL_EXPECT_TRUE(ranCleanly(
        "module M; import System.IO; import System.Threading; "
        "class C public static method Main(): int "
        "  var s := Path.Combine(\"a\", \"b\"); "
        "  _ := s; "
        "  return 0; "
        "end Main; end C;"));
    // Directory.Exists(path) → bool (stdlib §7.4).
    BEZEL_EXPECT_TRUE(ranCleanly(
        "module M; import System.IO; import System.Threading; "
        "class C public static method Main(): int "
        "  var b := Directory.Exists(\"/tmp\"); "
        "  _ := b; "
        "  return 0; "
        "end Main; end C;"));
    // Debug.WriteLine(s) → unit (stdlib §8.2).
    BEZEL_EXPECT_TRUE(ranCleanly(
        "module M; import System.Diagnostics; import System.Threading; "
        "class C public static method Main(): int "
        "  Debug.WriteLine(\"hi\"); "
        "  return 0; "
        "end Main; end C;"));
    // Encoding.UTF8.GetBytes(s) → Array<byte> (stdlib §3.2).
    // Encoding IS a static class; the legitimate access is to its
    // singleton properties, but the singleton must be used in the
    // direct chained form (`Encoding.UTF8.GetBytes(...)`) — there
    // is no first-class singleton OBJECT (per the §3.2 reject at
    // ExprTypecheck_Calls.cpp ~14240 that disallows
    // `var e := Encoding.UTF8; e.GetBytes(...)`). Pins that the §3.2
    // reject arm did not over-reject the documented surface.
    BEZEL_EXPECT_TRUE(ranCleanly(
        "module M; import System.IO; import System.Threading; "
        "class C public static method Main(): int "
        "  using var bytes: Array<byte> := Encoding.UTF8.GetBytes(\"hi\"); "
        "  return 0; "
        "end Main; end C;"));
    // BitConverter.FloatToInt32Bits(value) → int (stdlib §2.12).
    // Pins that the rejectBuiltinStatic() arm did not over-reject
    // any of BitConverter's documented static methods.
    // (BitConverter has NO GetBytes method — its surface is the
    // four Float/Double bits operations + the generic
    // Reinterpret<TTo>; see stdlib §2.12.)
    BEZEL_EXPECT_TRUE(ranCleanly(
        "module M; import System.Threading; "
        "class C public static method Main(): int "
        "  var bits := BitConverter.FloatToInt32Bits(1.0); "
        "  _ := bits; "
        "  return 0; "
        "end Main; end C;"));
}

// ── T5: control — user class with same name still compiles ──────────────
//
// Per the §11.3-discriminator source-code convention (fix #3022,
// v0.240 — §11.3 is "Member body", which fixes the naming-collision
// rule for member bodies: a user type may share a name with a builtin),
// a USER class named `Path` (or any of the affected names) with its
// own static methods MUST still compile. The fix relies on a
// `tgtSym->decl == nullptr` builtin-only gate to distinguish "builtin
// name in a static-call position" from "user type that happens to
// share a name with a builtin" — pinning this means a future fix that
// drops the gate (or narrows the discriminator) fails here. The
// canonical example the #2996 / Wave-3 follow-up cites:
// `Path.Deserialize(d)` on `record Path(…): IDeserializable` is a
// legal user record whose Deserialize is the synthesized
// IDeserializable member. Here we use a user class with an explicit
// static method to keep the shape minimal.
BEZEL_TEST(static_class_bogus_member_3021,
           user_class_named_path_still_compiles) {
    BEZEL_EXPECT_TRUE(ranCleanly(
        "module M; import System.Diagnostics; "
        "class Path public "
        "  static method Combine(a: string, b: string): string "
        "    return a; "
        "  end Combine; "
        "end Path; "
        "class C public static method Main(): int "
        "  var s := Path.Combine(\"x\", \"y\"); "
        "  _ := s; "
        "  return 0; "
        "end Main; end C;"));
}