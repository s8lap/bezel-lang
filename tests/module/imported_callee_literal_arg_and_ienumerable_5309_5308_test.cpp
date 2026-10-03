// Cross-module regression rows for #5309 and #5308 (§15.4 "Collection
// literals", §22.5 "Argument passing", §29.2 "Desugaring of `for`", stdlib §4
// `Array<T>`/`LinkedList<T>` implement `IEnumerable<T>`, v0.313).
//
//  * #5309: a collection literal passed to an IMPORTED callee's `Array<T>`
//    parameter used to lower to nothing (`lowerExpr` is null for a
//    CollectionLiteralExpr and the imported-argument arm had no literal case),
//    so only warning #3079/#2434 was emitted and the enclosing statement
//    vanished with exit 0.
//  * #5308: an `Array<T>` / `LinkedList<T>` handed to an IMPORTED callee's
//    pure `IEnumerable<T>` parameter was never wrapped in the vtable-backed
//    enumerator (the imported formal has no `astNode`, which the wrap gate
//    required), so the callee's for-in jumped to a null pc.
//
// Every row builds the library to an object, builds the importer under
// generated-code AddressSanitizer (LeakSanitizer ON), runs it with stderr
// folded in, and compares EXACT output and exit 0; a leak or a sanitizer
// report changes the output/exit code. Loops run 200 iterations.

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

struct TempFile5309 {
    std::string path;
    explicit TempFile5309(const std::string& p) : path(p) {}
    ~TempFile5309() { std::remove(path.c_str()); }
};

void writeSource5309(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

struct Run5309 {
    int code = 0;        // exit code; -1 app failed to build/run, -2 lib failed
    std::string out;     // stdout + stderr
    std::size_t errors = 0;
};

const char* kLib5309 = R"BZL(module XL5309;
public class Base
  public constructor() end constructor;
  public method SumA(xs: Array<int>): int
    var t := 0;
    for var x in xs do t := t + x; end;
    return t;
  end SumA;
  public method SumE(xs: IEnumerable<int>): int
    var t := 0;
    for var x in xs do t := t + x; end;
    return t;
  end SumE;
  public method Mid(a: int, xs: Array<int>, b: int): int
    var t := 0;
    for var x in xs do t := t + x; end;
    return a * 10000 + t * 100 + b;
  end Mid;
  public method Last(a: int, xs: Array<int>): int
    var t := 0;
    for var x in xs do t := t + x; end;
    return a * 100 + t;
  end Last;
  public method First(xs: Array<int>, a: int): int
    var t := 0;
    for var x in xs do t := t + x; end;
    return a * 100 + t;
  end First;
  public method Cat(xs: Array<string>): string
    var r := "";
    for var x in xs do r := r + x; end;
    return r;
  end Cat;
  public static method CountG<T>(xs: IEnumerable<T>): int
    var n := 0;
    for var x in xs do n := n + 1; end;
    return n;
  end CountG;
  public async method SumAsync(xs: Array<int>): Task<int>
    await Task.Delay(1);
    var t := 0;
    for var x in xs do t := t + x; end;
    return t;
  end SumAsync;
  public static method SumS(xs: Array<int>): int
    var t := 0;
    for var x in xs do t := t + x; end;
    return t;
  end SumS;
end Base;
)BZL";

// `mainBody` is the body of `Main(): int` (it has `b` bound as an XL5309.Base).
Run5309 run5309(const std::string& mainBody, bool async = false) {
    Run5309 r;
    auto dir = wtScratchDir("wt-5309-module");
    std::string stem = unique_stem("bz5309_");
    TempFile5309 libSrc((dir / (stem + "_lib.bzl")).string());
    TempFile5309 libObj((dir / (stem + "_lib.o")).string());
    writeSource5309(libSrc.path, kLib5309);
    {
        bezel::Driver driver;
        if (!driver.compileObject(libSrc.path, libObj.path).success) {
            r.code = -2;
            return r;
        }
    }
    TempFile5309 appSrc((dir / (stem + "_app.bzl")).string());
    TempFile5309 appExe((dir / (stem + "_app")).string());
    writeSource5309(appSrc.path,
        "module App5309;\nimport XL5309;\npublic class Program\n  public static " +
        std::string(async ? "async method Main(): Task<int>" : "method Main(): int") +
        "\n    using var b := new XL5309.Base();\n" + mainBody +
        "    return 0;\n  end Main;\nend Program;\n");
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = libObj.path;
    d.objectPath = libObj.path;
    deps.push_back(d);
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

// `for 200 iterations: s := s + <expr>` then print `<tag>=<s>`.
std::string loop200(const std::string& tag, const std::string& expr,
                    bool awaited = false) {
    (void)awaited;
    return "    var s := 0;\n"
           "    for var i in 0..<200 do s := s + " + expr + "; end;\n"
           "    Console.WriteLine(\"" + tag + "=\" + s.ToString());\n";
}

#define EXPECT_RUN_5309(r, want)             \
    do {                                     \
        BEZEL_EXPECT_TRUE((r).code != -2);   \
        BEZEL_EXPECT_EQ((r).code, 0);        \
        BEZEL_EXPECT_EQ((r).out, std::string(want)); \
    } while (0)

}  // namespace

