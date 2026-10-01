// Cross-module regression for issue #5215 - §22.12 (v0.313): "a static member
// declared on a base class is a member of every class derived from it ... There
// is one cell per declaring instantiation, whatever type name reaches it." The
// generic base `B<T>` (static `_c`, static `Bump`) is IMPORTED from another
// module; the app's derived classes call the inherited static RECEIVERLESS
// (`Bump()`), which must run at the substitution the app's base clause supplies
// (`E: B<int>` -> T=int; `D<T>: B<T>` inside `D<string>` -> T=string), so the
// qualified reads `B<int>._c` / `B<string>._c` observe the writes. Exit code =
// B<int>._c * 10 + B<string>._c (expected 2*10 + 3 = 23; the pre-fix erased
// static wrote a cell those reads never saw, exit 0).
// Built like tests/module/imported_setter_fresh_temp_dispose_5209_test.cpp.

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

struct TempFile5215 {
    std::string path;
    explicit TempFile5215(std::string p) : path(std::move(p)) {}
    ~TempFile5215() { std::remove(path.c_str()); }
};

void writeSource5215(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj5215(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// -2 = the dependency toolchain is unavailable (test skips); -1 = the app did
// not build or did not exit normally.
int compileImportAndRun5215(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz5215_");
    TempFile5215 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile5215 aObj((dir / (stem + "_a.o")).string());
    writeSource5215(aSrc.path, libSrc);
    if (!compileToObj5215(aSrc.path, aObj.path)) return -2;
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile5215 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile5215 bExe((dir / stem).string());
    writeSource5215(bSrc.path, appSrc);
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


const char* kLib5215 = R"BZL(module BaseLib5215;
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

BEZEL_TEST(receiverless_inherited_static_generic_base_xmod_5215, imported_generic_base_cells_shared) {
    int r = compileImportAndRun5215(kLib5215, R"BZL(module App5215;
import BaseLib5215;
public class D<T>: B<T>
  public constructor() base(); end constructor;
  public static method Inner(): int
    return Bump();
  end Inner;
  public method InstInner(): int
    return Bump();
  end InstInner;
end D;
public class E: B<int>
  public constructor() base(); end constructor;
  public static method Inner2(): int
    return Bump();
  end Inner2;
end E;
public class Program
  public static method Main(): int
    D<string>.Inner();
    var d := new D<string>();
    d.InstInner();
    D<string>.Inner();
    E.Inner2();
    E.Inner2();
    return B<int>._c * 10 + B<string>._c;
  end Main;
end Program;
)BZL");
    BEZEL_EXPECT_TRUE(r != -2);
    BEZEL_EXPECT_EQ(r, 23);
}

}  // namespace bezel::module
