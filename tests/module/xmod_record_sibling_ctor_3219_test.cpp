// fix #3219 (§22.15 "Members classified by rule" + "Pure constructors", v0.254) —
// an imported RECORD's `pure` ctor that names a SIBLING type was being
// dropped from `PureChecker::findUserCtorByArity`'s candidate walk, because
// the walk read `ConstructorSymbol::decl` (an `ast::ConstructorDecl*`) and
// `ModuleLoader::parseAndAttachCtorBody` leaves `decl` null for an imported
// ctor whose re-parse over a SINGLE-type synthetic source fails to resolve
// (#2290 bug 2 / #710). The drop produced a §22.15 FALSE REJECT for a body
// record whose `pure` ctor names a sibling — the ctor is `pure` but the walk
// misses it, so `recordConstructionIsPure` returns false and §22.15 fires.
//
// §22.15 (v0.254), verbatim: "record construction `new R(args)` is admitted in
// a pure body iff the invoked constructor is compiler-synthesized-pure (a
// POSITIONAL record — class-identity check, not arity-based) OR a user-declared
// pure constructor with matching arity (body records have no synthesized ctor)".
//
// WHAT THE DEFECT WAS, measured on HEAD (`226aca1`) before this fix, with a
// two-module `bezelc -c lib.bzl -o lib.o` + `bezelc --import lib.o app.bzl`
// AOT pair (the strict oracle):
//
//   module Lib3219;
//   public record Sib
//     public readonly property S: int;
//     public pure constructor(s: int) this.S := s; end constructor;
//   end Sib;
//   public record BodyRec2
//     public readonly property X: int;
//     public pure constructor(s: Sib)            -- names SIBLING Sib
//       this.X := s.S;
//     end constructor;
//   end BodyRec2;
//
//   module App3219; import Lib3219;
//   public class C
//     public static pure method Make(): int
//       var s: Sib := new Sib(1);
//       var r: BodyRec2 := new BodyRec2(s);       -- FALSE REJECT
//       return r.X;
//     end Make;
//     public static method Main(): int => 0;
//   end C;
//
//   imported (pre-fix) -> rc=1,
//     "record construction is not admitted in a pure body — the invoked
//      constructor is not pure-declared ..." (§22.15)
//   the single-module twin (pre-fix) -> rc=0, runs clean
//
// The only difference between the two programs is the module boundary. This is
// a FALSE REJECT: the ctor IS `pure`-declared, the importer simply cannot see
// the declaration.
//
// ⚠ WHY THE ROWS ASSERT ON DIAGNOSTIC TEXT, NOT ON "did it build". The accept
// rows below check that the §22.15 diagnostic text does NOT appear in stderr.
// The reject rows pin the §22.15 record-construction sentence. A row that only
// checked `rc == -1` would pass on a crash or on an unrelated rejection.
//
// ⚠ THE FALSE-ACCEPT HALF OF #3219 IS NOT TESTABLE ON THIS TREE. The issue's
// second repro was an imported POSITIONAL record with an impure shadowing ctor
// naming a sibling. #3141 (family F3 of umbrella #3152) added a positional
// arity check at `checkNewExprArgs` that fires BEFORE this walk is consulted:
// `new PosShadow2(3, s)` on an imported `record PosShadow2(A: int)` with a
// user-declared `public constructor(a: int, ignored: Sib)` rejects with "the
// parameter list has 1 (§22.5)" — `checkNewExprArgs` does not consult
// user-declared ctors on a positional record; the synthesized positional ctor
// is the ONLY candidate. So the false-accept shape is now unreachable as
// described in the issue, and the regression row for it would compile-FAIL for
// the wrong reason (arity), not for §22.15. The widening in this fix still
// closes that defect class wherever the walk IS reachable (body records, and
// any future shape that routes through it).
//
// ⚠ THE OVER-REJECTION CONTROLS ARE HALF THE TEST. This fix moves in the
// ACCEPT direction, so a test set that only pinned what should be admitted
// would also pass if the compiler admitted everything. T2/T2b (imported body
// record with an impure ctor naming a sibling) are the rows that fail if the
// new predicate is over-broad.
//
// ⚠ EACH ROW IS THE DIFFERENTIAL CONTROL. Both halves of every pair are
// required: T1 alone would pass on a compiler that admitted every imported
// record construction; T1b alone would pass on a §22.15 enforcement claim
// rather than a cross-module claim. Pairs together pin "the importer agrees
// with the twin".

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"  // #3156: the ONE sanitizer-honouring compileFile wrapper
#include "test_harness.hpp"

