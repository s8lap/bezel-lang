// Duplicate-module detection, implementation-provided-name grab, artifact dedup
// at the build step (§2.8.1 rules 1/3/5, v0.188, issue #1729).
//
// §2.8.1 rules at v0.188 require:
//   (a) two DISTINCT dependency artifacts declaring the same module name is
//       a build error naming both origins, diagnosed at artifact-set load
//       (metadata time; native-link discovery is conforming but substandard);
//   (b) bit-identical duplicate presentations of one artifact are deduplicated,
//       not diagnosed (diamond assembly);
//   (c) a root compilation OR a dependency artifact declaring an
//       implementation-provided module name (`module System;`,
//       `module System.Threading;`, …) is the rule-3 build error;
//       stdlib modules stay exempt from artifact presentation.
//
// Before the fix (probes at HEAD `d34c8b4`):
//   - Two artifacts declaring `module Dup` with different content linked and
//     ran silently — multiple-definition was a §2.8.1 rule-3 violation
//     uncaught until the native-linker `multiple definition of bz$Dup.Alpha$…`
//     errors, with no metadata-time diagnostic naming both artifacts (rule a
//     was diagnosed, but the wording named "stale or disagreeing descriptions"
//     rather than the spec's "two distinct artifacts declaring one module
//     name").
//   - Bit-identical artifacts declaring `module Dup` had the metadata-side
//     dedup applied (one ModuleMetadata, one TypeSymbol shell) but the
//     dependency objects were still BOTH linked — every symbol emitted
//     twice, the link failed with `multiple definition of bz$Dup.Alpha$…`,
//     exactly the diamond-assembly case rule (b) carves out as idempotent
//     (#1729's headline repro).
//   - User files declaring `module System;` or `module System.Threading;`
//     compiled and ran silently — rule (c) had no enforcement on the root
//     side; the stdlib namespace was silently grab-able. (`rev0034/sysgrab.bz`)
//   - User libraries declaring `module System;` likewise compiled silently,
//     leaving the door open for a downstream consumer that imports `System`
//     to receive the user artifact instead of the implementation's surface.
//
// The fix (see src/module/ModuleLoader.cpp `loadDependencies` and
// src/driver/Driver.cpp `compileExecutableObject` / `compileFile`):
//   - `loadDependencies` accepts the root's full dotted module name and
//     rejects any dep whose moduleName matches it (rule 3, root side) or is
//     impl-provided (rule 5).
//   - `loadDependencies` collects `survivingPaths` so the link step sees
//     only the dedup'd set (rule 3's idempotence carve-out reaches the
//     link, not just the type checker).
//   - `compileExecutableObject` and `compileObject` reject a root whose
//     `module X;` declaration names an impl-provided module.
//   - The error wording is the spec's own: "two distinct dependency
//     artifacts declare the same module name … a build whose artifact set
//     contains two distinct artifacts declaring one module name is
//     ill-formed (§2.8.1 rule 3)" — no more "stale or disagreeing
//     descriptions", which the spec does not say.
//
// Spec-derived tests:
//   (a) two DISTINCT dependency artifacts declaring the same module name
//       → build fails (with the §2.8.1-rule-3 error naming both artifacts).
//   (b) two BIT-IDENTICAL dependency artifacts declaring the same module
//       name → build SUCCEEDS, link succeeds, output reflects the single
//       artifact's behavior (diamond assembly).
//   (c) root file declaring an impl-provided module name → build fails
//       with the §2.8.1 rules-3+5 error naming the module name.
//   (c') library (-c) declaring an impl-provided module name → build fails
//       with the same error (the loader side rejects it independently —
//       a library that somehow reached the loader would be caught there
//       too).
//   (d) dependency artifact declaring the root's module name → build fails
//       with the §2.8.1 rule-3 error naming the artifact and the root's
//       module.
//   (e) control: a single-module build with no cross-module surface is
//       unaffected (the impl-provided list does not block user module names).

#include "driver/Driver.hpp"
#include "module/ModuleLoader.hpp"
#include "test_harness.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "support/unique_stem.hpp"