// §15.4 / §22.5 (v0.313), #5309 headline repro: the statement must run.
BEZEL_TEST(imported_literal_arg_5309, headline_repro_prints_b6) {
    auto r = run5309(
        "    Console.WriteLine(\"before\");\n"
        "    Console.WriteLine(\"B=\" + b.SumA([1, 2, 3]).ToString());\n"
        "    Console.WriteLine(\"after\");\n");
    EXPECT_RUN_5309(r, "before\nB=6\nafter\n");
}

// Static imported callee, the literal as a `var` initializer too.
BEZEL_TEST(imported_literal_arg_5309, static_callee_and_local_initializer) {
    auto r = run5309(
        "    var n := XL5309.Base.SumS([1, 2, 3]);\n"
        "    Console.WriteLine(\"n=\" + n.ToString());\n"
        "    Console.WriteLine(\"m=\" + XL5309.Base.SumS([4, 5]).ToString());\n");
    EXPECT_RUN_5309(r, "n=6\nm=9\n");
}

BEZEL_TEST(imported_literal_arg_5309, literal_loop_200) {
    auto r = run5309(loop200("lit", "b.SumA([1, 2, 3])"));
    EXPECT_RUN_5309(r, "lit=1200\n");
}

BEZEL_TEST(imported_literal_arg_5309, empty_literal_loop_200) {
    auto r = run5309(loop200("empty", "b.SumA([]) + 1"));
    EXPECT_RUN_5309(r, "empty=200\n");
}

BEZEL_TEST(imported_literal_arg_5309, literal_first_argument_loop_200) {
    auto r = run5309(loop200("first", "b.First([2, 3], 1)"));
    EXPECT_RUN_5309(r, "first=21000\n");
}

BEZEL_TEST(imported_literal_arg_5309, literal_middle_argument_loop_200) {
    auto r = run5309(loop200("mid", "b.Mid(1, [2, 3], 4)"));
    EXPECT_RUN_5309(r, "mid=2100800\n");
}

BEZEL_TEST(imported_literal_arg_5309, literal_last_argument_loop_200) {
    auto r = run5309(loop200("last", "b.Last(1, [2, 3])"));
    EXPECT_RUN_5309(r, "last=21000\n");
}

// Leak-checked (LSan ON): the borrow-parameter literal temp is disposed.
BEZEL_TEST(imported_literal_arg_5309, string_literal_loop_200_leak_checked) {
    auto r = run5309(
        "    var str := \"\";\n"
        "    for var i in 0..<200 do str := b.Cat([\"a\", \"b\", \"c\"]); end;\n"
        "    Console.WriteLine(\"str=\" + str);\n");
    EXPECT_RUN_5309(r, "str=abc\n");
}

// Async imported callee taking the literal (frame-spilled slot, #5292).
BEZEL_TEST(imported_literal_arg_5309, async_imported_callee_literal_loop_200) {
    auto r = run5309(
        "    var s := 0;\n"
        "    for var i in 0..<200 do s := s + await b.SumAsync([1, 2, 3]); end;\n"
        "    Console.WriteLine(\"async=\" + s.ToString());\n",
        /*async=*/true);
    EXPECT_RUN_5309(r, "async=1200\n");
}

// A literal at an imported INTERFACE formal is not admissible (§15.4); the
// same-module twin is a sema error. It must be a loud compile error, never a
// dropped statement.
BEZEL_TEST(imported_literal_arg_5309, literal_at_imported_ienumerable_is_rejected) {
    auto r = run5309(loop200("enumLit", "b.SumE([1, 2, 3])"));
    BEZEL_EXPECT_EQ(r.code, -1);
    BEZEL_EXPECT_TRUE(r.errors >= 1);
}

// Same-module controls.
BEZEL_TEST(imported_literal_arg_5309, same_module_control) {
    namespace fs = std::filesystem;
    auto dir = wtScratchDir("wt-5309-module");
    std::string stem = unique_stem("bz5309s_");
    TempFile5309 src((dir / (stem + ".bzl")).string());
    TempFile5309 exe((dir / stem).string());
    writeSource5309(src.path, R"BZL(module SM5309;
public class Base
  public constructor() end constructor;
  public method SumA(xs: Array<int>): int
    var t := 0;
    for var x in xs do t := t + x; end;
    return t;
  end SumA;
  public method SumE(xs: IEnumerable<int>): int
    var t := 0;
    for var x in xs do t := t + x; end;
    return t;
  end SumE;
end Base;
public class Program
  public static method Main(): int
    using var b := new Base();
    using var arr := new Array<int>(3);
    arr.Add(1); arr.Add(2); arr.Add(3);
    Console.WriteLine("B=" + b.SumA([1, 2, 3]).ToString());
    Console.WriteLine("A=" + b.SumE(arr).ToString());
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
    BEZEL_EXPECT_EQ(out, std::string("B=6\nA=6\n"));
}

}  // namespace bezel::module
