// fix #3167 (spec §19.6 / §11.7, v0.252) — cross-module exhaustiveness over a
// `closed` class hierarchy.
//
// WHAT §19.6 / §11.7 REQUIRE.  The non-exhaustiveness check over a `case`
// scrutinee whose static type is a `closed` class walks the *closed set* —
// the root and every transitive concrete subclass (§11.7 forbids subclassing
// outside the declaring module, so the set is closed cross-module). An
// expression-position `case` whose arms do not name every concrete leaf
// emits the §19.6 non-exhaustive warning naming the missing leaf type(s).
//
// THE DEFECT was a silent-accept regression.  The descendant census walked
// `ctx_.symbols()` only, which by construction excludes every imported
// symbol (SemaContext.hpp:225).  An imported `closed` hierarchy therefore
// had an EMPTY leaf set: the warn-missing path was unreachable, and a
// `case` over an imported closed class accepted non-coverage without
// complaint.  Pinning the regression required reading the importer's
// stderr (the diagnostic text is the only signal — the compile SUCCEEDS).
//
// TEST MATRIX (module A exports a closed hierarchy with two concrete
// sealed leaves; module B discriminates over it in expression position):
//   T1  imported closed class, ONE leaf covered — §19.6 warning naming
//       the other leaf, and compile still succeeds (a warning is not an
//       error).
//   T2  imported closed class, BOTH leaves covered — no warning.
//   T3  local closed class, ONE leaf covered — same shape, the pre-fix
//       pass: must keep warning. Differential control for the fix.
//   T4  imported closed class whose hierarchy is OPEN (a non-sealed leaf
//       outside the importer) — the §11.7 "open" warning fires, NOT a
//       "missing leaf" warning. Pins that the imported-symbol walk does
//       not also wrongly grant a closed set on an open hierarchy.
//
// Spec citations: §19.6, §11.7.

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