namespace bezel::module {
namespace {

namespace fs = std::filesystem;

struct TempFile3219 {
    std::string path;
    explicit TempFile3219(std::string p) : path(std::move(p)) {}
    ~TempFile3219() { std::remove(path.c_str()); }
};

void writeSource3219(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// The dependency library.
//
//   Sib        — a body record whose `pure` ctor is a sibling of BodyRec2. Used
//                as the parameter type of BodyRec2's `pure` ctor to trip the
//                sibling-name failure of `parseAndAttachCtorBody` (#2290 bug 2
//                / #710): the synthetic source declares only the one type, so
//                `resolveTypes` cannot resolve `Sib` and BodyRec2's ctor
//                `decl` is left null on the importer side.
//
//   BodyRec2   — body record with a `pure` ctor naming Sib. The ctor IS
//                `pure`-declared; the importer cannot see the declaration,
//                and `findUserCtorByArity` used to drop it.
//
//   ImpureBodyRec2 — body record with an IMPURE ctor naming Sib. The
//                over-rejection control: the ctor is impure on both sides of
//                the boundary and §22.15 fires on both sides too.
const char* kLib3219 =
    "module Lib3219; "
    "public record Sib "
    "  public readonly property S: int; "
    "  public pure constructor(s: int) this.S := s; end constructor; "
    "end Sib; "
    "public record BodyRec2 "
    "  public readonly property X: int; "
    "  public pure constructor(s: Sib) "
    "    this.X := s.S; "
    "  end constructor; "
    "end BodyRec2; "
    "public record ImpureBodyRec2 "
    "  public readonly property X: int; "
    "  public constructor(s: Sib) "
    "    Console.WriteLine(\"impure body ctor ran\"); "
    "    this.X := s.S; "
    "  end constructor; "
    "end ImpureBodyRec2;";

// The single-module twin of kLib3219 — character-identical declarations, no
// `import` anywhere. The ONLY difference from an imported row is the module
// boundary, which is what makes a divergence a cross-module defect rather than
// a §22.15 enforcement bug.
const char* kLocalDecls3219 =
    "public record Sib "
    "  public readonly property S: int; "
    "  public pure constructor(s: int) this.S := s; end constructor; "
    "end Sib; "
    "public record BodyRec2 "
    "  public readonly property X: int; "
    "  public pure constructor(s: Sib) "
    "    this.X := s.S; "
    "  end constructor; "
    "end BodyRec2; "
    "public record ImpureBodyRec2 "
    "  public readonly property X: int; "
    "  public constructor(s: Sib) "
    "    Console.WriteLine(\"impure body ctor ran\"); "
    "    this.X := s.S; "
    "  end constructor; "
    "end ImpureBodyRec2;";

std::string appSource3219(const std::string& body, const std::string& tag) {
    return "module App3219" + tag + "; import Lib3219; " + body;
}

std::string sameModuleProgram3219(const std::string& body,
                                  const std::string& tag) {
    return "module Ctl3219" + tag + "; " +
           std::string(kLocalDecls3219) + " " + body;
}

// Compile the dependency, compile the app against it, run the app in a SEPARATE
// process (per tests/README.md). Returns the child's exit status, or:
//   -1 compile/link/run failure (the app did not build)
//   -2 the dependency itself could not be produced (no toolchain)
// `capturedStderr`, when non-null, receives the app compile's stderr — the
// only way to assert on diagnostic TEXT.
int runImported3219(const std::string& body, const std::string& tag,
                    std::string* capturedStderr = nullptr) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bz3219_" + tag + "_" + std::to_string(std::rand());

    TempFile3219 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile3219 aObj((dir / (stem + "_a.o")).string());
    writeSource3219(aSrc.path, kLib3219);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success) return -2;
    }
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile3219 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3219 bExe((dir / (stem + "_b")).string());
    TempFile3219 bErr((dir / (stem + "_b.err")).string());
    writeSource3219(bSrc.path, appSource3219(body, tag));

    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);

    bool compiled = false;
    if (capturedStderr) {
        std::fflush(stderr);
        int saved = ::dup(STDERR_FILENO);
        int sink = ::open(bErr.path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (sink >= 0) {
            ::dup2(sink, STDERR_FILENO);
            ::close(sink);
        }
        compiled = bezel::test::compileFileForTest(driver, bSrc.path, bExe.path,
                                                   deps).success;
        std::fflush(stderr);
        if (saved >= 0) { ::dup2(saved, STDERR_FILENO); ::close(saved); }
        std::ifstream in(bErr.path);
        capturedStderr->assign((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    } else {
        compiled = bezel::test::compileFileForTest(driver, bSrc.path, bExe.path,
                                                   deps).success;
    }
    if (!compiled) return -1;
    if (!std::ifstream(bExe.path).good()) return -1;

    pid_t pid = ::fork();
    if (pid == 0) {
        ::execl(bExe.path.c_str(), bExe.path.c_str(), (char*)nullptr);
        ::_exit(127);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    if (WIFSIGNALED(status)) return 1000 + WTERMSIG(status);  // 1011 = SIGSEGV
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// Compile and run a SINGLE-module program — the differential control twin.
int runSingleModule3219(const std::string& src, const std::string& tag,
                        std::string* capturedStderr = nullptr) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bz3219ctl_" + tag + "_" + std::to_string(std::rand());
    TempFile3219 sSrc((dir / (stem + ".bzl")).string());
    TempFile3219 sExe((dir / stem).string());
    TempFile3219 sErr((dir / (stem + ".err")).string());
    writeSource3219(sSrc.path, src);

    bezel::Driver driver;
    bool compiled = false;
    if (capturedStderr) {
        std::fflush(stderr);
        int saved = ::dup(STDERR_FILENO);
        int sink = ::open(sErr.path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (sink >= 0) {
            ::dup2(sink, STDERR_FILENO);
            ::close(sink);
        }
        compiled = bezel::test::compileFileForTest(driver, sSrc.path, sExe.path,
                                                   {}).success;
        std::fflush(stderr);
        if (saved >= 0) { ::dup2(saved, STDERR_FILENO); ::close(saved); }
        std::ifstream in(sErr.path);
        capturedStderr->assign((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    } else {
        compiled = bezel::test::compileFileForTest(driver, sSrc.path, sExe.path,
                                                   {}).success;
    }
    if (!compiled) return -1;
    if (!std::ifstream(sExe.path).good()) return -1;

    pid_t pid = ::fork();
    if (pid == 0) {
        ::execl(sExe.path.c_str(), sExe.path.c_str(), (char*)nullptr);
        ::_exit(127);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    if (WIFSIGNALED(status)) return 1000 + WTERMSIG(status);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

bool mentions3219(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

// The §22.15 record-construction reject sentence, as the diagnostic renders
// it. Used by the reject rows.
const char* kRecordCtorDiag3219 =
    "record construction is not admitted in a pure body";

}  // namespace

// ===========================================================================
// T1 — HEADLINE ROW: an IMPORTED body record with a `pure` ctor that NAMES A
// SIBLING TYPE is ADMITTED in a pure body and runs clean. Pre-fix this
// compiled with rc=1 (false reject) on the importer side; the character-
// identical twin (T1b) compiled with rc=0.
// ===========================================================================
BEZEL_TEST(xmod_record_sibling_ctor_3219,
           t1_imported_body_record_pure_ctor_naming_sibling_admitted_22_15) {
    std::string err;
    int rc = runImported3219(
        "public class C3219T1 "
        "  public static pure method Make(): int "
        "    var s: Sib := new Sib(1); "
        "    var r: BodyRec2 := new BodyRec2(s); "
        "    return r.X; "
        "  end Make; "
        "  public static method Main(): int => 0; "
        "end C3219T1;",
        "t1", &err);
    if (rc == -2) return;  // no toolchain for the dependency
    BEZEL_EXPECT_EQ(rc, 0);
    BEZEL_EXPECT_TRUE(!mentions3219(err, kRecordCtorDiag3219));
}

// ===========================================================================
// T1b — the SAME-MODULE twin of T1. The two must agree; this is the
// differential control that makes T1 a cross-module claim rather than a §22.15
// enforcement claim.
// ===========================================================================
BEZEL_TEST(xmod_record_sibling_ctor_3219,
           t1b_same_module_body_record_pure_ctor_naming_sibling_admitted_22_15) {
    std::string err;
    int rc = runSingleModule3219(
        sameModuleProgram3219(
            "public class C3219T1b "
            "  public static pure method Make(): int "
            "    var s: Sib := new Sib(1); "
            "    var r: BodyRec2 := new BodyRec2(s); "
            "    return r.X; "
            "  end Make; "
            "  public static method Main(): int => 0; "
            "end C3219T1b;",
            "T1b"),
        "t1b", &err);
    BEZEL_EXPECT_EQ(rc, 0);
    BEZEL_EXPECT_TRUE(!mentions3219(err, kRecordCtorDiag3219));
}

// ===========================================================================
// T2 — OVER-REJECTION CONTROL: an IMPORTED body record with an IMPURE ctor
// that NAMES A SIBLING TYPE is REJECTED in a pure body, exactly as its
// character-identical twin is. Without this row T1 alone would pass on a
// compiler that admitted every imported record construction.
//
// The ctor is impure on BOTH sides of the boundary, and the rejection now
// answers the right reason: the ctor is impure, not "the ctor's decl is null
// and we don't know what it is" — the predicate widened, but the symbol-side
// `cs->isPure` is read off the wire (ModuleLoader::applyBehavior) and answers
// correctly.
// ===========================================================================
BEZEL_TEST(xmod_record_sibling_ctor_3219,
           t2_imported_body_record_impure_ctor_naming_sibling_rejected_22_15) {
    std::string err;
    int rc = runImported3219(
        "public class C3219T2 "
        "  public static pure method Make(): int "
        "    var s: Sib := new Sib(1); "
        "    var r: ImpureBodyRec2 := new ImpureBodyRec2(s); "
        "    return r.X; "
        "  end Make; "
        "  public static method Main(): int => 0; "
        "end C3219T2;",
        "t2", &err);
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, -1);
    BEZEL_EXPECT_TRUE(mentions3219(err, kRecordCtorDiag3219));
    BEZEL_EXPECT_TRUE(mentions3219(err, "22.15"));
}

// ===========================================================================
// T2b — the SAME-MODULE twin of T2.
// ===========================================================================
BEZEL_TEST(xmod_record_sibling_ctor_3219,
           t2b_same_module_body_record_impure_ctor_naming_sibling_rejected_22_15) {
    std::string err;
    int rc = runSingleModule3219(
        sameModuleProgram3219(
            "public class C3219T2b "
            "  public static pure method Make(): int "
            "    var s: Sib := new Sib(1); "
            "    var r: ImpureBodyRec2 := new ImpureBodyRec2(s); "
            "    return r.X; "
            "  end Make; "
            "  public static method Main(): int => 0; "
            "end C3219T2b;",
            "T2b"),
        "t2b", &err);
    BEZEL_EXPECT_EQ(rc, -1);
    BEZEL_EXPECT_TRUE(mentions3219(err, kRecordCtorDiag3219));
    BEZEL_EXPECT_TRUE(mentions3219(err, "22.15"));
}

}  // namespace bezel::module