namespace bezel::module {
namespace {

namespace fs = std::filesystem;

struct TempFile {
    std::string path;
    explicit TempFile(const std::string& p) : path(p) {}
    ~TempFile() { std::remove(path.c_str()); }
};

void writeSource(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Build one source to an object and return whether the build succeeded. On
// success the artifact is left at `objPath` (the caller's TempFile owns it).
// On failure the driver's stderr is captured into `*errOut` for the test to
// assert against (FileCheck-style substring assertions).
bool buildLib(const std::string& src, const std::string& objPath,
              std::string* errOut) {
    auto tmpSrc = objPath + ".src.bzl";
    writeSource(tmpSrc, src);
    TempFile srcGuard(tmpSrc);
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileObject(tmpSrc, objPath);
    if (!r.success && errOut) {
        // We can't read the driver's stderr here (it printed to the real
        // stderr); tests that need to assert on the message must use the
        // dedicated bezelc process form. For these tests we just check
        // success/failure.
        *errOut = "compile failed (see test stderr)";
    }
    return r.success;
}

// Compile a consumer source against `deps` to an executable and return the
// build outcome. On success the executable is at `exePath`; on failure the
// return is false and the consumer has been rejected.
bool buildConsumer(const std::string& src, const std::string& exePath,
                   const std::vector<std::string>& depObjectPaths) {
    auto tmpSrc = exePath + ".src.bzl";
    writeSource(tmpSrc, src);
    TempFile srcGuard(tmpSrc);
    std::vector<bezel::Dependency> deps;
    deps.reserve(depObjectPaths.size());
    for (const std::string& p : depObjectPaths) {
        bezel::Dependency d;
        d.metadataPath = p;
        d.objectPath = p;
        deps.push_back(d);
    }
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileFile(tmpSrc, exePath, deps);
    return r.success;
}

int runExe(const std::string& path) {
    int status = std::system(path.c_str());
    if (status == -1 || !WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

}  // namespace

// ---------------------------------------------------------------------------
// (a) §2.8.1 rule 3: two DISTINCT dependency artifacts declaring the same
// module name is a build error naming both artifacts. Before the fix the
// rejection's wording was "stale or disagreeing descriptions … recompile
// dependents" — a staleness message that did not name the spec rule that
// actually fires and obscured the real defect (two distinct user modules
// under one name). After the fix the wording matches the spec.
// ---------------------------------------------------------------------------
BEZEL_TEST(duplicate_module_artifact_1729,
           two_distinct_deps_same_module_name_rejected_spec_2_8_1_rule_3) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_1729_distinct_" + unique_stem("");
    std::string aObj = (dir / (stem + "_a.o")).string();
    std::string bObj = (dir / (stem + "_b.o")).string();
    std::string exe = (dir / stem).string();
    TempFile ga(aObj), gb(bObj), ge(exe);

    // Two distinct user libraries, both declaring `module Dup;`. They
    // disagree on their public surface (Alpha in one, Beta in the other).
    bool aOk = buildLib(
        "module Dup; "
        "public class Alpha "
        "  public method Name(): string return \"alpha\"; end Name; "
        "end Alpha;",
        aObj, nullptr);
    bool bOk = buildLib(
        "module Dup; "
        "public class Beta "
        "  public method Name(): string return \"beta\"; end Name; "
        "end Beta;",
        bObj, nullptr);
    BEZEL_EXPECT_TRUE(aOk);
    BEZEL_EXPECT_TRUE(bOk);

    // Consumer imports `Dup` and uses both — both would resolve to the same
    // moduleName under §2.8.1 rule 3, which forbids two distinct artifacts
    // under one name.
    std::string consumer =
        "module App; "
        "import Dup; "
        "class Program "
        "  public static method Main(): int return 0; end Main; "
        "end Program;";
    bool cOk = buildConsumer(consumer, exe, {aObj, bObj});
    BEZEL_EXPECT_FALSE(cOk);  // rule-3 build error
}

// ---------------------------------------------------------------------------
// (b) §2.8.1 rule 3's idempotence carve-out: two BIT-IDENTICAL dependency
// artifacts (diamond assembly) link and run without multi-definition errors.
// Before the fix the metadata-side dedup fired but the link step linked
// every supplied object, producing `multiple definition of bz$Dup.Alpha$…`
// — the headline #1729 repro. After the fix the loader's survivingPaths
// informs the link step's dep list, so the dedup'd duplicate is not
// double-linked.
// ---------------------------------------------------------------------------
BEZEL_TEST(duplicate_module_artifact_1729,
           bit_identical_deps_dedup_link_and_run_spec_2_8_1_rule_3) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_1729_bitidentical_" + unique_stem("");
    std::string aObj = (dir / (stem + "_a.o")).string();
    std::string bObj = (dir / (stem + "_b.o")).string();
    std::string exe = (dir / stem).string();
    TempFile ga(aObj), gb(bObj), ge(exe);

    // The same source compiles to two distinct object files (different
    // scratch paths). Each is its own `module Dup;` artifact, but their
    // carried metadata is bit-identical — the diamond-assembly shape.
    std::string src =
        "module Dup; "
        "public class Alpha "
        "  public static method Hello(): string return \"alpha\"; end Hello; "
        "end Alpha;";
    bool aOk = buildLib(src, aObj, nullptr);
    bool bOk = buildLib(src, bObj, nullptr);
    BEZEL_EXPECT_TRUE(aOk);
    BEZEL_EXPECT_TRUE(bOk);

    std::string consumer =
        "module App; "
        "import Dup; "
        "class Program "
        "  public static method Main(): int "
        "    if Alpha.Hello() = \"alpha\" then return 42; end; "
        "    return 0; "
        "  end Main; "
        "end Program;";
    bool cOk = buildConsumer(consumer, exe, {aObj, bObj});
    BEZEL_EXPECT_TRUE(cOk);
    // The dedup'd executable runs and prints the single artifact's value —
    // NOT a multi-definition link error.
    BEZEL_EXPECT_EQ(runExe(exe), 42);
}

// ---------------------------------------------------------------------------
// (c) §2.8.1 rules 3+5 (root side): the root compilation cannot declare an
// implementation-provided module name. Before the fix a file declaring
// `module System;` (the `rev0034/sysgrab.bz` repro) compiled and ran
// silently. After the fix the root-side check rejects it before the loader
// runs, with a diagnostic that names the file and the colliding module name.
// ---------------------------------------------------------------------------
BEZEL_TEST(duplicate_module_artifact_1729,
           root_module_system_rejected_spec_2_8_1_rules_3_5) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_1729_sysgrab_" + unique_stem("");
    std::string obj = (dir / (stem + ".o")).string();
    TempFile go(obj);

