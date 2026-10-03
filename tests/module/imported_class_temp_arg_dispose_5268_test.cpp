// Regression tests for issue #5268 - §28.3 *Unbound owned temporaries*
// (v0.313): a fresh `new C()` temporary passed to a BORROW parameter of an
// IMPORTED callee (method, static method, constructor) must be disposed at the
// end of the full expression, exactly as for a local callee. Item.n counts
// destructor runs in the library; each row loops 120 times and exits with
// Item.n - 100, so a correct program exits 20 and a leaking one (n = 0)
// exits (0 - 100) mod 256 = 156. Built like
// tests/module/imported_setter_fresh_temp_dispose_5209_test.cpp.

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

struct TempFile5268 {
    std::string path;
    explicit TempFile5268(std::string p) : path(std::move(p)) {}
    ~TempFile5268() { std::remove(path.c_str()); }
};

void writeSource5268(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj5268(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// -2 = the dependency toolchain is unavailable (test skips); -1 = the app did
// not build or did not exit normally.
int compileImportAndRun5268(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz5268_");
    TempFile5268 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile5268 aObj((dir / (stem + "_a.o")).string());
    writeSource5268(aSrc.path, libSrc);
    if (!compileToObj5268(aSrc.path, aObj.path)) return -2;
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile5268 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile5268 bExe((dir / stem).string());
    writeSource5268(bSrc.path, appSrc);
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


const char* kLib5268 = R"BZL(module ItemLib5268;
public class Item
  public static n: int := 0;
  public constructor() end constructor;
  destructor Item.n := Item.n + 1; end destructor;
  public method V(): int return 1; end V;
end Item;
public class Base
  public constructor() end constructor;
  public virtual method Take(x: Item): int return 10 + x.V(); end Take;
  public static method TakeS(x: Item): int return 20 + x.V(); end TakeS;
  public method Eat(owned x: Item): int return 30 + x.V(); end Eat;
end Base;
public class Der: Base
  public constructor() end constructor;
  public override method Take(x: Item): int return 110 + x.V(); end Take;
end Der;
public class Holder
  public v: int;
  public constructor(x: Item) v := x.V(); end constructor;
end Holder;
)BZL";

// Wraps `stmt` (run 120 times, `s` accumulates) in an app and returns
// Item.n - 100 when the accumulated value is right.
int run5268(const std::string& stmt, int perIter, const std::string& extra = "") {
    std::string app = "module App5268;\nimport ItemLib5268;\n"
        "public class Program\n" + extra +
        "  public static method Main(): int\n"
        "    var s := 0;\n"
        "    do\n"
        "      using var b: ItemLib5268.Base := new ItemLib5268.Der();\n"
        "      for var i in 0..<120 do s := s + " + stmt + "; end;\n"
        "    end;\n"
        "    if s = 120 * " + std::to_string(perIter) + " then return ItemLib5268.Item.n - 100; end;\n"
        "    return 255;\n"
        "  end Main;\n"
        "end Program;\n";
    return compileImportAndRun5268(kLib5268, app);
}

}  // namespace

// Imported virtual instance method, borrow param (the issue's headline shape).
BEZEL_TEST(imported_class_temp_arg_dispose_5268, instance_method_arg_loop_120) {
    int r = run5268("b.Take(new ItemLib5268.Item())", 111);
    BEZEL_EXPECT_TRUE(r != -2);
    BEZEL_EXPECT_EQ(r, 20);
}

// Imported static method, borrow param.
BEZEL_TEST(imported_class_temp_arg_dispose_5268, static_method_arg_loop_120) {
    int r = run5268("ItemLib5268.Base.TakeS(new ItemLib5268.Item())", 21);
    BEZEL_EXPECT_TRUE(r != -2);
    BEZEL_EXPECT_EQ(r, 20);
}

// Imported constructor, borrow param.
BEZEL_TEST(imported_class_temp_arg_dispose_5268, ctor_arg_loop_120) {
    int r = run5268("(new ItemLib5268.Holder(new ItemLib5268.Item())).v", 1);
    BEZEL_EXPECT_TRUE(r != -2);
    BEZEL_EXPECT_EQ(r, 20);
}

// Imported constructor, borrow param, NAMED argument (§22.5 arm).
BEZEL_TEST(imported_class_temp_arg_dispose_5268, ctor_named_arg_loop_120) {
    int r = run5268("(new ItemLib5268.Holder(x := new ItemLib5268.Item())).v", 1);
    BEZEL_EXPECT_TRUE(r != -2);
    BEZEL_EXPECT_EQ(r, 20);
}

// Local function taking an imported class (the shape that was already clean).
BEZEL_TEST(imported_class_temp_arg_dispose_5268, local_callee_imported_type_loop_120) {
    int r = run5268("G(new ItemLib5268.Item())", 41,
        "  public static method G(x: ItemLib5268.Item): int return 40 + x.V(); end G;\n");
    BEZEL_EXPECT_TRUE(r != -2);
    BEZEL_EXPECT_EQ(r, 20);
}

// `owned` formal: the callee consumes the temporary; the caller must NOT also
// dispose it (no double dispose, still exactly 120 destructions).
BEZEL_TEST(imported_class_temp_arg_dispose_5268, owned_param_consumed_not_double_disposed) {
    int r = run5268("b.Eat(new ItemLib5268.Item())", 31);
    BEZEL_EXPECT_TRUE(r != -2);
    BEZEL_EXPECT_EQ(r, 20);
}

// Control: a borrowed (named) argument is not disposed by the call; only the
// `using var` at scope end destroys it (n = 1, exit 1 - 100 mod 256 = 157).
BEZEL_TEST(imported_class_temp_arg_dispose_5268, named_borrow_arg_not_disposed) {
    int r = compileImportAndRun5268(kLib5268, R"BZL(module App5268;
import ItemLib5268;
public class Program
  public static method Main(): int
    var s := 0;
    do
      using var b: ItemLib5268.Base := new ItemLib5268.Der();
      using var it := new ItemLib5268.Item();
      for var i in 0..<120 do s := s + b.Take(it); end;
    end;
    if s = 120 * 111 then return ItemLib5268.Item.n; end;
    return 255;
  end Main;
end Program;
)BZL");
    BEZEL_EXPECT_TRUE(r != -2);
    BEZEL_EXPECT_EQ(r, 1);
}

// Control: the single-module twin of the headline row (always clean).
BEZEL_TEST(imported_class_temp_arg_dispose_5268, single_module_control_loop_120) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz5268s_");
    TempFile5268 src((dir / (stem + ".bzl")).string());
    TempFile5268 exe((dir / stem).string());
    writeSource5268(src.path, R"BZL(module L5268;
class Item
  public static n: int := 0;
  public constructor() end constructor;
  destructor Item.n := Item.n + 1; end destructor;
  public method V(): int return 1; end V;
end Item;
class Base
  public constructor() end constructor;
  public method Take(x: Item): int return 10 + x.V(); end Take;
end Base;
class Program
  public static method Main(): int
    var s := 0;
    do
      using var b := new Base();
      for var i in 0..<120 do s := s + b.Take(new Item()); end;
    end;
    if s = 120 * 11 then return Item.n - 100; end;
    return 255;
  end Main;
end Program;
)BZL");
    bezel::Driver driver;
    BEZEL_EXPECT_TRUE(driver.compileFile(src.path, exe.path, {}).success);
    int status = std::system(exe.path.c_str());
    BEZEL_EXPECT_EQ(WIFEXITED(status) ? WEXITSTATUS(status) : -1, 20);
}

}  // namespace bezel::module
