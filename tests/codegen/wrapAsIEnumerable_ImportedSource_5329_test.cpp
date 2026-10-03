// Cross-module regression rows for #5329 — the SOURCE-side mirror of #5308
// (spec §29.2 "Desugaring of `for`" / interface coercion, stdlib §4
// `Array<T>`/`LinkedList<T>` implement `IEnumerable<T>`, v0.313).
//
// #5308 fixed the TARGET side of `wrapAsIEnumerableForTarget`: an imported
// callee's `IEnumerable<T>` formal is rebuilt from module metadata and so has
// no `astNode`, which the wrap gate used to require; the head test alone is the
// gate now. But the same function decides the SOURCE side with
//
//     if (actualTy && actualTy->kind == Named && actualTy->astNode &&
//         actualTy->astNode->name && !actualTy->astNode->name->parts.empty())
//         an = actualTy->headName();
//
// A source expression whose static type comes from an IMPORTED declaration is
// exactly such an `astNode`-less TypeRef: its head lives in `TypeRef::
// importedName`, which `headName()` already falls back to (src/sema/TypeRef.
// hpp:80). The `astNode` conjunct therefore left `an` empty, `actualIsArray`
// false, and the function returned the RAW collection pointer. The consumer's
// for-in read that pointer's first word as an `IEnumerable` vtable and jumped
// to a null pc: `AddressSanitizer: SEGV ... pc 0x000000000000`, exit 1.
//
// Every row here builds the library to an object, builds the importer under
// generated-code AddressSanitizer (LeakSanitizer ON), runs it with stderr
// folded in, and compares EXACT output and exit 0; a leak or a sanitizer
// report changes the output or the exit code. The amplified rows run the
// coercion 200 times so a one-in-N misbehaviour cannot hide.

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

