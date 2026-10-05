// codegen — fix #5293 (§28.3 *Unbound owned temporaries* rule 1 /
// §2.8.3 rule 4, v0.313): an IMPORTED callee with an `owned` formal followed
// by a later argument that throws — `b.TwoOwned(new L.Item(), L.Base.BoomS())`
// — leaks the fresh `new L.Item()` temporary on the exception edge. The spec's
// §28.3 exception-path discharge requires that an already-evaluated owned temporary
// for an `owned` formal be auto-disposed as the exception unwinds.
//
// The local-callee twin is clean: buildOrderedArgs (LlvmBackend_Assign.cpp:
// 15270–15315) registers an unwind-only disposal entry for this shape. The
// imported-callee argument arm (LlvmBackend_Calls2.cpp:12760–13070) did NOT,
// so the #993 registration was absent on the cross-module edge.
//
// FAIL-WITHOUT: n=0 (0 Items disposed) + 200×16B LSan Direct leak.
// PASS-WITH: n=200 h=200, no leak.
//
// CONTROL (single-module): the same program with both classes in one module
// already works — n=200 h=200. This confirms the bug is cross-module only
// and the fix does not regress the local path.

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan oracle
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace bezel::codegen {
namespace {

using bezel::test::runAotChecked;

// A dispose-counting class Item (static counter `Item.n` is bumped in the
// destructor). This is the type whose instances must NOT leak.
const char* kItem =
    "public class Item "
    "  public static n: int := 0; "
    "  public constructor() end constructor; "
    "  destructor Item.n := Item.n + 1; end destructor; "
    "  public method V(): int return 1; end V; "
    "end Item;\n";

// The base class that carries the method under test and the throwing accessor.
const char* kBase =
    "public class Base "
    "  public static hits: int := 0; "
    "  public constructor() end constructor; "
    "  public static method Hit() Base.hits := Base.hits + 1; end Hit; "
    "  public method TwoOwned(owned x: Item, y: int): int return x.V() + y; end TwoOwned; "
    "  public static method BoomS(): int throw new InvalidOperationException(\"b\"); end BoomS; "
    "end Base; ";

// Single-module program.
std::string singleModuleProg() {
    return std::string("module Single; ") + kItem + kBase +
           "class Program "
           "  public static method Main(): int "
           "    var s := 0; "
           "    for var i in 0..<200 do "
           "      using var b := new Base(); "
           "      try "
           "        s := s + b.TwoOwned(new Item(), Base.BoomS()); "
           "      catch "
           "        InvalidOperationException => Base.Hit(), "
           "      end try; "
           "    end; "
           "    Console.WriteLine(\"n=\" + Item.n.ToString() + \" h=\" + Base.hits.ToString()); "
           "    return 0; "
           "  end Main; "
           "end Program;";
}

// Cross-module harness — same pattern as alias_owned_param_keeps_auto_drop_3035_test.cpp.

const std::filesystem::path& wtDir() {
    return wtScratchDir("wt-5293-5293");
}

struct TempFile {
    std::string path;
    explicit TempFile(const std::string& p) : path(p) {}
    ~TempFile() { std::remove(path.c_str()); }
};

void writeSource(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

struct CaptureResult {
    int exitCode = -1;
    std::string stdoutText;
};

CaptureResult runCapture(const std::string& exe) {
    CaptureResult r;
    FILE* p = popen((exe + " 2>&1").c_str(), "r");
    if (!p) return r;
    char buf[256];
    while (std::size_t n = std::fread(buf, 1, sizeof(buf), p)) r.stdoutText.append(buf, n);
    int status = pclose(p);
    r.exitCode = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    return r;
}

struct LibHandle {
    std::shared_ptr<TempFile> src;
    std::shared_ptr<TempFile> obj;
    std::string objPath;
    bool ok = false;
};

LibHandle buildLib(const std::filesystem::path& dir, const std::string& libSrc) {
    LibHandle h;
    std::string stem = unique_stem("bezel_5293lib_");
    h.src = std::make_shared<TempFile>((dir / (stem + "_lib.bzl")).string());
    h.obj = std::make_shared<TempFile>((dir / (stem + "_lib.o")).string());
    writeSource(h.src->path, libSrc);
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileObject(h.src->path, h.obj->path);
    h.ok = r.success;
    h.objPath = h.obj->path;
    return h;
}

struct InProcessApp {
    bool ok = false;
    std::shared_ptr<TempFile> src;
    std::shared_ptr<TempFile> exe;
    std::string exePath;
};

InProcessApp buildApp(const std::filesystem::path& dir,
                      const std::string& libObjPath,
                      const std::string& appSrc) {
    InProcessApp r;
    if (libObjPath.empty()) return r;
    std::string stem = unique_stem("bezel_5293app_");
    r.src = std::make_shared<TempFile>((dir / (stem + "_app.bzl")).string());
    r.exe = std::make_shared<TempFile>((dir / (stem + "_app")).string());
    writeSource(r.src->path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = libObjPath;
    d.objectPath   = libObjPath;
    deps.push_back(d);
    bezel::CompileResult rb = driver.compileFile(r.src->path, r.exe->path, deps);
    r.ok = rb.success;
    r.exePath = r.exe->path;
    return r;
}

}  // namespace

// ===========================================================================
// CONTROL (single-module): the same program with both classes in one module
// already works. This confirms the local path is clean before and after the
// fix — the regression guard so a future change cannot silently reintroduce
// the local-path leak (which would also hide a re-regression of the
// imported-path fix).
// ===========================================================================
BEZEL_TEST(imported_owned_formal_unwind_leak_5293,
           control_single_module_clean) {
    // runAotChecked with sanitize=address — any leak would be caught.
    auto r = runAotChecked(singleModuleProg());
    BEZEL_EXPECT_TRUE(r.ran);    // no LSan leak
    // The expected output is "n=200 h=200\n" — all 200 Items are disposed
    // by the method's owned-param auto-disposal on the normal path (the
    // throw is caught and Hit() is called).
    BEZEL_EXPECT_EQ(r.stdoutText,
                     std::string("n=200 h=200\n"));
}

// ===========================================================================
// REGRESSION TEST (cross-module): the imported-callee path leaks the owned
// temporary when a later argument throws. Before the fix: n=0 (0 Items
// disposed) + 200×16B LSan Direct leak. After the fix: n=200 h=200, no
// leak.
// ===========================================================================
BEZEL_TEST(imported_owned_formal_unwind_leak_5293,
           xmod_imported_callee_owned_formal_unwind_leak) {
    auto dir = wtDir();
    const std::string lib =
        std::string("module L; ") + kItem + kBase;
    const std::string app =
        "module App; "
        "import L; "
        "class Program "
        "  public static method Main(): int "
        "    var s := 0; "
        "    for var i in 0..<200 do "
        "      using var b := new L.Base(); "
        "      try "
        "        s := s + b.TwoOwned(new L.Item(), L.Base.BoomS()); "
        "      catch "
        "        InvalidOperationException => L.Base.Hit(), "
        "      end try; "
        "    end; "
        "    Console.WriteLine(\"n=\" + L.Item.n.ToString() + \" h=\" + L.Base.hits.ToString()); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    LibHandle libH = buildLib(dir, lib);
    BEZEL_EXPECT_TRUE(libH.ok);
    if (!libH.ok) return;
    InProcessApp appH = buildApp(dir, libH.objPath, app);
    BEZEL_EXPECT_TRUE(appH.ok);
    if (!appH.ok) return;
    // Run the cross-module program and capture output. The stdout line
    // "n=<n> h=<h>\n" proves the fix: n=200 means the Item destructor ran
    // 200 times (no leak), and h=200 means the catch path still ran.
    // FAIL-WITHOUT: n=0 (0 Items disposed, 200×16B leaked, finalizers never
    // fired) while exitCode=0 (no exception propagated). Both numbers must be
    // asserted to guard against a regression that silently skips the try.
    CaptureResult cap = runCapture(appH.exePath);
    BEZEL_EXPECT_EQ(cap.exitCode, 0);
    BEZEL_EXPECT_EQ(cap.stdoutText,
                     std::string("n=200 h=200\n"));
}

}  // namespace bezel::codegen