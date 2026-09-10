// Cross-module confirmation for fix #4213 (§26.3, Appendix A.4
// `ExtensionMember` grammar, v0.298) — the default-deny per-member check
// `ExprTC::visitTopDecl`'s `ExtensionDecl` arm now applies is a SEMA-TIME
// rejection, so it fires the same way for a module compiled as a
// standalone LIBRARY (never imported by anything) as it does for one
// compiled directly. This is the stronger guarantee the #4204 review found
// for the sibling `InvariantDecl` check: the library fails at its OWN
// `compileObject` call, before there is ever an object to import — not a
// call-site check that only trips when some importer happens to reference
// the forbidden member.
//
// One representative kind (`ConstDecl`, the issue's own measured repro
// shape) is enough to confirm the check runs identically regardless of
// module role: the fix lives in sema, which runs once per compilation unit
// on its own AST, with no special-casing for "is this module ever
// imported". `tests/sema/extension_member_kind_4213_test.cpp` covers all
// six kinds (plus the bonus `IndexerDecl` case) in-process; this file only
// needs to prove the SAME rejection survives the `compileObject` cross-
// module entry point sema tests never exercise.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"

namespace {

struct TempFile4213 {
    std::string path;
    explicit TempFile4213(std::string p) : path(std::move(p)) {}
    ~TempFile4213() { std::remove(path.c_str()); }
};

void write4213(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

}  // namespace

// REJECT — a LIBRARY module (never imported here) whose `public extension`
// declares a `const` member must fail at its own `compileObject`, exactly
// as `bezelc -c` alone does for the same source (the issue's own repro).
// Before the fix this call returned `success == true` with zero
// diagnostics and a real `.o` on disk.
BEZEL_TEST(xmod_extension_member_kind_4213,
           library_module_const_in_extension_rejected_at_own_compile_26_3) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz4213_xmod_const_");
    TempFile4213 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile4213 aObj((dir / (stem + "_a.o")).string());

    write4213(aSrc.path,
        "module Lib4213Const; "
        "public class WidgetXC4213 "
        "  public X: int; "
        "end WidgetXC4213; "
        "public extension WidgetXC4213 "
        "  public static const K4213x: int := 42; "
        "  public method DoIt(): int => this.X; "
        "end extension;");

    bezel::Driver driver;
    auto result = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_FALSE(result.success);
    // No object should have been produced from a failed library compile.
    BEZEL_EXPECT_FALSE(std::ifstream(aObj.path).good());
}

// ACCEPT (control) — the SAME library module with the forbidden `const`
// member removed compiles and links cleanly as a dependency, and an
// importer can call its legitimate extension method — confirming this
// fix's rejection is specific to the forbidden member, not to library-role
// compilation or `public extension` in general.
BEZEL_TEST(xmod_extension_member_kind_4213,
           library_module_without_const_accepted_and_importable_26_3) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz4213_xmod_ok_");
    TempFile4213 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile4213 aObj((dir / (stem + "_a.o")).string());

    write4213(aSrc.path,
        "module Lib4213Ok; "
        "public class WidgetXOk4213 "
        "  public X: int; "
        "  public constructor(x: int) "
        "    X := x; "
        "  end constructor; "
        "end WidgetXOk4213; "
        "public extension WidgetXOk4213 "
        "  public method DoIt(): int => this.X + 1; "
        "end extension;");

    bezel::Driver driver;
    auto libResult = driver.compileObject(aSrc.path, aObj.path);
    BEZEL_EXPECT_TRUE(libResult.success);
    if (!std::ifstream(aObj.path).good()) return;

    TempFile4213 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile4213 bExe((dir / (stem + "_b")).string());
    write4213(bSrc.path,
        "module App4213; "
        "import Lib4213Ok; "
        "class C4213xok public static method Main(): int "
        "  var w := new WidgetXOk4213(41); "
        "  return w.DoIt(); "
        "end Main; end C4213xok;");

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    auto appResult = driver.compileFile(bSrc.path, bExe.path, deps);
    BEZEL_EXPECT_TRUE(appResult.success);
    if (!appResult.success) return;
    int status = std::system(bExe.path.c_str());
    BEZEL_EXPECT_TRUE(status != -1 && WIFEXITED(status));
    if (status != -1 && WIFEXITED(status))
        BEZEL_EXPECT_EQ(WEXITSTATUS(status), 42);
    std::remove(bExe.path.c_str());
}
