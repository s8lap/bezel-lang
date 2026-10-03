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

#include "codegen/LlvmBackend.hpp"
#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"
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

// ===========================================================================
// fix-forward (#5286, §28.3 *Deferred-activation callees*, §30.1, v0.313):
// inside an ASYNC body, a suspension between the temporary's store and its
// disposal. The disposal runs on the resumed invocation of `...$Resume`, so
// the helper's slot must be frame-spilled exactly like the local arm's
// (tests/codegen/async_fresh_temp_arg_frame_spill_5286_test.cpp). Without it,
// the #5268 helper SEGVs in `Main$Resume`. The importer is built with
// `--sanitize=address` and run under the ambient (leak-detecting) ASan, 200
// iterations. Main returns 42 only when n, s and h all match; a sanitizer
// report exits nonzero with another code.
// ===========================================================================
namespace {

const char* kAsyncLib5268 = R"BZL(module AsyncLib5268;
public class Item
  public static n: int := 0;
  public constructor() end constructor;
  destructor Item.n := Item.n + 1; end destructor;
  public method V(): int return 1; end V;
end Item;
public class Base
  public static hits: int := 0;
  public constructor() end constructor;
  public static method Hit() Base.hits := Base.hits + 1; end Hit;
  public async method TakeA(x: Item): Task<int>
    const before := Item.n;
    await Task.Delay(1);
    return 10 * (Item.n - before) + x.V();
  end TakeA;
  public async method TakeNoSusp(x: Item): Task<int>
    return x.V();
  end TakeNoSusp;
  public async method ThrowA(x: Item): Task<int>
    await Task.Delay(1);
    if x.V() = 1 then throw new InvalidOperationException("x"); end;
    return 0;
  end ThrowA;
  public method Two(x: Item, y: int): int return x.V() + y; end Two;
  public static async method One(): Task<int>
    await Task.Delay(1);
    return 1;
  end One;
  public static async method Boom(): Task<int>
    await Task.Delay(1);
    throw new InvalidOperationException("b");
  end Boom;
end Base;
)BZL";

// -2 = the library did not build (row skips); -1 = the app did not build or
// did not exit normally.
int runAsync5268(const std::string& body, int wantS, int wantH) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz5268a_");
    TempFile5268 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile5268 aObj((dir / (stem + "_a.o")).string());
    writeSource5268(aSrc.path, kAsyncLib5268);
    if (!compileToObj5268(aSrc.path, aObj.path)) return -2;
    if (!std::ifstream(aObj.path).good()) return -2;

    std::string app =
        "module AppA5268;\nimport AsyncLib5268;\n"
        "public class Program\n"
        "  public static async method Main(): Task<int>\n"
        "    var s := 0;\n"
        "    for var i in 0..<200 do\n"
        "      using var b := new AsyncLib5268.Base();\n"
        "      " + body + "\n"
        "    end;\n"
        "    if AsyncLib5268.Item.n = 200 and s = " + std::to_string(wantS) +
        " and AsyncLib5268.Base.hits = " + std::to_string(wantH) +
        " then return 42; end;\n"
        "    return 7;\n"
        "  end Main;\n"
        "end Program;\n";
    TempFile5268 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile5268 bExe((dir / stem).string());
    writeSource5268(bSrc.path, app);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    codegen::CodegenOptions opts;
    opts.sanitizeAddress = true;
    if (!bezel::test::compileFileForTest(driver, bSrc.path, bExe.path, deps,
                                         opts).success)
        return -1;
    int status = std::system(bExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

}  // namespace

// Imported async borrow callee, fresh temporary, real suspension (the review's
// probe (d)).
BEZEL_TEST(imported_class_temp_arg_dispose_5268,
           async_awaited_imported_borrow_callee_fresh_temp_loop_200) {
    int r = runAsync5268(
        "s := s + await b.TakeA(new AsyncLib5268.Item());", 200, 0);
    BEZEL_EXPECT_TRUE(r != -2);
    BEZEL_EXPECT_EQ(r, 42);
}

// The imported task faults after its suspension. The unwind edge of the
// resumed invocation disposes the temporary.
BEZEL_TEST(imported_class_temp_arg_dispose_5268,
           async_imported_callee_faults_after_suspend_loop_200) {
    int r = runAsync5268(
        "try\n"
        "        s := s + await b.ThrowA(new AsyncLib5268.Item());\n"
        "      catch\n"
        "        InvalidOperationException => AsyncLib5268.Base.Hit(),\n"
        "      end try;", 0, 200);
    BEZEL_EXPECT_TRUE(r != -2);
    BEZEL_EXPECT_EQ(r, 42);
}

// A SYNC imported borrow callee whose later argument awaits.
BEZEL_TEST(imported_class_temp_arg_dispose_5268,
           async_body_imported_sync_callee_later_arg_awaits_loop_200) {
    int r = runAsync5268(
        "s := s + b.Two(new AsyncLib5268.Item(), "
        "await AsyncLib5268.Base.One()) - 1;", 200, 0);
    BEZEL_EXPECT_TRUE(r != -2);
    BEZEL_EXPECT_EQ(r, 42);
}

// The same, with the later awaited argument faulting.
BEZEL_TEST(imported_class_temp_arg_dispose_5268,
           async_body_imported_sync_callee_later_arg_faults_loop_200) {
    int r = runAsync5268(
        "try\n"
        "        s := s + b.Two(new AsyncLib5268.Item(), "
        "await AsyncLib5268.Base.Boom());\n"
        "      catch\n"
        "        InvalidOperationException => AsyncLib5268.Base.Hit(),\n"
        "      end try;", 0, 200);
    BEZEL_EXPECT_TRUE(r != -2);
    BEZEL_EXPECT_EQ(r, 42);
}

// Control: the imported async callee never suspends (clean both ways).
BEZEL_TEST(imported_class_temp_arg_dispose_5268,
           async_control_imported_callee_without_suspension_loop_200) {
    int r = runAsync5268(
        "s := s + await b.TakeNoSusp(new AsyncLib5268.Item());", 200, 0);
    BEZEL_EXPECT_TRUE(r != -2);
    BEZEL_EXPECT_EQ(r, 42);
}

// Control: a bound `using var` argument, not a temporary (clean both ways).
BEZEL_TEST(imported_class_temp_arg_dispose_5268,
           async_control_imported_callee_bound_local_loop_200) {
    int r = runAsync5268(
        "using var it := new AsyncLib5268.Item();\n"
        "      s := s + await b.TakeA(it);", 200, 0);
    BEZEL_EXPECT_TRUE(r != -2);
    BEZEL_EXPECT_EQ(r, 42);
}

}  // namespace bezel::module
