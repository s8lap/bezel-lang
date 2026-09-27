// Spec-derived regression test for #5048 / #5095, CROSS-MODULE rows: a record
// that lists the auto-imported `IEquatable<Self>` is satisfied by its
// synthesized `Equals(T)` (§9.1, §14.2 clause 2, stdlib §2.9, v0.307), and that
// admission stays narrow (§12.5 / §2.3, v0.307).
//
// The single-module rows live in tests/sema/record_iequatable_self_synthesis_
// 5048_test.cpp. These pin the module boundary:
//
//   X1  ACCEPT: a LIBRARY record `RecL(X, Y): IEquatable<RecL>` with no written
//       `Equals` compiles as a library, and an importer's
//       `where U: IEquatable<U>` inferred call and `IEquatable<RecL>` itable
//       call both compute structural equality (exit 7 = 1 + 2 + 4).
//   X2  REJECT (#5048's review note): a library record that does NOT list
//       `IEquatable<T>` does not satisfy `where U: IEquatable<U>` through a
//       cross-module INFERRED call — the §14.2 constraint diagnostic, located.
//   X3  REJECT (#5095 shape (c) at the boundary): a library record listing
//       `IEquatable<Point>, IEquatable<int>` with no `Equals(int)` fails its
//       own library compile with the §12.5 diagnostic; the synthesized
//       `Equals(Point)` witnesses only the Self entry.

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

struct TempFile5095 {
    std::string path;
    explicit TempFile5095(std::string p) : path(std::move(p)) {}
    ~TempFile5095() { std::remove(path.c_str()); }
};

void writeSource5095(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

template <typename Fn>
std::string captureStderr5095(Fn&& fn) {
    std::string errPath =
        (fs::temp_directory_path() /
         ("bz5095_err_" + unique_stem("") + ".txt")).string();
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

struct Outcome5095 {
    bool libOk = false;      // the library compiled to an object
    std::string libErr;      // its diagnostics
    bool appOk = false;      // the importer compiled and linked
    std::string appErr;      // its diagnostics
    int exitCode = -1;       // the importer's exit code when it ran
};

// Compile `libSrc` as a library object, then compile, link and (on success)
// RUN `appSrc` against it. `compileFileForTest` / `runProgramForTest` (#4532)
// honour BEZEL_AOT_FORCE_SANITIZE, so the gate's `--full` leg runs the
// importer under generated-code ASan while the row keeps asserting its value.
Outcome5095 buildImportAndRun5095(const std::string& libSrc,
                                  const std::string& appSrc) {
    auto dir = fs::temp_directory_path();
    std::string id = unique_stem("bz5095_");
    TempFile5095 libSrcF((dir / (id + "_lib.bzl")).string());
    TempFile5095 libObjF((dir / (id + "_lib.o")).string());
    TempFile5095 appSrcF((dir / (id + "_app.bzl")).string());
    TempFile5095 appExeF((dir / id).string());
    writeSource5095(libSrcF.path, libSrc);
    writeSource5095(appSrcF.path, appSrc);
    Outcome5095 o;
    o.libErr = captureStderr5095([&] {
        bezel::Driver driver;
        o.libOk = driver.compileObject(libSrcF.path, libObjF.path).success &&
                  std::ifstream(libObjF.path).good();
    });
    if (!o.libOk) return o;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = libObjF.path;
    d.objectPath = libObjF.path;
    deps.push_back(d);
    o.appErr = captureStderr5095([&] {
        bezel::Driver driver;
        o.appOk = bezel::test::compileFileForTest(driver, appSrcF.path,
                                                  appExeF.path, deps)
                      .success;
    });
    if (!o.appOk) return o;
    int status = bezel::test::runProgramForTest(appExeF.path);
    o.exitCode = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    return o;
}

const char* kLib5095 =
    "module LibEq5095; "
    "public record RecL(X: int, Y: int): IEquatable<RecL>; "
    "public record PlainL(X: int); ";

}  // namespace
}  // namespace bezel::module

using namespace bezel::module;

// X1: the accept row. §9.1: "the synthesized `Equals(T)` is what satisfies a
// declared `IEquatable<T>` conformance" — at the module boundary too.
BEZEL_TEST(xmod_record_iequatable_self_5095,
           x1_library_record_lists_iequatable_self_accepted_across_modules_spec_9_1_14_2) {
    auto o = buildImportAndRun5095(kLib5095,
        "module App5095; "
        "import LibEq5095; "
        "class App "
        "  public static method Chk<U>(a: U, b: U): bool where U: IEquatable<U> "
        "    => a.Equals(b); "
        "  public static method ChkI(a: IEquatable<RecL>, b: RecL): bool "
        "    => a.Equals(b); "
        "  public static method Main(): int "
        "    var r := 0; "
        "    if Chk(new RecL(1, 2), new RecL(1, 2)) then r := r + 1; end if; "
        "    if not Chk(new RecL(1, 2), new RecL(1, 3)) then r := r + 2; end if; "
        "    if ChkI(new RecL(4, 5), new RecL(4, 5)) then r := r + 4; end if; "
        "    return r; "
        "  end Main; "
        "end App;");
    BEZEL_EXPECT_TRUE(o.libOk);
    BEZEL_EXPECT_TRUE(o.appOk);
    BEZEL_EXPECT_EQ(o.exitCode, 7);
}

// X2: #5048's review note — an UNLISTED library record is rejected by the
// §14.2 constraint check through a cross-module inferred call.
BEZEL_TEST(xmod_record_iequatable_self_5095,
           x2_unlisted_library_record_rejected_by_iequatable_constraint_across_modules_spec_14_2) {
    auto o = buildImportAndRun5095(kLib5095,
        "module App5095R; "
        "import LibEq5095; "
        "class App "
        "  public static method Chk<U>(a: U, b: U): bool where U: IEquatable<U> "
        "    => a.Equals(b); "
        "  public static method Main(): int "
        "    if Chk(new PlainL(1), new PlainL(1)) then return 1; end if; "
        "    return 2; "
        "  end Main; "
        "end App;");
    BEZEL_EXPECT_TRUE(o.libOk);
    BEZEL_EXPECT_FALSE(o.appOk);
    BEZEL_EXPECT_TRUE(o.appErr.find("type argument `PlainL` does not satisfy "
                                    "the constraint") != std::string::npos);
    BEZEL_EXPECT_TRUE(o.appErr.find("§14.2") != std::string::npos);
}

// X3: #5095 shape (c) in a LIBRARY. The synthesized `Equals(Point)` witnesses
// only the `IEquatable<Point>` entry (§9.1); `IEquatable<int>.Equals(int)` has
// no implementation, so the library itself is rejected (§12.5).
BEZEL_TEST(xmod_record_iequatable_self_5095,
           x3_library_record_duplicate_iequatable_listing_rejected_spec_12_5) {
    auto o = buildImportAndRun5095(
        "module LibEqDup5095; "
        "public record PointD(X: int, Y: int): IEquatable<PointD>, IEquatable<int>; ",
        "module AppDup5095; "
        "import LibEqDup5095; "
        "class App "
        "  public static method Chk(a: IEquatable<int>, b: int): bool "
        "    => a.Equals(b); "
        "  public static method Main(): int "
        "    if Chk(new PointD(1, 2), 5) then return 3; end if; "
        "    return 4; "
        "  end Main; "
        "end App;");
    BEZEL_EXPECT_FALSE(o.libOk);
    BEZEL_EXPECT_TRUE(o.libErr.find("concrete record `PointD` does not "
                                    "implement inherited abstract member "
                                    "`Equals` (§12.5)") != std::string::npos);
}
