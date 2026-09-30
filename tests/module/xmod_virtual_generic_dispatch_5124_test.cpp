// Regression tests for #5124 (split into #5160 and #5161): a constrained
// generic method called ACROSS a module boundary through a virtual dispatch.
//
// Spec: §14.1 / §14.2 (generic methods are monomorphized per type argument),
// §2.8.3 rule 4 (an imported generic is instantiated in the importing module),
// §11 (a call through a base-typed receiver runs the most-derived override),
// v0.313. Every row asserts the value the single-module twin computes AND the
// same value across the boundary: a 0 or the base body's value is the bug.
//
//   #5160 — a body re-parsed on the #5009 constraint route could not type a
//           call to an attached generic sibling member (`b.Get()` on
//           `BoxVO5124<T>`), so even the NON-virtual `NB` degraded to a zero
//           return (rejected by the #5009 backstop; twin 57).
//   #5161 / #5124 — virtual / override bodies were not shipped, so the
//           dispatch had no body to specialize (no substitution, `ret 0`), and
//           neither imported overrides nor an importer's override of an
//           imported virtual generic method were dispatch candidates.
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"

namespace bezel::module {
namespace {

namespace fs = std::filesystem;

struct TempFile5124 {
    std::string path;
    explicit TempFile5124(std::string p) : path(std::move(p)) {}
    ~TempFile5124() { std::remove(path.c_str()); }
};

void writeSource5124(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

template <typename Fn>
std::string captureStderr5124(Fn&& fn) {
    std::string errPath =
        (fs::temp_directory_path() /
         ("bz5124_err_" + unique_stem("") + ".txt")).string();
    std::fflush(stderr);
    std::cerr.flush();
    int saved = ::dup(STDERR_FILENO);
    int fd = ::open(errPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) ::dup2(fd, STDERR_FILENO);
    fn();
    std::fflush(stderr);
    std::cerr.flush();
    if (fd >= 0) {
        ::close(fd);
        ::dup2(saved, STDERR_FILENO);
    }
    ::close(saved);
    std::ifstream in(errPath);
    std::string out((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());
    std::remove(errPath.c_str());
    return out;
}

struct Run5124 {
    bool depOk = true;
    bool compiled = false;
    int rc = -1;
    std::string err;
};

Run5124 buildImportAndRun5124(const std::string& libSrc,
                              const std::string& mainSrc) {
    auto dir = fs::temp_directory_path();
    std::string id = unique_stem("bz5124r_");
    TempFile5124 libSrcF((dir / (id + "_lib.bzl")).string());
    TempFile5124 libObjF((dir / (id + "_lib.o")).string());
    TempFile5124 mainSrcF((dir / (id + "_main.bzl")).string());
    TempFile5124 mainExeF((dir / id).string());
    writeSource5124(libSrcF.path, libSrc);
    writeSource5124(mainSrcF.path, mainSrc);
    Run5124 r;
    {
        bezel::Driver driver;
        if (!driver.compileObject(libSrcF.path, libObjF.path).success ||
            !std::ifstream(libObjF.path).good()) {
            r.depOk = false;
            return r;
        }
    }
    r.err = captureStderr5124([&] {
        bezel::Driver driver;
        std::vector<bezel::Dependency> deps;
        bezel::Dependency d;
        d.metadataPath = libObjF.path;
        d.objectPath = libObjF.path;
        deps.push_back(d);
        r.compiled = bezel::test::compileFileForTest(driver, mainSrcF.path,
                                                     mainExeF.path, deps)
                         .success;
    });
    if (!r.compiled) return r;
    int status = bezel::test::runProgramForTest(mainExeF.path);
    r.rc = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    return r;
}

// A dependency D, a LIBRARY L that imports D and makes the virtual call, and
// a main M that imports both. Returns the raw wait status (-1: no program).
int buildLibCallSiteAndRun5124(const std::string& depSrc,
                               const std::string& libSrc,
                               const std::string& mainSrc) {
    auto dir = fs::temp_directory_path();
    std::string id = unique_stem("bz5124c_");
    TempFile5124 depSrcF((dir / (id + "_dep.bzl")).string());
    TempFile5124 depObjF((dir / (id + "_dep.o")).string());
    TempFile5124 libSrcF((dir / (id + "_lib.bzl")).string());
    TempFile5124 libObjF((dir / (id + "_lib.o")).string());
    TempFile5124 mainSrcF((dir / (id + "_main.bzl")).string());
    TempFile5124 mainExeF((dir / id).string());
    writeSource5124(depSrcF.path, depSrc);
    writeSource5124(libSrcF.path, libSrc);
    writeSource5124(mainSrcF.path, mainSrc);
    bezel::Dependency d;
    d.metadataPath = depObjF.path;
    d.objectPath = depObjF.path;
    bezel::Dependency l;
    l.metadataPath = libObjF.path;
    l.objectPath = libObjF.path;
    {
        bezel::Driver driver;
        if (!driver.compileObject(depSrcF.path, depObjF.path).success)
            return -1;
    }
    {
        bezel::Driver driver;
        if (!driver.compileObject(libSrcF.path, libObjF.path, {d}).success)
            return -1;
    }
    bezel::Driver driver;
    if (!bezel::test::compileFileForTest(driver, mainSrcF.path, mainExeF.path,
                                         {d, l})
             .success)
        return -1;
    return bezel::test::runProgramForTest(mainExeF.path);
}

int buildLocalAndRun5124(const std::string& src) {
    auto dir = fs::temp_directory_path();
    std::string id = unique_stem("bz5124l_");
    TempFile5124 srcF((dir / (id + ".bzl")).string());
    TempFile5124 exeF((dir / id).string());
    writeSource5124(srcF.path, src);
    bezel::Driver driver;
    if (!bezel::test::compileFileForTest(driver, srcF.path, exeF.path).success)
        return -1;
    int status = bezel::test::runProgramForTest(exeF.path);
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

const char* kDecls5124 =
    "public interface IVO5124 "
    "  method Area(): int; "
    "end IVO5124; "
    "public class SqVO5124: IVO5124 "
    "  public constructor() end constructor; "
    "  public method Area(): int return 7; end Area; "
    "end SqVO5124; "
    "public class BoxVO5124<U> "
    "  private _u: U; "
    "  public constructor(u: U) _u := u; end constructor; "
    "  public method Get(): U return _u; end Get; "
    "end BoxVO5124; "
    "public class GVO5124 "
    "  public constructor() end constructor; "
    "  public virtual method VB<T>(b: BoxVO5124<T>): int where T: IVO5124 "
    "    return b.Get().Area() + 120; "
    "  end VB; "
    "  public virtual method VP<T>(v: T): int where T: IVO5124 "
    "    return v.Area() + 100; "
    "  end VP; "
    "  public static method NB<T>(b: BoxVO5124<T>): int where T: IVO5124 "
    "    return b.Get().Area() + 50; "
    "  end NB; "
    "  public static method Run(g: GVO5124): int "
    "    using var s := new SqVO5124(); "
    "    var bx := new BoxVO5124<SqVO5124>(s); "
    "    return g.VB(bx); "
    "  end Run; "
    "end GVO5124; "
    "public class GVO5124Drv: GVO5124 "
    "  public constructor() end constructor; "
    "  public override method VB<T>(b: BoxVO5124<T>): int where T: IVO5124 "
    "    return b.Get().Area() + 200; "
    "  end VB; "
    "  public override method VP<T>(v: T): int where T: IVO5124 "
    "    return v.Area() + 200; "
    "  end VP; "
    "end GVO5124Drv; ";

const char* kMine5124 =
    "class Mine5124: GVO5124 "
    "  public constructor() end constructor; "
    "  public override method VB<T>(b: BoxVO5124<T>): int where T: IVO5124 "
    "    return b.Get().Area() + 30; "
    "  end VB; "
    "end Mine5124; ";

std::string mainOf5124(const std::string& body) {
    return std::string(kMine5124) +
           "class C5124 public static method Main(): int "
           "using var s := new SqVO5124(); "
           "var bx := new BoxVO5124<SqVO5124>(s); " +
           body + " end Main; end C5124;";
}

void probe5124(const std::string& tag, const std::string& body, int want) {
    int local = buildLocalAndRun5124("module L5124" + tag + "; " +
                                     std::string(kDecls5124) +
                                     mainOf5124(body));
    auto r = buildImportAndRun5124(
        "module D5124" + tag + "; " + std::string(kDecls5124),
        "module M5124" + tag + "; import D5124" + tag + "; " +
            mainOf5124(body));
    BEZEL_EXPECT_EQ(local, want);
    BEZEL_EXPECT_TRUE(r.depOk);
    BEZEL_EXPECT_TRUE(r.compiled);
    if (!r.compiled) std::cerr << "[" << tag << "] " << r.err << "\n";
    BEZEL_EXPECT_EQ(r.rc, want);
}

}  // namespace

// #5124 original repro: base-typed receiver holding the imported override.
BEZEL_TEST(xmod_virtual_generic_dispatch_5124, spec_11_base_receiver_runs_imported_override_5124) {
    probe5124("P1", "using var g := new GVO5124Drv(); var b: GVO5124 := g; "
                    "return b.VB(bx);", 207);
}
// #5161: the base body itself, specialized at the call's type argument.
BEZEL_TEST(xmod_virtual_generic_dispatch_5124, spec_14_1_virtual_base_body_specialized_5161) {
    probe5124("P2", "using var g := new GVO5124(); return g.VB(bx);", 127);
}
// #5124: a bare `T` formal (no generic sibling in the body).
BEZEL_TEST(xmod_virtual_generic_dispatch_5124, spec_11_bare_t_formal_imported_override_5124) {
    probe5124("P3", "using var g := new GVO5124Drv(); var b: GVO5124 := g; "
                    "return b.VP(s);", 207);
}
// #5160: the non-virtual static twin, generic sibling call `b.Get()`.
BEZEL_TEST(xmod_virtual_generic_dispatch_5124, spec_2_8_3_static_generic_sibling_call_5160) {
    probe5124("P4", "return GVO5124.NB(bx);", 57);
}
// #5161: the override called through its own static type.
BEZEL_TEST(xmod_virtual_generic_dispatch_5124, spec_14_1_derived_receiver_override_5161) {
    probe5124("P5", "using var g := new GVO5124Drv(); return g.VB(bx);", 207);
}
// #5124: the importer's own override of the imported virtual generic method.
BEZEL_TEST(xmod_virtual_generic_dispatch_5124, spec_11_importer_override_of_imported_method_5124) {
    probe5124("P6", "using var g := new Mine5124(); var b: GVO5124 := g; "
                    "return b.VB(bx);", 37);
}

// The call site is in a LIBRARY (§2.4) compiled before the module that
// declares the receiver's class. `LibL5124.Call` knows `GVO5124`, `GVO5124Drv`
// (its dependency) and dispatches them correctly (rows a, b). An instance of
// the later module's `Mine5124` is a class the library cannot know; running
// the base body for it would return 127 where §11 requires 37 (the same
// closed-world limit a dependency-internal virtual generic call has). The
// constraint-route dispatch traps that instance instead (row c): the program
// aborts, it never returns a wrong value.
namespace {
const char* kLib5124 =
    "module L5124; import D5124; "
    "public static class LibL5124 "
    "  public static method Call(g: GVO5124): int "
    "    using var s := new SqVO5124(); "
    "    var bx := new BoxVO5124<SqVO5124>(s); "
    "    return g.VB(bx); "
    "  end Call; "
    "end LibL5124; ";

int libCallSite5124(const std::string& tag, const std::string& body) {
    return buildLibCallSiteAndRun5124(
        "module D5124; " + std::string(kDecls5124), kLib5124,
        "module M5124" + tag + "; import D5124; import L5124; " +
            std::string(kMine5124) +
            "class C5124 public static method Main(): int " + body +
            " end Main; end C5124;");
}
}  // namespace

BEZEL_TEST(xmod_virtual_generic_dispatch_5124, spec_11_library_call_site_known_override_5124) {
    int st = libCallSite5124(
        "La", "using var g := new GVO5124Drv(); return LibL5124.Call(g);");
    BEZEL_EXPECT_TRUE(st != -1 && WIFEXITED(st));
    BEZEL_EXPECT_EQ(WIFEXITED(st) ? WEXITSTATUS(st) : -1, 207);
}
BEZEL_TEST(xmod_virtual_generic_dispatch_5124, spec_11_library_call_site_base_5124) {
    int st = libCallSite5124(
        "Lb", "using var g := new GVO5124(); return LibL5124.Call(g);");
    BEZEL_EXPECT_TRUE(st != -1 && WIFEXITED(st));
    BEZEL_EXPECT_EQ(WIFEXITED(st) ? WEXITSTATUS(st) : -1, 127);
}
BEZEL_TEST(xmod_virtual_generic_dispatch_5124, spec_11_library_call_site_unknown_class_traps_5124) {
    int st = libCallSite5124(
        "Lc", "using var g := new Mine5124(); return LibL5124.Call(g);");
    BEZEL_EXPECT_TRUE(st != -1);
    // SIGABRT from bz_debug_assert_fail; under the sanitizer sweep the shell
    // reports it as exit 128+6 (see runProgramForTest).
    bool aborted = st != -1 && ((WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT) ||
                                (WIFEXITED(st) && WEXITSTATUS(st) == 134));
    BEZEL_EXPECT_TRUE(aborted);
}

}  // namespace bezel::module
