// Spec-derived regression test for issue #2971 — the CROSS-MODULE arm.
//
// §31.4 (Standard exception types) / §12.1 (Inheritance and dispatch) / §11.3
// (Member body) / §2.5 (Modules and imports) / §2.8.3 rule 4, v0.240.
//
// §31.4 declares the surface a class deriving from a built-in exception
// inherits, verbatim:
//
//     public class Exception
//       public readonly property Message: string;
//       public readonly property StackTrace: string;             // formatted; "" when unavailable
//       public readonly property CauseChain: Array<ExceptionInfo>;   // empty when no cause
//     end Exception;
//     public class ArgumentException: Exception
//       public readonly property ParamName: string?;   // null when not supplied
//     end ArgumentException;
//
// THE DEFECT (#2971) was that sema exposed only `Message` of it through a
// subclass's own static type. The issue measured the gap to be SYMMETRIC across
// the module boundary — "a LOCAL subclass and an IMPORTED one behave
// identically, both before and after #2950" — so the fix must be symmetric too,
// and that is what this file pins. It is not automatic: whether a class carries
// the §31.4 header at all is answered by `sema::builtinExceptionBaseOf`, whose
// IMPORTED arm reads the `builtinExceptionBase` symbol field that #2950 added
// and ModuleLoader restores; an imported class has no `ast::TypeDecl` for the
// walk to read. If that field were ever lost across the boundary, the member
// LOOKUP would silently answer "not inherited" here while the exporter's
// compiled constructor had already written the slots — the same
// two-artifacts-disagree shape #2950 fixed, seen through the member surface.
//
// tests/codegen/exception_surface_inherited_2971_test.cpp covers the
// single-module shapes and the negative controls. This file is the boundary.
//
// Built like its sibling xmod_imported_exception_subclass_layout_2950_test.cpp:
// a real `bezelc -c lib.bzl` + `bezelc --import lib.o app.bzl` + a native run.
// The observable is the process EXIT CODE, one distinct value per assertion, so
// a wrong value is attributable rather than merely "non-zero".
//
// BASE MESSAGES ARE LITERALS ON PURPOSE, as in the #2950 sibling: nothing
// releases a user exception subclass's inherited Message (#2951), and the
// CauseChain snapshot a subclass's `base(msg, cause)` builds is not reclaimed
// either (#2989, measured by this lane). These cells are about the member SURFACE,
// not lifetime, so they run without sanitizers and stay independent of both.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "codegen/LlvmBackend.hpp"
#include "driver/Driver.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"


namespace bezel::module {
namespace {

struct TempFile2971 {
    std::string path;
    explicit TempFile2971(std::string p) : path(std::move(p)) {}
    ~TempFile2971() { std::remove(path.c_str()); }
};

void writeSource2971(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// -2 = the dependency toolchain is unavailable (test skips); -1 = the app did
// not build/link, or did not exit normally. WITHOUT the fix these cells land on
// -1: the app does not compile at all ("no member `StackTrace` on type …").
int compileImportAndRun2971(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2971_");
    TempFile2971 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2971 aObj((dir / (stem + "_a.o")).string());
    writeSource2971(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success) return -2;
    }
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2971 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2971 bExe((dir / stem).string());
    writeSource2971(bSrc.path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    codegen::CodegenOptions opts;
    if (!driver.compileFile(bSrc.path, bExe.path, deps, opts).success)
        return -1;
    int status = std::system(bExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

}  // namespace

// ===========================================================================
// T1 — the IMPORTER reads the §31.4 surface off an IMPORTED subclass through
// that subclass's own static type. Each assertion has its own exit code:
//
//   3  StackTrace  (§31.4 "" when unavailable)
//   4  CauseChain.Count on a chained instance (the snapshot the EXPORTER's
//      compiled constructor wrote at offset 24)
//   5  CauseChain[0].Message — the snapshot's content, not merely its length
//   6  Message, the one member that already worked, as an in-program control
// ===========================================================================
BEZEL_TEST(xmod_exception_surface_2971,
           imported_subclass_exposes_the_whole_31_4_surface_spec_31_4) {
    const char* lib =
        "module ExLib2971T1; "
        "public class MyEx2971: Exception "
        "  public constructor(m: string, c: Exception) base(m, c); "
        "  end constructor; "
        "end MyEx2971;";
    int r = compileImportAndRun2971(
        lib,
        "module ExApp2971T1; import ExLib2971T1; "
        "public class C2971T1 "
        "  public static method Main(): int "
        "    using var root := new Exception(\"root\"); "
        "    using var e: MyEx2971 := new MyEx2971(\"wrap\", root); "
        "    if e.StackTrace <> \"\" then return 3; end if; "
        "    if e.CauseChain.Count <> 1 then return 4; end if; "
        "    if e.CauseChain[0].Message <> \"root\" then return 5; end if; "
        "    if e.Message <> \"wrap\" then return 6; end if; "
        "    return 0; "
        "  end Main; "
        "end C2971T1;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 0);
}

// ===========================================================================
// T2 — the OTHER direction: a LOCAL class whose base is an IMPORTED exception
// subclass. `builtinExceptionBaseOf` must cross the boundary MID-WALK (the
// imported ancestor has no `ast::TypeDecl`), which is the arm #2950's T2 pinned
// for layout and this pins for the member surface. `ParamName` rides an
// IMPORTED `ArgumentException` subclass, so the §31.4-declaring ancestor is
// reached only through metadata.
//
//   3  ParamName on the local subclass of an imported ArgumentException subclass
//   4  StackTrace through the same chain
//   5  Message through the same chain (control)
// ===========================================================================
BEZEL_TEST(xmod_exception_surface_2971,
           local_subclass_of_an_imported_exception_base_spec_12_1) {
    const char* lib =
        "module ExLib2971T2; "
        "public class ImpArg2971: ArgumentException "
        "  public constructor(m: string, p: string) base(m, p); "
        "  end constructor; "
        "end ImpArg2971;";
    int r = compileImportAndRun2971(
        lib,
        "module ExApp2971T2; import ExLib2971T2; "
        "public class LocalArg2971: ImpArg2971 "
        "  public constructor() base(\"mm\", \"pp\"); end constructor; "
        // The UNQUALIFIED spelling, from inside the local subclass's body —
        // a different resolver site from the receiver-typed one below.
        "  public method BarePn(): string return ParamName ?? \"<null>\"; "
        "  end BarePn; "
        "end LocalArg2971; "
        "public class C2971T2 "
        "  public static method Main(): int "
        "    using var l := new LocalArg2971(); "
        "    if (l.ParamName ?? \"<null>\") <> \"pp\" then return 3; end if; "
        "    if l.BarePn() <> \"pp\" then return 4; end if; "
        "    if l.StackTrace <> \"\" then return 5; end if; "
        "    if l.Message <> \"mm\" then return 6; end if; "
        "    return 0; "
        "  end Main; "
        "end C2971T2;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 0);
}

}  // namespace bezel::module
