// Spec-derived regression test for issue #5039 — an `override` of a generic
// method that OMITS the inherited `where` clause dispatched through the WRONG
// branch when called cross-module through a base-typed reference.
//
// `var g: Base := new Derived(); return g.EqV(5, 5);` gave **92** cross-module
// (`a.Equals(b)` treated as false); the correct answer, matching the
// same-module twin, is **91**.
//
// spec §14.2 "Constraints on overrides" (v0.307): "An `override` of a generic
// method ... **inherits the overridden ... declaration's constraints**.
// Omitting the `where` clauses is the recommended spelling and means exactly
// the inherited set."
//
// ── ROOT CAUSE (MEASURED) ───────────────────────────────────────────────────
//
// `Resolver::checkOverrideSignatureMatch` (src/sema/TypeResolution_Checks.cpp)
// only ever *compared* an override's omitted `where` clause against the
// inherited one (`if (!dc) continue;` — "nothing to compare, always legal")
// and never copied the inherited `GenericParamConstraint`s onto the
// override's OWN `GenericParamSymbol`. `Derived::EqV<T>`'s own constraint
// list stayed empty.
//
// `Derived.EqV` is `public`, generic, bound (once correctly inherited) only by
// the synthesized builtin interface `IEquatable<T>` — exactly the #5009/#5035
// shape that ships as an i32-PLACEHOLDER body for cross-module linkage
// (fix-forward 4's "sound at i32" set), rather than shipping source. Virtual
// dispatch from `App` through the `Base`-typed reference calls into that
// placeholder body. Its `a.Equals(b)` lowering asks
// `genericParamSoundAtI32ForPlaceholder` (LlvmBackend_Exprs.cpp) whether the
// CURRENT method's own type parameter `T` is bound only by a synthesized
// builtin interface — reading `currentMethod->genericParams[i]->constraints`.
// With that list empty (the bug), the answer was "no" (`sawTypeBound` never
// set), so the fast i32 substitution never fired and the call fell through to
// pointer-based interface dispatch on an i32 VALUE receiver — the same
// null-condition-reads-as-false failure #5035 pinned, just reached through a
// different route (an empty constraint list instead of a genuinely
// unconstrained `T`).
//
// `Base.EqV` itself (never overridden) already dispatched correctly (61) —
// its OWN `where T: IEquatable<T>` clause was collected directly by
// `SymbolCollection::collectGenericParams` at declaration time. Only the
// OVERRIDE's inherited-but-omitted clause was lost.
//
// ── THE FIX ──────────────────────────────────────────────────────────────
//
// `checkOverrideSignatureMatch`'s per-generic-parameter loop, in the
// `!dc` ("override omits the clause") arm: materialize the base's
// constraints positionally onto `derived`'s own `GenericParamSymbol`, from
// whichever side is available (`bDecl`'s AST clause for a same-module base,
// `ctx_.importedGenericConstraints(base)` for an imported one) — the same
// two sources the comparison arm already reads just below.
//
// (`Base`/`Derived` are declared TOGETHER in the dependency module, so the
// override check itself runs same-module, through the `bDecl` branch; the
// cross-module element is in how `App` DISPATCHES into `Derived`'s
// placeholder-compiled body — see the mechanism note above.)
//
// ── WHAT EACH ROW PINS ───────────────────────────────────────────────────
//
//   T1 ★ THE REPRO. Cross-module: `g.EqV(5, 5)` through a `Base`-typed
//        reference -> 91 (pre-fix: 92).
//   T2 ★ SAME-MODULE CONTROL: the identical program in one module -> 91 on
//        both sides of the fix (documents the bug was cross-module-only,
//        and that the fix does not perturb the already-correct path).
//   T3   the false branch is still reachable cross-module:
//        `g.EqV(5, 6)` -> 92 on both sides (the fix does not turn the
//        override into an always-true stub).
//
// Every row asserts the exact exit code (91/92 both fit the 8-bit exit-code
// range, so no truncation risk — CLAUDE.md's "8-bit exit truncation" trap).

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

namespace fs = std::filesystem;

struct TempFile5039 {
    std::string path;
    explicit TempFile5039(std::string p) : path(std::move(p)) {}
    ~TempFile5039() { std::remove(path.c_str()); }
};