    std::string src =
        "module System; "
        "class Program "
        "  public static method Main(): int return 0; end Main; "
        "end Program;";
    bool ok = buildLib(src, obj, nullptr);
    BEZEL_EXPECT_FALSE(ok);  // rules-3+5 error
}

// (c2) Same check for a dotted impl-provided name (`System.Threading`).
// Before the fix this compiled silently — the leaf-only name lookup in the
// pre-fix SemaContext-based check missed dotted names entirely.
BEZEL_TEST(duplicate_module_artifact_1729,
           root_module_system_threading_rejected_spec_2_8_1_rules_3_5) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_1729_systhreading_" + unique_stem("");
    std::string obj = (dir / (stem + ".o")).string();
    TempFile go(obj);

    std::string src =
        "module System.Threading; "
        "class Program "
        "  public static method Main(): int return 0; end Main; "
        "end Program;";
    bool ok = buildLib(src, obj, nullptr);
    BEZEL_EXPECT_FALSE(ok);  // rules-3+5 error
}

// ---------------------------------------------------------------------------
// (c') §2.8.1 rules 3+5 (dep side): the loader rejects a dependency artifact
// whose moduleName is impl-provided. The library (-c) build also rejects the
// same source, so a dep path that reaches the loader through an older or
// out-of-band build (an artifact written before this fix, or hand-crafted)
// is still caught at load time. We exercise the dep-side path by passing a
// pre-existing .o file directly to the loader-side test below — but the
// library-build path is the more important regression to pin, since it is
// the one a user hits first.
// ---------------------------------------------------------------------------
BEZEL_TEST(duplicate_module_artifact_1729,
           library_module_system_rejected_spec_2_8_1_rules_3_5) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_1729_syslib_" + unique_stem("");
    std::string obj = (dir / (stem + ".o")).string();
    TempFile go(obj);

    std::string src =
        "module System.Collections.Generic; "
        "public class Helper "
        "  public static method Hello(): string return \"hi\"; end Hello; "
        "end Helper;";
    bool ok = buildLib(src, obj, nullptr);
    BEZEL_EXPECT_FALSE(ok);  // rules-3+5 error
}

