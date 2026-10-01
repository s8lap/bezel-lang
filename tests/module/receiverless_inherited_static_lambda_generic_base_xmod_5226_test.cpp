// Cross-module regression for issue #5226 - §22.12 (v0.313), §14.1: the generic
// base `B<T>` (static `_c`, static `Bump`) is IMPORTED; the app's derived
// classes call the inherited static RECEIVERLESS from inside a LAMBDA, which
// must run at the substitution the app's base clause supplies (`E: B<int>` ->
// T=int; `D<T>: B<T>` inside `D<string>` -> T=string). Exit code =
// B<int>._c * 10 + B<string>._c (expected 4*10 + 3 = 43; pre-fix the lambda's
// call wrote an erased cell those reads never saw).
// Built like tests/module/receiverless_inherited_static_generic_base_xmod_5215_test.cpp.

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

namespace bezel::module {
namespace {

struct TempFile5226 {
    std::string path;
    explicit TempFile5226(std::string p) : path(std::move(p)) {}
    ~TempFile5226() { std::remove(path.c_str()); }
};

void writeSource5226(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj5226(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// -2 = the dependency toolchain is unavailable (test skips); -1 = the app did
// not build or did not exit normally.
int compileImportAndRun5226(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz5226_");
    TempFile5226 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile5226 aObj((dir / (stem + "_a.o")).string());
    writeSource5226(aSrc.path, libSrc);
    if (!compileToObj5226(aSrc.path, aObj.path)) return -2;
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile5226 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile5226 bExe((dir / stem).string());
    writeSource5226(bSrc.path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    if (!driver.compileFile(bSrc.path, bExe.path, deps).success) return -1;
    int status = std::system(bExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}


const char* kLib5226 = R"BZL(module BaseLib5226;
public class B<T>
  public static _c: int := 0;
  public constructor() end constructor;
  public static method Bump(): int
    B<T>._c := B<T>._c + 1;
    return B<T>._c;
  end Bump;
end B;
)BZL";

}  // namespace

BEZEL_TEST(receiverless_inherited_static_lambda_generic_base_xmod_5226, imported_generic_base_cells_shared) {
    int r = compileImportAndRun5226(kLib5226, R"BZL(module App5226;
import BaseLib5226;
public class D<T>: B<T>
  public constructor() base(); end constructor;
  public static method Inner(): int
    var f := lambda(): int => Bump();
    return f() + f();
  end Inner;
  public method InstInner(): int
    var f := lambda(): int => Bump();
    return f();
  end InstInner;
end D;
public class E: B<int>
  public constructor() base(); end constructor;
  public static method Inner2(): int
    var f := lambda(): int => Bump();
    return f() + f();
  end Inner2;
end E;
public class Program
  public static method Main(): int
    D<string>.Inner();
    var d := new D<string>();
    d.InstInner();
    E.Inner2();
    E.Inner2();
    return B<int>._c * 10 + B<string>._c;
  end Main;
end Program;
)BZL");
    BEZEL_EXPECT_TRUE(r != -2);
    BEZEL_EXPECT_EQ(r, 43);
}

}  // namespace bezel::module