void writeSource5039(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Build the dependency (`libSrc`), then compile+link an importer (`mainSrc`)
// against it and RUN it. Returns the exit code, or -2 when the dependency
// toolchain is unavailable (skip the row), -1 on a compile/link/run failure.
// Goes through `compileFileForTest`/`runProgramForTest` (#4532) so
// `gate.sh --full`'s generated-code ASan sweep reaches this hand-rolled
// two-module runner too.
int buildImportAndRun5039(const std::string& libSrc,
                          const std::string& mainSrc) {
    auto dir = fs::temp_directory_path();
    std::string id = unique_stem("bz5039r_");
    TempFile5039 libSrcF((dir / (id + "_lib.bzl")).string());
    TempFile5039 libObjF((dir / (id + "_lib.o")).string());
    TempFile5039 mainSrcF((dir / (id + "_main.bzl")).string());
    TempFile5039 mainExeF((dir / id).string());
    writeSource5039(libSrcF.path, libSrc);
    writeSource5039(mainSrcF.path, mainSrc);

    bezel::Driver driver;
    if (!driver.compileObject(libSrcF.path, libObjF.path).success) return -2;
    if (!std::ifstream(libObjF.path).good()) return -2;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = libObjF.path;
    d.objectPath = libObjF.path;
    deps.push_back(d);

    if (!bezel::test::compileFileForTest(driver, mainSrcF.path, mainExeF.path,
                                         deps).success)
        return -1;
    int status = bezel::test::runProgramForTest(mainExeF.path);
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// The one-module twin — no dependency anywhere.
int buildLocalAndRun5039(const std::string& src) {
    auto dir = fs::temp_directory_path();
    std::string id = unique_stem("bz5039l_");
    TempFile5039 srcF((dir / (id + ".bzl")).string());
    TempFile5039 exeF((dir / id).string());
    writeSource5039(srcF.path, src);
    bezel::Driver driver;
    if (!bezel::test::compileFileForTest(driver, srcF.path, exeF.path).success)
        return -1;
    int status = bezel::test::runProgramForTest(exeF.path);
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// ---------------------------------------------------------------------------
// The dependency: `Base5039` declares `EqV5039<T>` bound by `IEquatable<T>`;
// `Derived5039` overrides it WITHOUT restating the `where` clause — the
// omitted-clause spelling §14.2 says means "exactly the inherited set".
// ---------------------------------------------------------------------------
const char* kDeclsBody5039 =
    "public class Base5039 "
    "  public constructor() end constructor; "
    "  public virtual method EqV5039<T>(a: T, b: T): int where T: "
    "IEquatable<T> "
    "    if a.Equals(b) then return 61; end if; "
    "    return 62; "
    "  end EqV5039; "
    "end Base5039; "
    "public class Derived5039 : Base5039 "
    "  public constructor() end constructor; "
    "  public override method EqV5039<T>(a: T, b: T): int "
    "    if a.Equals(b) then return 91; end if; "
    "    return 92; "
    "  end EqV5039; "
    "end Derived5039; ";

std::string dep5039() { return std::string("module Dep5039; ") + kDeclsBody5039; }

std::string app5039(const std::string& tag, const std::string& mainBody) {
    return "module App5039" + tag + "; import Dep5039; " +
           "class C5039 public static method Main(): int " + mainBody +
           " end Main; end C5039;";
}

std::string local5039(const std::string& tag, const std::string& mainBody) {
    return "module Loc5039" + tag + "; " + kDeclsBody5039 +
           "class C5039 public static method Main(): int " + mainBody +
           " end Main; end C5039;";
}

const char* kEqualBody5039 =
    "var g: Base5039 := new Derived5039(); return g.EqV5039(5, 5);";
const char* kUnequalBody5039 =
    "var g: Base5039 := new Derived5039(); return g.EqV5039(5, 6);";

}  // namespace

// ===========================================================================
// T1 — ★ THE REPRO. Cross-module virtual dispatch through the `Base5039`-
// typed reference into `Derived5039`'s override, `a.Equals(b)` TRUE
// (5 == 5): correct answer 91. Pre-fix: 92 (the override's own generic
// parameter carried no constraints, so the placeholder body's `if` silently
// took the else arm regardless of the arguments).
// // per §14.2 "Constraints on overrides" / stdlib §2.9 IEquatable<T>, v0.307
// ===========================================================================
BEZEL_TEST(override_omitted_where_clause_xmod_5039,
           equal_arguments_cross_module_returns_91) {
    int rc = buildImportAndRun5039(dep5039(), app5039("T1", kEqualBody5039));
    if (rc == -2) return;  // dependency toolchain unavailable
    BEZEL_EXPECT_EQ(rc, 91);
}

// ===========================================================================
// T2 — ★ SAME-MODULE CONTROL: the identical program compiled as ONE module
// gives 91 on both sides of the fix (matches the issue's own note that the
// bug was cross-module-only). Guards the fix does not perturb the
// already-correct in-module path.
// // per §14.2, v0.307
// ===========================================================================
BEZEL_TEST(override_omitted_where_clause_xmod_5039,
           equal_arguments_same_module_returns_91) {
    BEZEL_EXPECT_EQ(buildLocalAndRun5039(local5039("T2", kEqualBody5039)), 91);
}

// ===========================================================================
// T3 — the false branch is still reachable cross-module with genuinely
// unequal arguments: `EqV5039(5, 6)` -> 92 on BOTH sides of the fix (this
// row does not discriminate the bug; it documents the fix did not turn the
// override into an always-true stub).
// // per §14.2 / stdlib §2.9, v0.307
// ===========================================================================
BEZEL_TEST(override_omitted_where_clause_xmod_5039,
           unequal_arguments_cross_module_returns_92) {
    int rc = buildImportAndRun5039(dep5039(), app5039("T3", kUnequalBody5039));
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 92);
}

}  // namespace bezel::module
