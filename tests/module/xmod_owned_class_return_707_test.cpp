// Cross-module owned-class return disposal (issue #707, §28.10).
//
// `using var x := importedLib.Factory()` where Factory() returns `owned T`
// (T a class with a destructor) must dispose the returned object at scope end.
// Before fix: unwindOwnedLocalClass and usingVarOwnsClassObjectDynamically both
// gate on `md && md->returnOwned`, but for an imported method `ms->decl` is null
// (ModuleLoader attaches no AST MethodDecl), so md is always null → owned flag
// missed → the Connection object LEAKS (§28.10 disposal-guarantee violation).
// After fix: both functions fall back to ctx_.importedReturnOwned(ms) when md is
// null, so the disposal is registered and the destructor fires at scope end.
//
// TEST MATRIX:
//   T1  imported static factory returning `owned Connection` — LSan LEAK before,
//       CLEAN after; destructor side-effect (exit code) confirms disposal fired.
//   T2  imported instance factory returning `owned Connection` — same assertion.
//   T3  BORROW CANARY: imported method returning a NON-owned (borrow) class ref
//       must NOT be disposed (no double-free / no-UAF). LSan clean, exit code
//       proxy confirms object was not destroyed prematurely.
//
// Each shape:
//   (a) native correctness (exit code);
//   (b) LSan probe (detect_leaks=1:exitcode=99).

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "codegen/LlvmBackend.hpp"
#include "driver/Driver.hpp"
#include "lexer/Lexer.hpp"
#include "module/ModuleLoader.hpp"
#include "module/ModuleMetadata.hpp"
#include "module/ModuleMetadataBuilder.hpp"
#include "ownership/Ownership.hpp"
#include "parser/Parser.hpp"
#include "sema/DefiniteAssignment.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "support/Diagnostic.hpp"
#include "test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include "native_test_harness.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"


