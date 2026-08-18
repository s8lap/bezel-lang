// fix #3100 (spec §19.6 / §11.7) — cross-module exhaustiveness over a
// `closed` class hierarchy whose leaf set contains an `internal` class.
//
// WHAT §19.6 / §11.7 REQUIRE.  A `closed` class provides the strongest
// exhaustiveness guarantee (§19.6, last sentence, v0.249): "no subclass
// outside the declaring module can exist, so exhaustiveness holds across
// module boundaries".  §11.7 states the same: "the compiler can therefore
// prove exhaustiveness of `case` matches over `Shape` without requiring a
// `_` arm, even across module boundaries".
//
// THE DEFECT (soundness).  The cross-module guarantee depends on the
// COMPLETE leaf set being knowable from the importer.  The leaf set is
// read off `ctx_.symbols() ∪ ctx_.importedSymbols()` at the importer, but
// `internal` types are NOT exported (ModuleMetadataBuilder.cpp:2112,
// "§13.2 internal types not exported").  An `internal` leaf of a
// `public closed` hierarchy is invisible to the importer's leaf scan —
// so the importer's `case` proof ACCEPTS non-coverage of that leaf, and
// the runtime dispatches into it (the leaf's `new` is reachable via the
// declaring module's static factory, e.g. `Factory.Make()`), raising
// `MatchFailureException` (§21.7) at run time.  Per §2.8.3 rule 4, the
// runtime type identity must agree across artifacts; here the static
// proof is unsound because it relies on the exporter's complete leaf
// set without the leaf set actually being carried.
//
// THE FIX (§19.6 / §11.7).  Reject the shape at the declaration site so
// the dependent module does not produce an artifact whose cross-module
// guarantee is unsound.  The cross-module guarantee holds for a `closed`
// root only when every concrete leaf of its hierarchy is `public`, because
// only `public` leaves ride `isExportedType` (ModuleMetadataBuilder.cpp:41)
// into the importer's symbol table.  The dep does not compile, the
// importer never sees a closed root over a leaf set it cannot read, and
// the `caseArmsExhaustive` gate's `isClosedRoot` arm keeps its §19.6
// promise.
//
// TEST MATRIX:
//
//   T1  dep with `closed` root + one public sealed leaf + one INTERNAL
//       sealed leaf.  Compile MUST fail at the dep site with a diagnostic
//       naming `closed`, `public`, and `internal`.  Pre-fix: dep
//       compiles silently and the importer's `caseArmsExhaustive` proves
//       exhaustiveness over the invisible leaf — silent unsoundness.
//   T2  dep with `closed` root + internal ABSTRACT intermediate + public
//       sealed leaf under it.  MUST still compile — an `abstract`
//       intermediate has no instances (§11.7 / §12.5) and is invisible
//       only to instantiation, never to leaf-set computation; the
//       concrete leaf is public.  Pins that the fix does not over-reject
//       legitimate intermediate shapes.
//   T3  dep with `closed` root + all leaves public, importer covers only
//       one.  MUST compile and the importer MUST fire the §19.6 missing-
//       leaf warning.  The original #3167 positive path; pins that the
//       fix does not regress the legitimate missing-leaf path.
//   T4  dep with `closed` root + all leaves public AND covered.  MUST
//       compile and the importer MUST NOT warn.  Pins that the fix does
//       not over-warn and break the legitimate cross-module guarantee.
//
// Spec citations: §19.6, §11.7, §13.2, §2.8.3 rule 4.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"


