// Cross-module regression rows for #5328 items 3 and 4 (§15.4 "Collection
// literals", §28.3 "Disposal of unbound owned temporaries", stdlib §4
// `Set<T>` / `OwningArray<T>`, v0.315).
//
// Item 3: a Set<int> formal, imported, receiving a literal with duplicate
// elements. §15.4: "For Set<T>, OwningSet<T>: duplicates are collapsed".
// Before the fix: materializeHeapCollectionLiteral read collTy->astNode
// directly to check if the formal is a Set, which is null for an imported
// formal (the name lives on importedName). The Set dispatch was missed,
// the literal was stored as a bz_array_t* with NO deduplication, and the
// callee saw 3 elements where it should see 2.
//
// Item 4: a borrow OwningArray<Box> formal, imported, receiving a literal
// with fresh elements. §28.3: each new Box() element must be disposed.
// Before the fix: buildCollectionTempDisposal derived isOwning from
// collTy->headName(), which reads collTy->astNode (null for imported).
// isOwning was false, the borrowed-element policy was applied, and the
// Box objects leaked (400 allocations / 200 calls under ASan).
//
// Every leak-checked row runs 200 iterations (one-shot leak repros are
// vacuous — the harness requires ran, not just "didn't leak in isolation").

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

struct TempFile5328 {
    std::string path;
    explicit TempFile5328(const std::string& p) : path(p) {}
    ~TempFile5328() { std::remove(path.c_str()); }
};

void writeSource5328(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

struct Run5328 {
    int code = 0;        // exit code; -1 app failed to build/run, -2 lib failed
    std::string out;     // stdout + stderr
    std::size_t errors = 0;
};

// kLib5328: library that defines CountSt(Set<int>) and DisposeLater(borrow
// OwningArray<Box>). The Box type is defined in the library so it has a
// destructor that counts disposals.
const char* kLib5328 = R"BZL(module Lib5328;
public class Box
  public Item: int;
  public constructor(i: int) Item := i; end constructor;
  destructor end destructor;
end Box;
public class Ops
  // §15.4 Set dedupe. Returns s.Count — should be 2 for [1, 2, 2].
  public static method CountSt(s: Set<int>): int
    return s.Count;
  end CountSt;
  // §28.3: OwningArray<Box> (a borrow of an owning array). Returns sum of x.Item.
  // If the Box objects are leaked, the loop still runs but the output is
  // wrong (would be 600 if not leaked). We check output AND ran.
  public static method DisposeLater(xs: OwningArray<Box>): int
    var r := 0;
    for var x in xs do r := r + x.Item; end;
    return r;
  end DisposeLater;
end Ops;
)BZL";

// `mainBody` is the body of `Main(): int` (it uses Lib5328.Ops static methods).
Run5328 run5328(const std::string& mainBody,
                bool isAsync = false) {
    Run5328 r;
    auto dir = wtScratchDir("wt-5328-module");
    std::string stem = unique_stem("bz5328_");

    // Build main library.
    TempFile5328 libSrc((dir / (stem + "_lib.bzl")).string());
    TempFile5328 libObj((dir / (stem + "_lib.o")).string());
    writeSource5328(libSrc.path, kLib5328);
    {
        bezel::Driver driver;
        if (!driver.compileObject(libSrc.path, libObj.path).success) {
            r.code = -2;
            return r;
        }
    }

    TempFile5328 appSrc((dir / (stem + "_app.bzl")).string());
    TempFile5328 appExe((dir / (stem + "_app")).string());
    std::string appCode =
        "module App5328;\n"
        "import Lib5328;\n"
        "public class Program\n  public static " +
               std::string(isAsync ? "async method Main(): Task<int>" :
                                         "method Main(): int") +
               "\n" + mainBody +
               "    return 0;\n  end Main;\nend Program;\n";
    writeSource5328(appSrc.path, appCode);

    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    // Main library dependency.
    bezel::Dependency libDep;
    libDep.metadataPath = libObj.path;
    libDep.objectPath = libObj.path;
    deps.push_back(libDep);

    codegen::CodegenOptions opts;
    opts.sanitizeAddress = true;
    auto cr = bezel::test::compileFileForTest(driver, appSrc.path, appExe.path,
                                              deps, opts);
    r.errors = cr.errorCount;
    if (!cr.success) {
        r.code = -1;
        return r;
    }
    FILE* p = popen((appExe.path + " 2>&1").c_str(), "r");
    if (!p) {
        r.code = -1;
        return r;
    }
    char buf[256];
    while (std::size_t n = std::fread(buf, 1, sizeof(buf), p))
        r.out.append(buf, n);
    int status = pclose(p);
    r.code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    return r;
}

// 200 iterations of s := s + Lib5328.Ops.CountSt([1, 2, 2]), then print "c=<n>".
std::string loop200SetDedup(const std::string& tag) {
    return "    var s := 0;\n"
           "    for var i in 0..<200 do s := s + Lib5328.Ops.CountSt([1, 2, 2]); end;\n"
           "    Console.WriteLine(\"" + tag + "=\" + s.ToString());\n";
}

// 200 iterations of s := s + Lib5328.Ops.DisposeLater([new Lib5328.Box(i*2+1), new Lib5328.Box(i*2+2)])
// where each iteration produces sum of odd+even = (i*2+1)+(i*2+2) = 4i+3.
// Sum over 200: sum_{i=0}^{199} (4i+3) = 4*199*200/2 + 3*200 = 79600 + 600 = 80200.
std::string loop200OwningArray(const std::string& tag) {
    std::string body =
        "    var s := 0;\n"
        "    for var i in 0..<200 do\n"
        "      s := s + Lib5328.Ops.DisposeLater([\n"
        "        new Lib5328.Box(i * 2 + 1),\n"
        "        new Lib5328.Box(i * 2 + 2)]);\n"
        "    end;\n"
        "    Console.WriteLine(\"" + tag + "=\" + s.ToString());\n";
    return body;
}