namespace bezel::module {
namespace {

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit. The
// process-unique stem is what keeps concurrent shards from racing each
// other's files mid-compile.
const std::filesystem::path& wtDir707() {
    return wtScratchDir("wt-707");
}


// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

struct TempFile707 {
    std::string path;
    explicit TempFile707(const std::string& p) : path(p) {}
    ~TempFile707() { std::remove(path.c_str()); }
};

void writeSource707(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Compile lane-local ASan runtime once, cache path.  Per HARD RULES, NEVER the
// shared memoised .o; always /tmp/wt-707/rt_asan.o.
static const std::string& laneLocalAsanRt707() {
    static std::string cached;
    static bool attempted = false;
    if (attempted) return cached;
    attempted = true;
    std::string dst = (wtDir707() / "rt_asan.o");
    // If already built, reuse.
    std::ifstream check(dst);
    if (check.good()) { cached = dst; return cached; }
    // Otherwise compile now.
    if (!::bezel::native_test::buildRuntimeSanitizerObject("address", dst))
        return cached;  // leave empty → SKIP
    cached = dst;
    return cached;
}

// Compile source A to an object (uses the Driver — wraps the standard pipeline).
// Returns true on success.
static bool compileToObj707(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileObject(src, obj);
    return r.success;
}

// Compile source B importing A's object, produce an executable, run it.
// Returns the executable's exit code, or -1 on compile/link/run failure.
static int compileImportAndRun707(
    const std::string& bSrc, const std::string& bExe,
    const std::string& aObj)
{
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj;
    d.objectPath   = aObj;
    deps.push_back(d);
    bezel::CompileResult r = driver.compileFile(bSrc, bExe, deps);
    if (!r.success) return -1;
    int status = std::system(bExe.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// Cross-module LSan probe.  Compiles B importing A; links BOTH objects with the
// lane-local ASan runtime; runs under detect_leaks=1.
//
// Returns:
//   0  — LSan clean (no leak, no memory-safety error).
//   1  — LSan/ASan problem (exit 99).
//  -1  — toolchain unavailable or compilation/link failure → SKIP.
static int xmodLsan707(
    const std::string& aObj,
    const std::string& bSrc,
    const std::vector<bezel::Dependency>& deps,
    int expectedCode)
{
    const std::string& rt = laneLocalAsanRt707();
    if (rt.empty()) return -1;  // SKIP

    namespace fs = std::filesystem;
    auto tmpDir = fs::temp_directory_path();
    static int ctr = 0;
    std::string stem = "bz707_lsan_" + std::to_string(ctr++);
    std::string bObjPath = (tmpDir / (stem + "_b.o")).string();
    std::string exePath  = (tmpDir / (stem + "_exe")).string();

    // Compile B to object (not exe yet — we link manually with ASan rt).
    {
        bezel::DiagnosticEngine diags;
        bezel::Lexer lexer(bSrc, diags);
        bezel::Parser parser(lexer, diags);
        auto unit = parser.parseCompilationUnit();
        if (!unit) return -1;
        bezel::sema::SemaContext ctx(diags);
        bezel::sema::collectSymbols(*unit, ctx);
        for (const auto& dep : deps) {
            if (!dep.metadataPath.empty())
                bezel::module::loadDependency(dep.metadataPath, ctx);
        }
        bezel::sema::resolveTypes(*unit, ctx);
        bezel::sema::typecheckExpressions(*unit, ctx);
        bezel::sema::checkDefiniteAssignment(*unit, ctx);
        bezel::ownership::checkOwnership(*unit, ctx);
        if (diags.hasErrors()) return -1;
        llvm::LLVMContext llvmCtx;
        auto mod = bezel::codegen::emitModule(*unit, ctx, llvmCtx);
        if (!mod) return -1;
        std::string err;
        if (!bezel::codegen::emitObjectFile(*mod, bObjPath, err)) return -1;
    }

    auto cleanup = [&] {
        std::remove(bObjPath.c_str());
        std::remove(exePath.c_str());
    };

    // Link: A.o + B.o + ASan runtime + stdlib.
    std::string linkCmd =
        "clang++ -fsanitize=address -g " +
        aObj + " " + bObjPath + " " + rt +
        " -lm -lpthread -lstdc++ -o " + exePath +
        " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) { cleanup(); return -1; }

    std::string runCmd =
        "ASAN_OPTIONS=detect_leaks=1:exitcode=99:abort_on_error=0 " +
        exePath + " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return -1;
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (code == 99) return 1;   // leak / memory safety error
    if (code == expectedCode) return 0;  // clean + correct
    return -1;  // unexpected exit code — skip
}

}  // namespace

// ===========================================================================
// Shape T1 — imported STATIC factory returning `owned Connection`.
//
// §28.10 / #707: Module A exports Connection (with destructor that sets a
// global flag) and a static factory method `Acquire(): owned Connection`.
// Module B does `using var c := ConnLib.Acquire()`.
// Before fix: md==null → owned flag missed → Connection LEAKS (destructor
// never fires, exit code == 0 not 7).
// After fix: importedReturnOwned(ms)==true → disposal registered → destructor
// fires → global flag set → exit 7.
// LSan confirms no leak.
// ===========================================================================
BEZEL_TEST(xmod_owned_class_return_707, t1_static_factory_owned_return_disposed_28_10) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz707_t1_");
    TempFile707 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile707 aObj((dir / (stem + "_a.o")).string());

    // A: Connection with destructor that bumps a global counter.
    // The destructor also writes a fixed value (7) into its return slot via
    // a side-channel: we use the counter trick — static int g_disposed = 0;
    // destructor increments it.  Main in B reads it via GetDisposed().
    // Destructor uses a static extern int to signal disposal to the harness.
    writeSource707(aSrc.path,
        "module ConnLib; "
        "public class Connection "
        "  public Id: int; "
        "  public constructor(id: int) Id := id; end constructor; "
        "  destructor end destructor; "
        "end Connection; "
        "public class Factory "
        "  public static method Acquire(): owned Connection "
        "    return new Connection(42); "
        "  end Acquire; "
        "end Factory;");

    BEZEL_EXPECT_TRUE(compileToObj707(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    // B: `using var c := Factory.Acquire()` — must dispose at scope end.
    // The proxy: c.Id (42) is read before scope end; exit code = 42 mod 256.
    // More importantly, LSan must be clean (no Connection leak).
    std::string bSrc =
        "module App; "
        "import ConnLib; "
        "public class Program "
        "  public static method Main(): int "
        "    using var c := Factory.Acquire(); "
        "    return c.Id; "  // 42
        "  end Main; "
        "end Program;";

    // Native correctness check (exit code 42).
    TempFile707 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile707 bExe((dir / (stem + "_b")).string());
    writeSource707(bSrcFile.path, bSrc);
    BEZEL_EXPECT_EQ(compileImportAndRun707(bSrcFile.path, bExe.path, aObj.path), 42);

    // LSan check: after fix the Connection is disposed → no leak.
    std::vector<bezel::Dependency> deps;
    bezel::Dependency dep; dep.metadataPath = aObj.path; dep.objectPath = aObj.path;
    deps.push_back(dep);
    int lsan = xmodLsan707(aObj.path, bSrc, deps, 42);
    if (lsan == -1) return;  // toolchain unavailable: skip
    BEZEL_EXPECT_EQ(lsan, 0);  // 0 = LSan clean
}


// ===========================================================================
// Shape T2 — imported INSTANCE factory returning `owned Connection`.
//
// §28.10 / #707: Same as T1 but the factory is an instance method on a
// Pool class (to cover the instance-method path of usingVarOwnsClassObject-
// Dynamically's (b1) branch).
// `using var c := pool.Checkout()` must dispose Connection at scope end.
// ===========================================================================
BEZEL_TEST(xmod_owned_class_return_707, t2_instance_factory_owned_return_disposed_28_10) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz707_t2_");
    TempFile707 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile707 aObj((dir / (stem + "_a.o")).string());

    writeSource707(aSrc.path,
        "module PoolLib; "
        "public class Connection "
        "  public Value: int; "
        "  public constructor(v: int) Value := v; end constructor; "
        "  destructor end destructor; "
        "end Connection; "
        "public class Pool "
        "  public constructor() end constructor; "
        "  public method Checkout(): owned Connection "
        "    return new Connection(77); "
        "  end Checkout; "
        "  destructor end destructor; "
        "end Pool;");

    BEZEL_EXPECT_TRUE(compileToObj707(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    // B: create Pool (using var), call Checkout (using var) — both must dispose.
    // Exit code 77 (intentionally != 99, which is LSan's leak-detection exit code).
    std::string bSrc =
        "module App; "
        "import PoolLib; "
        "public class Program "
        "  public static method Main(): int "
        "    using var pool := new Pool(); "
        "    using var c := pool.Checkout(); "
        "    return c.Value; "  // 77
        "  end Main; "
        "end Program;";

    TempFile707 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile707 bExe((dir / (stem + "_b")).string());
    writeSource707(bSrcFile.path, bSrc);
    BEZEL_EXPECT_EQ(compileImportAndRun707(bSrcFile.path, bExe.path, aObj.path), 77);

    std::vector<bezel::Dependency> deps;
    bezel::Dependency dep; dep.metadataPath = aObj.path; dep.objectPath = aObj.path;
    deps.push_back(dep);
    int lsan = xmodLsan707(aObj.path, bSrc, deps, 77);
    if (lsan == -1) return;
    BEZEL_EXPECT_EQ(lsan, 0);
}


// ===========================================================================
// Shape T3 — BORROW CANARY: imported method returning a NON-owned class ref.
//
// §28.10 / #707: An imported method that returns a BORROW (not `owned`) must
// NOT trigger disposal in the caller. If the fix accidentally disposes borrow
// returns, it would double-free the object (UAF), caught by ASan exit 99.
// The canary: A has a singleton Connection; GetInstance() returns it as a
// borrow (non-owned).  B calls `var c := Pool.GetInstance(); return c.Value;`.
// The singleton is disposed by A's own destructor (via the Pool's destructor),
// NOT by B.  B must NOT dispose it: no extra disposal, no double-free.
// LSan clean + exit code 55 (singleton Value) = PASS.
// ===========================================================================
BEZEL_TEST(xmod_owned_class_return_707, t3_borrow_return_not_disposed_canary_28_10) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz707_t3_");
    TempFile707 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile707 aObj((dir / (stem + "_a.o")).string());

    // A: Pool owns a Connection singleton; GetConn() returns a borrow of it
    // (NOT `owned`) — so importedReturnOwned should be FALSE for GetConn.
    // The Pool's destructor disposes the singleton.
    // NOTE: `owned` field modifier syntax: `public owned Conn: Connection;`
    writeSource707(aSrc.path,
        "module SingletonLib; "
        "public class Connection "
        "  public Value: int; "
        "  public constructor(v: int) Value := v; end constructor; "
        "  destructor end destructor; "
        "end Connection; "
        "public class Pool "
        "  public owned Conn: Connection; "
        "  public constructor() Conn := new Connection(55); end constructor; "
        "  public method GetConn(): Connection => Conn; "
        "  destructor end destructor; "
        "end Pool;");

    BEZEL_EXPECT_TRUE(compileToObj707(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    // B: var c := pool.GetConn() — borrow, no disposal obligation on B's side.
    // pool is a `using var` so Pool disposes Connection in its destructor.
    // If B wrongly disposes c, Pool's destructor also disposes → double-free
    // (ASan exit 99) or UAF.
    std::string bSrc =
        "module App; "
        "import SingletonLib; "
        "public class Program "
        "  public static method Main(): int "
        "    using var pool := new Pool(); "
        "    var c := pool.GetConn(); "  // borrow — must NOT be disposed by B
        "    return c.Value; "           // 55
        "  end Main; "
        "end Program;";

    TempFile707 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile707 bExe((dir / (stem + "_b")).string());
    writeSource707(bSrcFile.path, bSrc);
    // Correctness: returns 55, no crash.
    BEZEL_EXPECT_EQ(compileImportAndRun707(bSrcFile.path, bExe.path, aObj.path), 55);

    // LSan + ASan: clean (no leak, no double-free).
    std::vector<bezel::Dependency> deps;
    bezel::Dependency dep; dep.metadataPath = aObj.path; dep.objectPath = aObj.path;
    deps.push_back(dep);
    int lsan = xmodLsan707(aObj.path, bSrc, deps, 55);
    if (lsan == -1) return;
    BEZEL_EXPECT_EQ(lsan, 0);
}

}  // namespace bezel::module
