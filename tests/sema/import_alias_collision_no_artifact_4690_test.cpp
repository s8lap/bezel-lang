// Spec-derived regression test for issue #4690 (rule-corpus `s2_2_0006`,
// spec §2.2, v0.307).
//
// THE GOVERNING RULE. Spec §2.2 "Aliases" / "Collisions" (v0.307): "An alias
// may not collide with another alias, an imported module's name segment in
// qualifier position, a type declared in the file's module, or an
// unambiguously imported public bare name."
//
// ROOT CAUSE. `Resolver::run` (src/sema/TypeResolution.cpp) gated its ENTIRE
// §2.2 alias-collision block behind `ctx_.hasImports()`
// (`!importedByModule_.empty()`), which is true only once some `import` has
// resolved to a LOADED DEPENDENCY ARTIFACT (`ModuleLoader::
// registerImportedType`). A file whose only imports are stdlib pseudo-
// modules (`System.Threading`, `System.IO`, ...) never registers one of
// those — those names resolve through the separate builtin-name tables in
// TypeResolution_Names.cpp / ExprTypecheck_Calls.cpp, not through a loaded
// artifact — so `hasImports()` was FALSE for such a file and the alias/alias
// and alias/local-declaration collision checks, which need only
// `src.imports` and the local module scope (NOT a loaded artifact), silently
// never ran.
//
// MEASURED on `bf7a7515` (this lane's base, `r19-c20`), with `build/bezelc`:
// the rule-corpus case (kAliasCollidesWithLocalType below, verbatim) compiled
// with rc=0 and zero diagnostics, despite declaring a class named exactly
// like its own import alias. The rule-corpus classifier's report line read:
//   KNOWN  s2_2_0006  reject  compiled, but the spec rejects this:
//
// THE FIX. Widen the gate to `ctx_.hasImports() || !src.imports.empty()` —
// i.e. run the block whenever the file wrote at least one `import`
// declaration, not only when one resolved to a loaded artifact. The
// `importedNameOrigin` loop inside already no-ops per-import when
// `ctx_.importedTypesOf(dotted)` is null, so this changes behavior ONLY for
// the alias-collision checks (the gap above) and stays byte-for-byte
// unchanged for a file with zero `import` declarations.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// True iff compilation failed with an ERROR diagnostic carrying `needle`.
bool rejectsWith(const std::string& src, const std::string& needle) {
    auto r = runAot(src);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

// True iff the program compiles, links, runs and exits with `expected`.
bool runs(int expected, const std::string& src) {
    auto r = runAot(src);
    return r.ran && r.exitCode == expected;
}

// §2.2 rule-corpus `s2_2_0006`, VERBATIM: the file's ONLY import is a stdlib
// pseudo-module alias (no real dependency artifact ever loads), and the
// alias collides with a type the file itself declares.
const char* kAliasCollidesWithLocalType =
    "module Conf; "
    "import System.Threading as Th; "
    "public class Th "
    "  public constructor() "
    "  end constructor; "
    "end Th;";

// A second collision shape the same gate silently skipped: two stdlib
// pseudo-module imports bound to the SAME alias. Neither import ever loads a
// real artifact, so pre-fix `hasImports()` was false here too.
const char* kAliasAliasCollisionStdlibOnly =
    "module Conf; "
    "import System.Threading as Th; "
    "import System.IO as Th; "
    "public class Program "
    "  public static method Main(): int return 0; end Main; "
    "end Program;";

}  // namespace

// §2.2, v0.307 — alias/local-declaration collision, stdlib-only import.
BEZEL_TEST(sema_import_alias_collision_no_artifact_4690,
           alias_collides_with_local_type_stdlib_only_import_rejected_4690) {
    BEZEL_EXPECT_TRUE(
        rejectsWith(kAliasCollidesWithLocalType, "collides with type"));
    // Pin the §2.2 citation so the row cannot be satisfied by an unrelated
    // "already declared"-style diagnostic that happens to mention `Th`.
    BEZEL_EXPECT_TRUE(rejectsWith(kAliasCollidesWithLocalType, "\xC2\xA7""2.2"));
}

// §2.2, v0.307 — alias/alias collision, BOTH imports stdlib-only (neither
// ever registers a loaded artifact, so `hasImports()` alone cannot gate it).
BEZEL_TEST(sema_import_alias_collision_no_artifact_4690,
           alias_alias_collision_stdlib_only_imports_rejected_4690) {
    BEZEL_EXPECT_TRUE(rejectsWith(kAliasAliasCollisionStdlibOnly,
                                   "collides with the same alias"));
}

// ACCEPT control — the false-reject hazard of widening the gate: a file with
// a stdlib-only import alias that does NOT collide with anything must still
// compile clean (the `importedNameOrigin`/ambiguous-bare-name machinery
// inside the widened block must keep no-oping for a module that never
// registers exported types).
BEZEL_TEST(sema_import_alias_collision_no_artifact_4690,
           nonclashing_stdlib_only_alias_still_accepted_4690) {
    BEZEL_EXPECT_TRUE(runs(
        42,
        "module Conf; "
        "import System.Threading as Th; "
        "public class Other "
        "  public static method Main(): int return 42; end Main; "
        "end Other;"));
}

// ACCEPT control — a file with NO imports at all must be entirely unaffected
// by the widened gate (the comment's "byte-for-byte unchanged" claim).
BEZEL_TEST(sema_import_alias_collision_no_artifact_4690,
           no_imports_at_all_unaffected_4690) {
    BEZEL_EXPECT_TRUE(runs(
        7,
        "module Conf; "
        "public class Program "
        "  public static method Main(): int return 7; end Main; "
        "end Program;"));
}
