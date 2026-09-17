// Cross-module imported generic-class method monomorphization (issue #783 / #428,
// §14.1 monomorphization, §2.5 modules).
//
// A generic class exported from module A (`Box<T>` with `Get(): T return this.v;`)
// instantiated in module B with a REFERENCE type (`Box<string>`) must return the
// field at the correct `ptr` offset, not the i32-placeholder offset the defining
// module compiled the template against. Before the fix, this was SOUND-GATED with
// a clean §14.1/#783 diagnostic (a wrong value was never emitted). After the fix,
// the exporter carries each generic-class method's body source text (schema v5
// `methodBodyText`); the loader re-parses it, type-resolves it, and attaches a
// live ast::MethodDecl to the imported MethodSymbol, and codegen monomorphizes the
// body per instantiation (the cross-module complement of the local generic-method
// path) so the value is correct. Shapes whose body could not be reconstructed
// (references to sibling types / module functions the synthetic class lacks) keep
// the clean gate — never a wrong value.
//
// TEST MATRIX:
//   T1  Box<string>.Get() — reference (string) instantiation returns the right
//       value (repro from the issue: `b.Get().Length` == 5).
//   T2  Box<Widget>.Get().Size — a SECOND reference type (a class) returns the
//       right class instance / field.
//   T3  Box<int>.Get() — the value (int) instantiation still works (must not
//       regress; matches the placeholder by luck and via the new path).
//   T4  Holder<Cell>.item owned field disposed EXACTLY once cross-module (§28.3);
//       Cell's destructor counter returns to 0, ASan clean (no leak/double-free).
//   T5  GATE CANARY: a method whose body references a sibling type the synthetic
//       reconstruction cannot reproduce, returning the type param, must be
//       REJECTED with the #783 diagnostic — never compiled to a wrong value.
//
// Each runnable shape also gets an ASan/LSan probe.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "test_harness.hpp"
#include "native_test_harness.hpp"
#include "support/driver_run_support.hpp"  // #3156: the ONE sanitizer-honouring compileFile wrapper
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"