namespace bezel::codegen {
namespace {

struct TempFile5329 {
    std::string path;
    explicit TempFile5329(const std::string& p) : path(p) {}
    ~TempFile5329() { std::remove(path.c_str()); }
};

void writeSource5329(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

struct Run5329 {
    int code = 0;        // exit code; -1 app failed to build/run, -2 lib failed
    std::string out;     // stdout + stderr
    std::size_t errors = 0;
};

// The library module. `MakeArr` returns a collection typed by a DECLARATION
// that lives in the importing module's metadata set, so every use of it in the
// importer has a source type with no `astNode` — the #5329 source shape. The
// same-module control below spells the identical body inline, where the type
// DOES have an `astNode`, and passes today; that is the row that proves the
// difference is the import, not the collection.
const char* kLib5329 = R"BZL(module XL5329;
public class Base
  public constructor() end constructor;
  public static method MakeArr(): owned Array<int> return [1, 2, 3]; end MakeArr;
  public static owned readonly Cached: Array<int> := [1, 2, 3];
  public static property Shared: Array<int> get => Cached; end;
  public method SumE(xs: IEnumerable<int>): int
    var t := 0;
    for var x in xs do t := t + x; end;
    return t;
  end SumE;
end Base;
)BZL";

// `mainBody` is the body of `Main(): int` (it has `b` bound as an XL5329.Base).
Run5329 run5329(const std::string& mainBody) {
    Run5329 r;
    auto dir = wtScratchDir("wt-5329-source");
    std::string stem = unique_stem("bz5329_");
    TempFile5329 libSrc((dir / (stem + "_lib.bzl")).string());
    TempFile5329 libObj((dir / (stem + "_lib.o")).string());
    writeSource5329(libSrc.path, kLib5329);
    {
        bezel::Driver driver;
        if (!driver.compileObject(libSrc.path, libObj.path).success) {
            r.code = -2;
            return r;
        }
    }
    TempFile5329 appSrc((dir / (stem + "_app.bzl")).string());
    TempFile5329 appExe((dir / (stem + "_app")).string());
    writeSource5329(appSrc.path,
        "module App5329;\nimport XL5329;\npublic class Program\n  public static "
        "method Main(): int\n    using var b := new XL5329.Base();\n" + mainBody +
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
std::string loop200(const std::string& tag, const std::string& expr) {
    return "    var s := 0;\n"
           "    for var i in 0..<200 do s := s + " + expr + "; end;\n"
           "    Console.WriteLine(\"" + tag + "=\" + s.ToString());\n";
}

#define EXPECT_RUN_5329(r, want)             \
    do {                                     \
        BEZEL_EXPECT_TRUE((r).code != -2);   \
        BEZEL_EXPECT_EQ((r).code, 0);        \
        BEZEL_EXPECT_EQ((r).out, std::string(want)); \
    } while (0)

}  // namespace

// §29.2 / stdlib §4 (v0.313), #5329 headline repro: an imported-typed
// `Array<int>` widened to a local `IEnumerable<int>`, then iterated. Pre-fix
// the local-decl site handed back the raw collection pointer and the for-in
// jumped to pc 0 (ASan SEGV, exit 1).
BEZEL_TEST(wrapAsIEnumerable_imported_source_5329, local_decl_widening_prints_6) {
    auto r = run5329(
        "    using var a := XL5329.Base.MakeArr();\n"
        "    var e: IEnumerable<int> := a;\n"
        "    var s := 0;\n"
        "    for var x in e do s := s + x; end;\n"
        "    Console.WriteLine(\"localdecl=\" + s.ToString());\n");
    EXPECT_RUN_5329(r, "localdecl=6\n");
}

// The imported-typed source into an IMPORTED `IEnumerable<int>` formal. The
// #5308 rows cover a source built in the importer (which has an `astNode`);
// this one has the source typed from the imported declaration, so the source
// side of the same wrap is what has to fire.
BEZEL_TEST(wrapAsIEnumerable_imported_source_5329, imported_formal_typed_source_prints_6) {
    auto r = run5329(
        "    using var a := XL5329.Base.MakeArr();\n"
        "    Console.WriteLine(\"sumE=\" + b.SumE(a).ToString());\n");
    EXPECT_RUN_5329(r, "sumE=6\n");
}

// The direct imported expression at the imported formal — the source type comes
// straight off the callee's own declared return type, again with no `astNode`.
// `Base.Shared` is the §28.3-blessed spelling for re-exposing a cached
// collection (a static method cannot return a class type unmarked `owned`:
// "A static call has no borrow source"), so this leg exercises the
// direct-expression source path WITHOUT the unrelated pre-existing leak an
// `owned` temporary at that argument leaks (measured same-module on d27e5597 —
// not import-specific, and outside this issue).
BEZEL_TEST(wrapAsIEnumerable_imported_source_5329, imported_formal_direct_call_prints_6) {
    auto r = run5329(
        "    Console.WriteLine(\"direct=\" + b.SumE(XL5329.Base.Shared).ToString());\n");
    EXPECT_RUN_5329(r, "direct=6\n");
}

// Amplified (200x) legs for the two imported-formal shapes, so a coercion that
// only works on the first call cannot pass the row.
BEZEL_TEST(wrapAsIEnumerable_imported_source_5329, imported_formal_typed_source_loop_200) {
    auto r = run5329(
        "    using var a := XL5329.Base.MakeArr();\n" +
        loop200("sumE", "b.SumE(a)"));
    EXPECT_RUN_5329(r, "sumE=1200\n");
}

BEZEL_TEST(wrapAsIEnumerable_imported_source_5329, imported_formal_direct_call_loop_200) {
    auto r = run5329(loop200("direct", "b.SumE(XL5329.Base.Shared)"));
    EXPECT_RUN_5329(r, "direct=1200\n");
}

// Same-module control: the identical collection, widened at a local decl,
// handed to an `IEnumerable<int>` formal, and passed as a direct expression —
// but typed in the SAME module, where the source type DOES carry an `astNode`.
// This row passes before and after the fix and is pinned so the `headName()`
// gate cannot be widened into a regression of the already-working path.
BEZEL_TEST(wrapAsIEnumerable_imported_source_5329, same_module_source_control) {
    namespace fs = std::filesystem;
    auto dir = wtScratchDir("wt-5329-source");
    std::string stem = unique_stem("bz5329s_");
    TempFile5329 src((dir / (stem + ".bzl")).string());
    TempFile5329 exe((dir / stem).string());
    writeSource5329(src.path, R"BZL(module SM5329;
public class Base
  public constructor() end constructor;
  public static method MakeArr(): owned Array<int> return [1, 2, 3]; end MakeArr;
  public static owned readonly Cached: Array<int> := [1, 2, 3];
  public static property Shared: Array<int> get => Cached; end;
  public method SumE(xs: IEnumerable<int>): int
    var t := 0;
    for var x in xs do t := t + x; end;
    return t;
  end SumE;
end Base;
public class Program
  public static method Main(): int
    using var b := new Base();
    using var a := Base.MakeArr();
    var e: IEnumerable<int> := a;
    var s := 0;
    for var x in e do s := s + x; end;
    Console.WriteLine("localdecl=" + s.ToString());
    Console.WriteLine("sumE=" + b.SumE(a).ToString());
    Console.WriteLine("direct=" + b.SumE(Base.Shared).ToString());
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
    BEZEL_EXPECT_EQ(out, std::string("localdecl=6\nsumE=6\ndirect=6\n"));
}

}  // namespace bezel::codegen