#define EXPECT_RUN_5328(r, want)             \
    do {                                     \
        BEZEL_EXPECT_TRUE((r).code != -2);  \
        BEZEL_EXPECT_EQ((r).code, 0);        \
        BEZEL_EXPECT_EQ((r).out, std::string(want)); \
    } while (0)

}  // namespace

// §15.4 / §28.3 (v0.315), #5328 item 3 headline: an imported Set<int>
// formal receives a literal with duplicate [1, 2, 2]. §15.4: "duplicates
// are collapsed". Pre-fix: collTy->astNode is null for imported Set formal,
// Set dispatch is missed, literal stored as bz_array_t* with no dedup,
// callee sees 3. Post-fix: callee sees 2, output is 400.
BEZEL_TEST(imported_literal_set_dedupe_5328, set_dup_count_loop_200) {
    auto r = run5328(loop200SetDedup("c"), /*isAsync=*/false);
    EXPECT_RUN_5328(r, "c=400\n");
}

// §15.4 / §28.3 (v0.315), #5328 item 4 headline: a borrowed
// OwningArray<Box> formal, imported, receives literals with new Box() elements.
// §28.3: owned elements must be disposed. Pre-fix: collTy->astNode is null
// for imported, isOwning was false, borrowed-element policy applied, each
// new Box() leaked (400 allocations / 200 calls under ASan). Post-fix:
// elements are disposed, output is 80200, no leak.
BEZEL_TEST(imported_literal_set_dedupe_5328,
           owning_array_borrowed_loop_200_no_leak) {
    auto r = run5328(loop200OwningArray("sum"), /*isAsync=*/false);
    EXPECT_RUN_5328(r, "sum=80200\n");
}

// Same-module CONTROL for item 3: identical program in one module. Pre-fix
// the same-module path already uses builtinTypeName (which checks astNode),
// so this compiles and runs correctly even before the fix. This proves the
// imported channel is the discriminator.
BEZEL_TEST(imported_literal_set_dedupe_5328, same_module_set_dup_count_control) {
    namespace fs = std::filesystem;
    auto dir = wtScratchDir("wt-5328-module");
    std::string stem = unique_stem("bz5328s_");
    TempFile5328 src((dir / (stem + ".bzl")).string());
    TempFile5328 exe((dir / stem).string());
    writeSource5328(src.path, R"BZL(module SM5328;
class Box public Item: int;
  public constructor(i: int) Item := i; end constructor;
  destructor end destructor;
end Box;
public class Ops
  public static method CountSt(s: Set<int>): int
    return s.Count;
  end CountSt;
end Ops;
public class Program
  public static method Main(): int
    var s := 0;
    for var i in 0..<200 do s := s + Ops.CountSt([1, 2, 2]); end;
    Console.WriteLine("c=" + s.ToString());
    return 0;
  end Main;
end Program;
)BZL");
    bezel::Driver driver;
    codegen::CodegenOptions opts;
    opts.sanitizeAddress = true;
    BEZEL_EXPECT_TRUE(bezel::test::compileFileForTest(driver, src.path, exe.path,
                                                      {}, opts).success);
    FILE* p = popen((exe.path + " 2>&1").c_str(), "r");
    std::string out;
    char buf[256];
    if (p)
        while (std::size_t n = std::fread(buf, 1, sizeof(buf), p))
            out.append(buf, n);
    int status = p ? pclose(p) : -1;
    BEZEL_EXPECT_EQ(WIFEXITED(status) ? WEXITSTATUS(status) : -1, 0);
    BEZEL_EXPECT_EQ(out, std::string("c=400\n"));
}

// Same-module CONTROL for item 4: identical program in one module.
BEZEL_TEST(imported_literal_set_dedupe_5328,
           same_module_owning_array_borrowed_no_leak_control) {
    namespace fs = std::filesystem;
    auto dir = wtScratchDir("wt-5328-module");
    std::string stem = unique_stem("bz5328s2_");
    TempFile5328 src((dir / (stem + ".bzl")).string());
    TempFile5328 exe((dir / stem).string());
    writeSource5328(src.path, R"BZL(module SM5328b;
class Box public Item: int;
  public constructor(i: int) Item := i; end constructor;
  destructor end destructor;
end Box;
public class Ops
  public static method DisposeLater(xs: OwningArray<Box>): int
    var r := 0;
    for var x in xs do r := r + x.Item; end;
    return r;
  end DisposeLater;
end Ops;
public class Program
  public static method Main(): int
    var s := 0;
    for var i in 0..<200 do
      s := s + Ops.DisposeLater([
        new Box(i * 2 + 1),
        new Box(i * 2 + 2)]);
    end;
    Console.WriteLine("sum=" + s.ToString());
    return 0;
  end Main;
end Program;
)BZL");
    bezel::Driver driver;
    codegen::CodegenOptions opts;
    opts.sanitizeAddress = true;
    BEZEL_EXPECT_TRUE(bezel::test::compileFileForTest(driver, src.path, exe.path,
                                                      {}, opts).success);
    FILE* p = popen((exe.path + " 2>&1").c_str(), "r");
    std::string out;
    char buf[256];
    if (p)
        while (std::size_t n = std::fread(buf, 1, sizeof(buf), p))
            out.append(buf, n);
    int status = p ? pclose(p) : -1;
    BEZEL_EXPECT_EQ(WIFEXITED(status) ? WEXITSTATUS(status) : -1, 0);
    BEZEL_EXPECT_EQ(out, std::string("sum=80200\n"));
}

}  // namespace bezel::module