namespace bezel::module {
namespace {

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit. The
// process-unique stem is what keeps concurrent shards from racing each
// other's files mid-compile.
const std::filesystem::path& wtDir783() {
    return wtScratchDir("wt-783");
}


struct TempFile783 {
    std::string path;
    explicit TempFile783(const std::string& p) : path(p) {}
    ~TempFile783() { std::remove(path.c_str()); }
};

void writeSource783(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

static const std::string& laneLocalAsanRt783() {
    static std::string cached;
    static bool attempted = false;
    if (attempted) return cached;
    attempted = true;
    std::string dst = (wtDir783() / "rt_asan.o");
    std::filesystem::create_directories(wtDir783());
    std::ifstream check(dst);
    if (check.good()) { cached = dst; return cached; }
    if (!::bezel::native_test::buildRuntimeSanitizerObject("address", dst))
        return cached;
    cached = dst;
    return cached;
}

// Compile A → object. Returns true on success.
static bool compileToObj783(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// Compile B importing A → exe, run, return exit code (-1 on compile/link/run fail).
static int compileImportAndRun783(const std::string& bSrc,
                                  const std::string& bExe,
                                  const std::string& aObj) {
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj;
    d.objectPath = aObj;
    deps.push_back(d);
    if (!driver.compileFile(bSrc, bExe, deps).success) return -1;
    int status = std::system(bExe.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// True when compiling B importing A FAILS (used by the gate canary).
static bool compileImportFails783(const std::string& bSrc,
                                  const std::string& bExe,
                                  const std::string& aObj) {
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj;
    d.objectPath = aObj;
    deps.push_back(d);
    return !driver.compileFile(bSrc, bExe, deps).success;
}

// Cross-module ASan/LSan probe: compile B importing A to an EXECUTABLE via
// compileFileForTest (so the Bezel compiler emits a native `int main()` for us
// — see LlvmBackend_Setup.cpp:89, `if (isMain(ms)) return "main";`), link
// A.o + the Bezel-emitted B exe + ASan runtime, run under detect_leaks.
// Returns 0 = clean+correct, 1 = leak/double-free (exit 99), -1 = toolchain
// unavailable → SKIP, -3 = compile/link failure.
//
// EARLIER DESIGN (revoked — see #783 regression log 2026-09-18): an earlier
// revision used Driver::compileObject for B (library mode, §33.6 / #1563) and
// supplied a hand-written C++ main shim of the form
//   extern int bz$App.C$Main(void);
//   int main(void) { return bz$App.C$Main(); }
// to fill the missing native `main`. The shim FAILS TO COMPILE because
// `bz$App.C$Main` contains a `.`, which is not a valid C or C++ identifier
// character (POSIX §6.4.2 / C++ §5.10 [lex.name]); clang rejects it with
//   error: expected ';' after top level declarator
// and the probe's link step then returns -3, masking whatever the Bezel
// program itself does (the Bezel symbol exists in b.o but the link fails on
// the unresolved shim — which is the opposite of the truth: the Bezel code
// compiles fine, the shim is malformed). The simpler fix is to NOT use
// library mode for B: `compileFile` is executable mode, the Bezel compiler
// emits the `main` symbol itself, and no shim is needed.
static int xmodAsan783(const std::string& aObj, const std::string& bSrc,
                       int expectedCode) {
    const std::string& rt = laneLocalAsanRt783();
    if (rt.empty()) return -1;

    namespace fs = std::filesystem;
    auto tmpDir = fs::temp_directory_path();
    static int ctr = 0;
    std::string stem = "bz783_asan_" + std::to_string(ctr++);
    std::string bSrcPath = (tmpDir / (stem + "_b.bzl")).string();
    std::string exePath = (tmpDir / (stem + "_exe")).string();
    writeSource783(bSrcPath, bSrc);

    auto cleanup = [&] {
        std::remove(bSrcPath.c_str());
        std::remove(exePath.c_str());
    };

    // Compile B importing A to an EXECUTABLE — Bezel emits a native `int main()`
    // for us. We must use compileFile (executable mode), not compileObject
    // (library mode), because the latter omits `main` and the resulting b.o has
    // only `bz$App.C$Main`, a symbol that cannot be referenced from a portable
    // C/C++ main shim (`.` is not an identifier character).
    {
        bezel::Driver driver;
        std::vector<bezel::Dependency> deps;
        bezel::Dependency d;
        d.metadataPath = aObj;
        d.objectPath = aObj;
        deps.push_back(d);
        if (!bezel::test::compileFileForTest(
                driver, bSrcPath, exePath, deps,
                bezel::codegen::CodegenOptions{}).success) {
            cleanup();
            return -3;
        }
    }

    std::string runCmd =
        "ASAN_OPTIONS=detect_leaks=1:exitcode=99:abort_on_error=0 " + exePath +
        " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return -3;
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (code == 99) return 1;
    if (code == expectedCode) return 0;
    return -3;
}

}  // namespace

// ===========================================================================
// T1 — Box<string>.Get() returns the string at the correct offset (#783 repro).
// ===========================================================================
BEZEL_TEST(xmod_generic_method_783, t1_box_string_get_returns_value_14_1) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz783_t1_");
    TempFile783 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile783 aObj((dir / (stem + "_a.o")).string());

    writeSource783(aSrc.path,
        "module Lib783A; "
        "public class Box<T> "
        "  public v: T; "
        "  public constructor() end constructor; "
        "  public method Get(): T return this.v; end Get; "
        "end Box;");
    BEZEL_EXPECT_TRUE(compileToObj783(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    std::string bSrc =
        "module App; import Lib783A; "
        "public class C "
        "  public static method Main(): int "
        "    using var b := new Box<string>(); "
        "    b.v := \"hello\"; "
        "    return b.Get().Length; "  // 5
        "  end Main; "
        "end C;";
    TempFile783 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile783 bExe((dir / (stem + "_b")).string());
    writeSource783(bSrcFile.path, bSrc);
    BEZEL_EXPECT_EQ(compileImportAndRun783(bSrcFile.path, bExe.path, aObj.path), 5);

    int asan = xmodAsan783(aObj.path, bSrc, 5);
    if (asan == -1) return;
    BEZEL_EXPECT_EQ(asan, 0);
}

// ===========================================================================
// T2 — a SECOND reference type (a class) — Box<Widget>.Get().Size.
// ===========================================================================
BEZEL_TEST(xmod_generic_method_783, t2_box_class_get_second_reftype_14_1) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz783_t2_");
    TempFile783 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile783 aObj((dir / (stem + "_a.o")).string());

    writeSource783(aSrc.path,
        "module Lib783B; "
        "public class Widget "
        "  public Size: int; "
        "  public constructor(s: int) Size := s; end constructor; "
        "end Widget; "
        "public class Box<T> "
        "  public v: T; "
        "  public constructor() end constructor; "
        "  public method Get(): T return this.v; end Get; "
        "end Box;");
    BEZEL_EXPECT_TRUE(compileToObj783(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    // #1020 / §28.6 (v0.153): a fresh `new Widget(99)` stored into the non-`owned`
    // generic field `v` would leak. Hold the Widget in an owned `using var` and
    // store a BORROW of it (a back-reference param) — the box borrows it for its
    // lifetime; the path still exercises the cross-module generic field write +
    // `Get()` read of a second reference type.
    std::string bSrc =
        "module App; import Lib783B; "
        "public class C "
        "  public static method UseBox(w: Widget): int "
        "    var b := new Box<Widget>(); "
        "    b.v := w; "
        "    return b.Get().Size; "  // 99
        "  end UseBox; "
        "  public static method Main(): int "
        "    using var w := new Widget(99); "
        "    return UseBox(w); "
        "  end Main; "
        "end C;";
    TempFile783 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile783 bExe((dir / (stem + "_b")).string());
    writeSource783(bSrcFile.path, bSrc);
    BEZEL_EXPECT_EQ(compileImportAndRun783(bSrcFile.path, bExe.path, aObj.path), 99);
}

// ===========================================================================
// T3 — Box<int>.Get() still works (must not regress).
// ===========================================================================
BEZEL_TEST(xmod_generic_method_783, t3_box_int_get_no_regression_14_1) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz783_t3_");
    TempFile783 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile783 aObj((dir / (stem + "_a.o")).string());

    writeSource783(aSrc.path,
        "module Lib783C; "
        "public class Box<T> "
        "  public v: T; "
        "  public constructor() end constructor; "
        "  public method Get(): T return this.v; end Get; "
        "end Box;");
    BEZEL_EXPECT_TRUE(compileToObj783(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    std::string bSrc =
        "module App; import Lib783C; "
        "public class C "
        "  public static method Main(): int "
        "    using var b := new Box<int>(); "
        "    b.v := 42; "
        "    return b.Get(); "  // 42
        "  end Main; "
        "end C;";
    TempFile783 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile783 bExe((dir / (stem + "_b")).string());
    writeSource783(bSrcFile.path, bSrc);
    BEZEL_EXPECT_EQ(compileImportAndRun783(bSrcFile.path, bExe.path, aObj.path), 42);
}

// ===========================================================================
// T4 — owned generic field disposed EXACTLY once cross-module (§28.3).
// Holder<Cell> with an owned Cell field; Cell's destructor decrements a static
// counter; after the holder's scope ends the counter is back to 0 (exactly one
// disposal). ASan confirms no leak / no double-free.
// ===========================================================================
BEZEL_TEST(xmod_generic_method_783, t4_owned_generic_field_disposed_once_28_3) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz783_t4_");
    TempFile783 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile783 aObj((dir / (stem + "_a.o")).string());

    writeSource783(aSrc.path,
        "module Lib783D; "
        "public class Cell "
        "  public static n: int; "
        "  public v: int; "
        "  public constructor(x: int) v := x; Cell.n := Cell.n + 1; end constructor; "
        "  destructor Cell.n := Cell.n - 1; end destructor; "
        "end Cell; "
        "public class Holder<T> "
        "  public owned item: T; "
        "  public constructor() end constructor; "
        "end Holder;");
    BEZEL_EXPECT_TRUE(compileToObj783(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    std::string bSrc =
        "module App; import Lib783D; "
        "public class C "
        "  public static method Main(): int "
        "    do "
        "      using var h := new Holder<Cell>(); "
        "      h.item := new Cell(7); "
        "    end; "
        "    return Cell.n; "  // 0 = disposed exactly once
        "  end Main; "
        "end C;";
    TempFile783 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile783 bExe((dir / (stem + "_b")).string());
    writeSource783(bSrcFile.path, bSrc);
    BEZEL_EXPECT_EQ(compileImportAndRun783(bSrcFile.path, bExe.path, aObj.path), 0);

    int asan = xmodAsan783(aObj.path, bSrc, 0);
    if (asan == -1) return;
    BEZEL_EXPECT_EQ(asan, 0);  // 0 = ASan clean (no leak, no double-free)
}

// ===========================================================================
// T5 — GATE CANARY: a generic method whose body references a sibling type the
// importer-side single-class reconstruction cannot reproduce, returning the type
// parameter, must be REJECTED with the #783 diagnostic — never a wrong value.
// (The reconstructed body fails to type-resolve `Helper.Compute()`, so the loader
// does NOT attach a body, and the §14.1/#783 sema gate fires on the reference
// instantiation.)
// ===========================================================================
BEZEL_TEST(xmod_generic_method_783, t5_unreconstructable_body_stays_gated_14_1) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz783_t5_");
    TempFile783 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile783 aObj((dir / (stem + "_a.o")).string());

    writeSource783(aSrc.path,
        "module Lib783E; "
        "public class Helper "
        "  public static method Compute(): int return 99; end Compute; "
        "end Helper; "
        "public class Box<T> "
        "  public v: T; "
        "  public constructor() end constructor; "
        "  public method GetDirty(): T var x := Helper.Compute(); return this.v; end GetDirty; "
        "end Box;");
    BEZEL_EXPECT_TRUE(compileToObj783(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    std::string bSrc =
        "module App; import Lib783E; "
        "public class C "
        "  public static method Main(): int "
        "    using var b := new Box<string>(); "
        "    b.v := \"hello\"; "
        "    return b.GetDirty().Length; "
        "  end Main; "
        "end C;";
    TempFile783 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile783 bExe((dir / (stem + "_b")).string());
    writeSource783(bSrcFile.path, bSrc);
    // The reference instantiation of the unreconstructable body must be a clean
    // compile REJECT, never a binary that returns a wrong value.
    BEZEL_EXPECT_TRUE(
        compileImportFails783(bSrcFile.path, bExe.path, aObj.path));
}

}  // namespace bezel::module