namespace bezel::module {
namespace {

namespace fs = std::filesystem;

struct TempFile3167 {
    std::string path;
    explicit TempFile3167(const std::string& p) : path(p) {}
    ~TempFile3167() { std::remove(path.c_str()); }
};

void writeSource3167(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Compile `libSrc` to an object, compile `appSrc` against it (capturing
// stderr), return the captured stderr and a status flag.  The actual
// compile IS expected to succeed in every arm — the §19.6 warning is a
// warning, not an error.  `depOk` / `importerOk` separate the two
// compile stages so a hard driver failure is NOT indistinguishable
// from a silenced warning (the only signal of a silenced warning is
// the missing diagnostic text in stderr).
struct CompileResult3167 {
    std::string stderr;
    bool depOk = false;        // did the lib's compileObject succeed?
    bool importerOk = false;    // did the importer's compileFile succeed?
};

CompileResult3167 compileAndCheckStderr3167(const std::string& libSrc,
                                            const std::string& appSrc,
                                            const std::string& tag) {
    CompileResult3167 out;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz3167_" + tag + "_" + std::to_string(std::rand());
    TempFile3167 lSrc((dir / (stem + "_lib.bzl")).string());
    TempFile3167 lObj((dir / (stem + "_lib.o")).string());
    TempFile3167 aSrc((dir / (stem + "_app.bzl")).string());
    TempFile3167 aExe((dir / stem).string());
    TempFile3167 aErr((dir / (stem + ".err")).string());
    writeSource3167(lSrc.path, libSrc);
    writeSource3167(aSrc.path, appSrc);

    bezel::Driver driver;
    bezel::CompileResult rl = driver.compileObject(lSrc.path, lObj.path);
    if (!rl.success) return out;
    if (!std::ifstream(lObj.path).good()) return out;
    out.depOk = true;

    bezel::Dependency dep;
    dep.metadataPath = lObj.path;
    dep.objectPath = lObj.path;
    std::vector<bezel::Dependency> deps{dep};

    // Redirect stderr to a file so the importer's diagnostic text is the
    // only signal.  Without the fix, the importer's compile SUCCEEDS and
    // the warning is silently dropped — no way to tell from the exit code
    // alone.
    std::fflush(stderr);
    int saved = ::dup(STDERR_FILENO);
    int sink = ::open(aErr.path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (sink >= 0) { ::dup2(sink, STDERR_FILENO); ::close(sink); }
    bezel::CompileResult rm = bezel::test::compileFileForTest(
        driver, aSrc.path, aExe.path, deps);
    std::fflush(stderr);
    if (saved >= 0) { ::dup2(saved, STDERR_FILENO); ::close(saved); }

    std::ifstream in(aErr.path);
    out.stderr.assign((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
    out.importerOk = rm.success;
    return out;
}

// Text predicates — keep the "needle" set conservative so a wording tweak
// that does not change the §19.6 verdict still passes.
bool stderrHas(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

// ---------------------------------------------------------------------------
// The dependency: a closed hierarchy with two concrete sealed leaves.
// A `closed` class is the §11.7 closed-set guarantee — no subclass outside
// the declaring module is admitted, so the leaf set is closed across the
// cross-module boundary.
// ---------------------------------------------------------------------------
const char* kClosedLib3167 =
    "module ClosedLib3167; "
    "public closed class Shape end Shape; "
    "public sealed class Circle: Shape "
    "  public radius: int; "
    "  public constructor(r: int) radius := r; end constructor; "
    "end Circle; "
    "public sealed class Square: Shape "
    "  public side: int; "
    "  public constructor(s: int) side := s; end constructor; "
    "end Square; ";

// T1 — imported closed class, ONE leaf covered.  Must fire the §19.6
// "missing leaf" warning naming the other leaf, AND the compile must
// still succeed.  Pre-fix: silent accept.
const char* kMissingOneApp3167 =
    "module App3167; import ClosedLib3167; class App3167 "
    "  public static method Classify(s: Shape): int "
    "    var i := (case s of "
    "      var c: Circle => c.radius); "
    "    return i; "
    "  end Classify; "
    "  public static method Main(): int => 0; "
    "end App3167; ";

// T2 — imported closed class, BOTH leaves covered.  Must NOT warn.
const char* kBothCoveredApp3167 =
    "module App3167; import ClosedLib3167; class App3167 "
    "  public static method Classify(s: Shape): int "
    "    var i := (case s of "
    "      var c: Circle => c.radius, "
    "      var q: Square => q.side); "
    "    return i; "
    "  end Classify; "
    "  public static method Main(): int => 0; "
    "end App3167; ";

// T3 - LOCAL closed class, ONE leaf covered. Differential control:
// the pre-fix pass warns here (the existing sema_case_exhaust test pins
// the local exhaustiveness). After the fix the importer path and the
// local path agree, so this arm must still warn. Local case puts the
// lib and app in one module - uses `compileLocalAndCheckStderr3167`
// below instead of the cross-module helper.
const char* kLocalMissingOneApp3167 =
    "module App3167; "
    "public closed class Shape end Shape; "
    "public sealed class Circle: Shape "
    "  public radius: int; "
    "  public constructor(r: int) radius := r; end constructor; "
    "end Circle; "
    "public sealed class Square: Shape "
    "  public side: int; "
    "  public constructor(s: int) side := s; end constructor; "
    "end Square; "
    "class App3167 "
    "  public static method Classify(s: Shape): int "
    "    var i := (case s of "
    "      var c: Circle => c.radius); "
    "    return i; "
    "  end Classify; "
    "  public static method Main(): int => 0; "
    "end App3167; ";

// Local-only compile: dumps the importer's stderr. The local case
// has no dep to compile - the lib and app are one module file. Returns
// a CompileResult3167 with depOk=true and importerOk=true iff the
// module compiled.
CompileResult3167 compileLocalAndCheckStderr3167(const std::string& appSrc,
                                                const std::string& tag) {
    CompileResult3167 out;
    out.depOk = true;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz3167L_" + tag + "_" + std::to_string(std::rand());
    TempFile3167 aSrc((dir / (stem + ".bzl")).string());
    TempFile3167 aExe((dir / stem).string());
    TempFile3167 aErr((dir / (stem + ".err")).string());
    writeSource3167(aSrc.path, appSrc);

    bezel::Driver driver;
    std::fflush(stderr);
    int saved = ::dup(STDERR_FILENO);
    int sink = ::open(aErr.path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (sink >= 0) { ::dup2(sink, STDERR_FILENO); ::close(sink); }
    bezel::CompileResult rm = bezel::test::compileFileForTest(
        driver, aSrc.path, aExe.path);
    std::fflush(stderr);
    if (saved >= 0) { ::dup2(saved, STDERR_FILENO); ::close(saved); }

    std::ifstream in(aErr.path);
    out.stderr.assign((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
    out.importerOk = rm.success;
    return out;
}

// T4 - imported ABSTRACT (not `closed`) hierarchy with a non-sealed
// concrete subclass. Per §19.6/§11.7 the closed-set computation needs
// the imported leaves; if the walk wrong granted a closed set, the
// §11.7 "open" warning would NOT fire here. The base is `abstract`
// (not `closed`), so the §11.7 cross-module guarantee does NOT
// apply; an `abstract` base can admit subclasses in the same module,
// but a non-sealed concrete leaf outside the importer's reach leaves
// the set OPEN. Pins that walking `importedSymbols()` does not also
// wrongly grant a closed set on an open hierarchy.
const char* kOpenLib3167 =
    "module OpenLib3167; "
    "public abstract class Shape end Shape; "
    "public class OpenLeaf: Shape "
    "  public v: int; "
    "  public constructor(x: int) v := x; end constructor; "
    "end OpenLeaf; ";
const char* kOpenApp3167 =
    "module App3167; import OpenLib3167; class App3167 "
    "  public static method Classify(s: Shape): int "
    "    var i := (case s of "
    "      var o: OpenLeaf => o.v); "
    "    return i; "
    "  end Classify; "
    "  public static method Main(): int => 0; "
    "end App3167; ";

}  // namespace

// ---------------------------------------------------------------------------
// T1 — pre-fix: silent accept.  post-fix: §19.6 warning naming `Square`.
// ---------------------------------------------------------------------------
BEZEL_TEST(xmod_closed_hierarchy_exhaustive_3167,
           imported_closed_class_missing_leaf_warns_spec_19_6) {
    CompileResult3167 r = compileAndCheckStderr3167(
        kClosedLib3167, kMissingOneApp3167, "miss");
    // The test runner needs a hard failure when the importer itself blew
    // up (e.g. an unrelated syntax/resolve error) — otherwise a passing
    // "no warning" verdict would be indistinguishable from a silenced
    // warning.  A driver compile failure is fatal to the test.
    BEZEL_EXPECT_TRUE(r.depOk);
    BEZEL_EXPECT_TRUE(r.importerOk);
    BEZEL_EXPECT_TRUE(stderrHas(r.stderr, "non-exhaustive"));
    BEZEL_EXPECT_TRUE(stderrHas(r.stderr, "Square"));
    // The OTHER covered leaf must NOT be named in the missing-leaf list.
    BEZEL_EXPECT_FALSE(stderrHas(r.stderr, "missing leaf type(s): Circle"));
}

// ---------------------------------------------------------------------------
// T2 — both leaves covered, no warning.  Pins the positive case so a
// future "always warn" over-correction would be caught.
// ---------------------------------------------------------------------------
BEZEL_TEST(xmod_closed_hierarchy_exhaustive_3167,
           imported_closed_class_all_leaves_covered_no_warn_spec_19_6) {
    CompileResult3167 r = compileAndCheckStderr3167(
        kClosedLib3167, kBothCoveredApp3167, "all");
    BEZEL_EXPECT_TRUE(r.depOk);
    BEZEL_EXPECT_TRUE(r.importerOk);
    BEZEL_EXPECT_FALSE(stderrHas(r.stderr, "non-exhaustive"));
    BEZEL_EXPECT_FALSE(stderrHas(r.stderr, "open"));
}

// ---------------------------------------------------------------------------
// T3 - LOCAL closed class, ONE leaf covered. Differential control: the
// pre-fix pass warns here (the existing sema_case_exhaust test pins the
// local exhaustiveness). After the fix the importer path and the local
// path agree, so this arm must still warn (and the warning must name
// the uncovered leaf).
// ---------------------------------------------------------------------------
BEZEL_TEST(xmod_closed_hierarchy_exhaustive_3167,
           local_closed_class_missing_leaf_warns_spec_19_6) {
    CompileResult3167 r = compileLocalAndCheckStderr3167(
        kLocalMissingOneApp3167, "local");
    BEZEL_EXPECT_TRUE(r.depOk);
    BEZEL_EXPECT_TRUE(r.importerOk);
    BEZEL_EXPECT_TRUE(stderrHas(r.stderr, "non-exhaustive"));
    BEZEL_EXPECT_TRUE(stderrHas(r.stderr, "Square"));
}
// ---------------------------------------------------------------------------
BEZEL_TEST(xmod_closed_hierarchy_exhaustive_3167,
           imported_open_hierarchy_warns_open_not_missing_spec_11_7) {
    CompileResult3167 r = compileAndCheckStderr3167(
        kOpenLib3167, kOpenApp3167, "open");
    BEZEL_EXPECT_TRUE(r.depOk);
    BEZEL_EXPECT_TRUE(r.importerOk);
    // The §11.7 "open" warning fires here (the base is `abstract`, not
    // `closed`; a non-sealed concrete leaf leaves the set open). The
    // diagnostic text contains BOTH "non-exhaustive" and "open" — the
    // §19.6 wrapper prints the open verdict via the same template. The
    // "missing leaf" wording is what the closed-set branch would use;
    // its absence is the signal that the open verdict is in play, not
    // a missing-leaf report.
    BEZEL_EXPECT_TRUE(stderrHas(r.stderr, "is open"));
    BEZEL_EXPECT_FALSE(stderrHas(r.stderr, "missing leaf type(s):"));
    BEZEL_EXPECT_FALSE(stderrHas(r.stderr, "missing leaf"));
}

}  // namespace bezel::module
