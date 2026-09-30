// Regression tests for issue #5209 - §23.3 *Explicit setter parameter modifier*
// and §28.3 *Unbound owned temporaries* (v0.313): the IMPORTED (cross-module)
// setter path lowerImportedPropertySetValue must dispose a fresh owned
// temporary passed to a plain setter, like #5119's inlined path. Item.n counts
// destructor runs in the library; exit code = Item.n - 100 (200 iterations, so
// the fixed program exits 100; a leaking one exits (0 - 100) mod 256 = 156).
// Built like tests/module/imported_nullable_property_set_2937_test.cpp.

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

struct TempFile5209 {
    std::string path;
    explicit TempFile5209(std::string p) : path(std::move(p)) {}
    ~TempFile5209() { std::remove(path.c_str()); }
};

void writeSource5209(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj5209(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// -2 = the dependency toolchain is unavailable (test skips); -1 = the app did
// not build or did not exit normally.
int compileImportAndRun5209(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz5209_");
    TempFile5209 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile5209 aObj((dir / (stem + "_a.o")).string());
    writeSource5209(aSrc.path, libSrc);
    if (!compileToObj5209(aSrc.path, aObj.path)) return -2;
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile5209 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile5209 bExe((dir / stem).string());
    writeSource5209(bSrc.path, appSrc);
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


const char* kLib5209 = R"BZL(module ItemLib5209;
public class Item
  public tag: int;
  public static n: int := 0;
  public constructor(t: int) tag := t; end constructor;
  destructor Item.n := Item.n + 1; end destructor;
end Item;
public class B
  private _v: Item?;
  public acc: int;
  public property Val: Item
    get => _v!;
    set => do acc := acc + value.tag; end;
  end Val;
end B;
public class BOwned
  private owned _v: Item?;
  public acc: int;
  public property Val: Item
    get => _v!;
    set(owned) => do acc := acc + value.tag; _v := value; end;
  end Val;
end BOwned;
)BZL";

}  // namespace

// Plain imported setter, 200 fresh temporaries: each destroyed once (Item.n
// = 200, exit 200 - 100 = 100).
BEZEL_TEST(imported_setter_fresh_temp_dispose_5209, plain_setter_loop_200) {
    int r = compileImportAndRun5209(kLib5209, R"BZL(module App5209;
import ItemLib5209;
public class Program
  public static method Main(): int
    var r := 0;
    do
      using var b := new ItemLib5209.B();
      for var i in 0..<200 do b.Val := new ItemLib5209.Item(i); end;
      if b.acc = 19900 then r := 1; end;
    end;
    if r = 1 then return ItemLib5209.Item.n - 100; end;
    return 255;
  end Main;
end Program;
)BZL");
    BEZEL_EXPECT_TRUE(r != -2);
    BEZEL_EXPECT_EQ(r, 100);
}

// Control: a borrowed RHS is never disposed by the assignment  (only `it` at scope end: Item.n = 1).
BEZEL_TEST(imported_setter_fresh_temp_dispose_5209, borrowed_rhs_not_disposed) {
    int r = compileImportAndRun5209(kLib5209, R"BZL(module App5209;
import ItemLib5209;
public class Program
  public static method Put(b: ItemLib5209.B, it: ItemLib5209.Item)
    b.Val := it;
  end Put;
  public static method Main(): int
    var r := 0;
    do
      using var it := new ItemLib5209.Item(3);
      using var b := new ItemLib5209.B();
      for var i in 0..<200 do Put(b, it); end;
      if b.acc = 600 then r := 1; end;
    end;
    if r = 1 then return ItemLib5209.Item.n; end;
    return 255;
  end Main;
end Program;
)BZL");
    BEZEL_EXPECT_TRUE(r != -2);
    BEZEL_EXPECT_EQ(r, 1);
}

// Control: set(owned) takes the value over, no caller-side dispose (no double
// dispose): still exactly 200 destructions.
BEZEL_TEST(imported_setter_fresh_temp_dispose_5209, set_owned_no_double_dispose) {
    int r = compileImportAndRun5209(kLib5209, R"BZL(module App5209;
import ItemLib5209;
public class Program
  public static method Main(): int
    var r := 0;
    do
      using var b := new ItemLib5209.BOwned();
      for var i in 0..<200 do b.Val := new ItemLib5209.Item(i); end;
      if b.acc = 19900 then r := 1; end;
    end;
    if r = 1 then return ItemLib5209.Item.n - 100; end;
    return 255;
  end Main;
end Program;
)BZL");
    BEZEL_EXPECT_TRUE(r != -2);
    BEZEL_EXPECT_EQ(r, 100);
}

}  // namespace bezel::module