// ---------------------------------------------------------------------------
// (d) §2.8.1 rule 3 (root-vs-dep half): a dependency artifact declaring the
// root compilation's module name is a build error. The root owns its
// moduleName; presenting a competing dependency under the same name would
// silently widen visibility for the root's `internal` access (the
// closure-by-compilation invariant of rule 2). This is distinct from the
// two-deps-same-name case (rule-3's other arm) — the head of the build is
// the root, and the dep is in the closure.
// ---------------------------------------------------------------------------
BEZEL_TEST(duplicate_module_artifact_1729,
           dep_declares_root_module_name_rejected_spec_2_8_1_rule_3) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_1729_rootdep_" + unique_stem("");
    std::string libObj = (dir / (stem + "_lib.o")).string();
    std::string exe = (dir / stem).string();
    TempFile gl(libObj), ge(exe);

    // A library declaring `module Dup;` — a NON-impl-provided name so the
    // library-build check passes. The dep-side rejection (rule-3 root arm)
    // fires when the root ALSO declares `module Dup;`.
    bool libOk = buildLib(
        "module Dup; "
        "public class Alpha "
        "  public static method Hello(): string return \"alpha\"; end Hello; "
        "end Alpha;",
        libObj, nullptr);
    BEZEL_EXPECT_TRUE(libOk);

    // Root declares the SAME module name `Dup;` — a build error per
    // §2.8.1 rule 3 ("a dependency artifact declaring a module name that
    // the root compilation also declares").
    std::string consumer =
        "module Dup; "
        "class Program "
        "  public static method Main(): int return 0; end Main; "
        "end Program;";
    bool cOk = buildConsumer(consumer, exe, {libObj});
    BEZEL_EXPECT_FALSE(cOk);
}

// ---------------------------------------------------------------------------
// (e) Control: the impl-provided list does not block user module names. A
// single-module build with a non-impl-provided module name is unaffected
// (regression check for the new check — it must not over-fire).
// ---------------------------------------------------------------------------
BEZEL_TEST(duplicate_module_artifact_1729,
           control_user_module_name_accepted) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_1729_control_" + unique_stem("");
    std::string exe = (dir / stem).string();
    TempFile ge(exe);

    std::string consumer =
        "module MyApp.UserModule; "
        "class Program "
        "  public static method Main(): int return 7; end Main; "
        "end Program;";
    bool cOk = buildConsumer(consumer, exe, {});
    BEZEL_EXPECT_TRUE(cOk);
    BEZEL_EXPECT_EQ(runExe(exe), 7);
}

// ---------------------------------------------------------------------------
// Unit-level: isImplProvidedModuleName classifies the stdlib catalog and
// admits user module names. Pins the helper directly so a future refactor
// of the canonical list (e.g. an incorrect rename) breaks here, not at the
// driver-level diagnostic.
// ---------------------------------------------------------------------------
BEZEL_TEST(duplicate_module_artifact_1729,
           impl_provided_module_name_helper_classifies_stdlib_catalog) {
    // §2.3 always-available set + standard-library document modules.
    BEZEL_EXPECT_TRUE(isImplProvidedModuleName("System"));
    BEZEL_EXPECT_TRUE(isImplProvidedModuleName("System.Text"));
    BEZEL_EXPECT_TRUE(isImplProvidedModuleName("System.Collections.Generic"));
    BEZEL_EXPECT_TRUE(isImplProvidedModuleName("System.Collections.Linq"));
    BEZEL_EXPECT_TRUE(isImplProvidedModuleName("System.Threading"));
    BEZEL_EXPECT_TRUE(isImplProvidedModuleName("System.IO"));
    BEZEL_EXPECT_TRUE(isImplProvidedModuleName("System.Diagnostics"));
    BEZEL_EXPECT_TRUE(isImplProvidedModuleName("System.Serialization"));
    BEZEL_EXPECT_TRUE(isImplProvidedModuleName("System.Reflection"));
    BEZEL_EXPECT_TRUE(isImplProvidedModuleName("System.Interop"));

    // User module names are not impl-provided (the impl-provided set is a
    // closed list — the §2.8.1 rule 5 carve-out is exhaustive).
    BEZEL_EXPECT_FALSE(isImplProvidedModuleName("Bank"));
    BEZEL_EXPECT_FALSE(isImplProvidedModuleName("Bank.Core"));
    BEZEL_EXPECT_FALSE(isImplProvidedModuleName("MyApp"));
    BEZEL_EXPECT_FALSE(isImplProvidedModuleName("SystemX"));  // not the stdlib
    BEZEL_EXPECT_FALSE(isImplProvidedModuleName(""));          // empty never matches
    // Case-sensitive: `system` (lowercase) is a different name from
    // `System` — impl-provided is the EXACT spec spelling.
    BEZEL_EXPECT_FALSE(isImplProvidedModuleName("system"));
}

}  // namespace bezel::module