namespace bezel::module {
namespace {

namespace fs = std::filesystem;

struct TempFile3100 {
    std::string path;
    explicit TempFile3100(const std::string& p) : path(p) {}
    ~TempFile3100() { std::remove(path.c_str()); }
};

void writeSource3100(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

struct CompileResult3100 {
    std::string stderr;
    bool depOk = false;
    bool importerOk = false;
};

// Compile `libSrc` to an object, compile `appSrc` against it (capturing
// stderr), return the captured stderr and per-stage status flags.  Either
// stage may legitimately fail (T1 / T2 expect the dep stage to fail;
// T3 / T4 expect both to succeed) — the test must read the flags and
// inspect the diagnostic text independently.
CompileResult3100 compileAndCheckStderr3100(const std::string& libSrc,
                                            const std::string& appSrc,
                                            const std::string& tag) {
    CompileResult3100 out;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz3100_" + tag + "_" + unique_stem("");
    TempFile3100 lSrc((dir / (stem + "_lib.bzl")).string());
    TempFile3100 lObj((dir / (stem + "_lib.o")).string());
    TempFile3100 aSrc((dir / (stem + "_app.bzl")).string());
    TempFile3100 aExe((dir / stem).string());
    TempFile3100 lErr((dir / (stem + "_lib.err")).string());
    TempFile3100 aErr((dir / (stem + "_app.err")).string());
    writeSource3100(lSrc.path, libSrc);
    writeSource3100(aSrc.path, appSrc);

    bezel::Driver driver;
    // Capture the dep's stderr separately so a decl-site diagnostic is
    // unambiguous (the importer's stderr is the only signal the §19.6
    // missing-leaf warning fires).
    std::fflush(stderr);
    int saved = ::dup(STDERR_FILENO);
    int sink = ::open(lErr.path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (sink >= 0) { ::dup2(sink, STDERR_FILENO); ::close(sink); }
    bezel::CompileResult rl = driver.compileObject(lSrc.path, lObj.path);
    std::fflush(stderr);
    if (saved >= 0) { ::dup2(saved, STDERR_FILENO); ::close(saved); }
    std::ifstream inL(lErr.path);
    out.stderr.assign((std::istreambuf_iterator<char>(inL)),
                      std::istreambuf_iterator<char>());
    if (!rl.success) return out;
    if (!std::ifstream(lObj.path).good()) return out;
    out.depOk = true;

    bezel::Dependency dep;
    dep.metadataPath = lObj.path;
    dep.objectPath = lObj.path;
    std::vector<bezel::Dependency> deps{dep};

    std::fflush(stderr);
    saved = ::dup(STDERR_FILENO);
    sink = ::open(aErr.path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (sink >= 0) { ::dup2(sink, STDERR_FILENO); ::close(sink); }
    bezel::CompileResult rm = bezel::test::compileFileForTest(
        driver, aSrc.path, aExe.path, deps);
    std::fflush(stderr);
    if (saved >= 0) { ::dup2(saved, STDERR_FILENO); ::close(saved); }

    std::ifstream in(aErr.path);
    out.stderr += "\n---importer---\n";
    out.stderr.append((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
    out.importerOk = rm.success;
    return out;
}

bool stderrHas(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

// ---------------------------------------------------------------------------
// T1 — the cross-module defect repro from the issue.  `ShapeLib3100`
// exports a `closed` hierarchy with one public sealed leaf + one
// INTERNAL sealed leaf.  The dep MUST fail to compile at the §19.6
// diagnostic, because the cross-module guarantee would be unsound.
// Pre-fix: dep compiles silently, importer's leaf set is `{Circle}`, the
// case proves exhaustive, runtime MatchFailureException.
// ---------------------------------------------------------------------------
const char* kInternalLeafLib3100 =
    "module ShapeLib3100; "
    "public closed class Shape end Shape; "
    "public sealed class Circle: Shape "
    "  public radius: int; "
    "  public constructor(r: int) radius := r; end constructor; "
    "end Circle; "
    "internal sealed class Hidden: Shape "
    "  public tag: int; "
    "  public constructor(t: int) tag := t; end constructor; "
    "end Hidden; ";

const char* kInternalLeafApp3100 =
    "module App3100; import ShapeLib3100; class App3100 "
    "  public static method Main(): int => 0; "
    "end App3100; ";

// ---------------------------------------------------------------------------
// T2 — `closed` root with one public concrete leaf under an INTERNAL
// abstract intermediate.  The intermediate has no instances (§11.7 /
// §12.5); the fix targets only concrete leaves, and the concrete leaf is
// public.  Dep MUST compile.
// ---------------------------------------------------------------------------
const char* kInternalAbstractLib3100 =
    "module ShapeLib3100; "
    "public closed class Shape end Shape; "
    "internal abstract class InternalIntermediate: Shape "
    "  public tag: int; "
    "end InternalIntermediate; "
    "public sealed class Circle: InternalIntermediate "
    "  public radius: int; "
    "  public constructor(r: int) radius := r; end constructor; "
    "end Circle; ";

const char* kInternalAbstractApp3100 =
    "module App3100; import ShapeLib3100; class App3100 "
    "  public static method Classify(s: Shape): int "
    "    var i := (case s of "
    "      var c: Circle => c.radius); "
    "    return i; "
    "  end Classify; "
    "  public static method Main(): int => 0; "
    "end App3100; ";

// ---------------------------------------------------------------------------
// T3 — positive control: closed hierarchy with ALL leaves public
// (the #3167 baseline), case covers only `Circle`.  MUST compile and
// the importer MUST fire the §19.6 missing-leaf warning.  Pins that the
// fix does not regress the legitimate missing-leaf path.
// ---------------------------------------------------------------------------
const char* kAllPublicLib3100 =
    "module ClosedLib3100; "
    "public closed class Shape end Shape; "
    "public sealed class Circle: Shape "
    "  public radius: int; "
    "  public constructor(r: int) radius := r; end constructor; "
    "end Circle; "
    "public sealed class Square: Shape "
    "  public side: int; "
    "  public constructor(s: int) side := s; end constructor; "
    "end Square; ";

const char* kMissingOneApp3100 =
    "module App3100; import ClosedLib3100; class App3100 "
    "  public static method Classify(s: Shape): int "
    "    var i := (case s of "
    "      var c: Circle => c.radius); "
    "    return i; "
    "  end Classify; "
    "  public static method Main(): int => 0; "
    "end App3100; ";

// ---------------------------------------------------------------------------
// T4 — positive control: closed hierarchy with ALL leaves public AND
// covered (the #3167 T2 positive).  MUST compile and MUST NOT warn.
// ---------------------------------------------------------------------------
const char* kBothCoveredApp3100 =
    "module App3100; import ClosedLib3100; class App3100 "
    "  public static method Classify(s: Shape): int "
    "    var i := (case s of "
    "      var c: Circle => c.radius, "
    "      var q: Square => q.side); "
    "    return i; "
    "  end Classify; "
    "  public static method Main(): int => 0; "
    "end App3100; ";

}  // namespace

// ---------------------------------------------------------------------------
// T1 — dep with closed root + internal sealed leaf MUST fail at the
// decl site, naming §19.6 / §11.7 and the leaf.  Pre-fix: dep compiles,
// importer proves unsound exhaustiveness, runtime trap.  Post-fix: dep
// fails with the diagnostic and the importer never sees the bad shape.
// ---------------------------------------------------------------------------
BEZEL_TEST(xmod_closed_hierarchy_internal_leaf_3100,
           closed_root_with_internal_leaf_rejected_spec_19_6) {
    CompileResult3100 r = compileAndCheckStderr3100(
        kInternalLeafLib3100, kInternalLeafApp3100, "internal");
    // The dep stage MUST reject — the cross-module guarantee would
    // otherwise be unsound.
    BEZEL_EXPECT_FALSE(r.depOk);
    // The diagnostic MUST name §19.6 and the `closed`/`public`/`internal`
    // key words so the developer can connect the rejection to the spec.
    BEZEL_EXPECT_TRUE(stderrHas(r.stderr, "closed"));
    BEZEL_EXPECT_TRUE(stderrHas(r.stderr, "public"));
    BEZEL_EXPECT_TRUE(stderrHas(r.stderr, "internal"));
    BEZEL_EXPECT_TRUE(stderrHas(r.stderr, "§19.6"));
    BEZEL_EXPECT_TRUE(stderrHas(r.stderr, "Hidden"));
    // The importer never runs (no object artifact to import).
    BEZEL_EXPECT_FALSE(r.importerOk);
}

// ---------------------------------------------------------------------------
// T2 — `closed` root with internal ABSTRACT intermediate and public
// concrete leaf under it.  MUST NOT be rejected — an abstract
// intermediate has no instances (§11.7 / §12.5) and is matched only
// through its concrete descendants, so the cross-module guarantee holds
// on the public concrete leaf.  Pins that the fix targets only
// concrete non-public leaves, not intermediate shapes.
// ---------------------------------------------------------------------------
BEZEL_TEST(xmod_closed_hierarchy_internal_leaf_3100,
           closed_root_with_internal_abstract_intermediate_ok_spec_19_6) {
    CompileResult3100 r = compileAndCheckStderr3100(
        kInternalAbstractLib3100, kInternalAbstractApp3100, "absinter");
    BEZEL_EXPECT_TRUE(r.depOk);
    BEZEL_EXPECT_TRUE(r.importerOk);
    // The dep's §19.6 diagnostic MUST NOT fire — the internal type is an
    // abstract intermediate (no instances, §11.7 / §12.5), not a concrete
    // leaf.  The fix targets only concrete non-public leaves; an
    // internal abstract is invisible to the importer only in the sense
    // that it has no instances, and the cross-module guarantee still
    // holds on the public concrete `Circle`.
    BEZEL_EXPECT_FALSE(stderrHas(r.stderr, "closed class"));
}

// ---------------------------------------------------------------------------
// T3 — cross-module control: closed hierarchy with ALL leaves public,
// one leaf covered.  The §19.6 missing-leaf warning must still fire
// (#3167 baseline).  Pins that the fix does not regress the legitimate
// missing-leaf path.
// ---------------------------------------------------------------------------
BEZEL_TEST(xmod_closed_hierarchy_internal_leaf_3100,
           imported_closed_all_public_missing_leaf_warns_spec_19_6) {
    CompileResult3100 r = compileAndCheckStderr3100(
        kAllPublicLib3100, kMissingOneApp3100, "miss");
    BEZEL_EXPECT_TRUE(r.depOk);
    BEZEL_EXPECT_TRUE(r.importerOk);
    BEZEL_EXPECT_TRUE(stderrHas(r.stderr, "non-exhaustive"));
    BEZEL_EXPECT_TRUE(stderrHas(r.stderr, "Square"));
}

// ---------------------------------------------------------------------------
// T4 — cross-module positive control: closed hierarchy with ALL leaves
// public AND covered.  Must NOT warn.  Pins that the fix does not
// over-warn and break the legitimate cross-module guarantee.
// ---------------------------------------------------------------------------
BEZEL_TEST(xmod_closed_hierarchy_internal_leaf_3100,
           imported_closed_all_public_covered_no_warn_spec_19_6) {
    CompileResult3100 r = compileAndCheckStderr3100(
        kAllPublicLib3100, kBothCoveredApp3100, "all");
    BEZEL_EXPECT_TRUE(r.depOk);
    BEZEL_EXPECT_TRUE(r.importerOk);
    BEZEL_EXPECT_FALSE(stderrHas(r.stderr, "non-exhaustive"));
    BEZEL_EXPECT_FALSE(stderrHas(r.stderr, "open"));
}

}  // namespace bezel::module