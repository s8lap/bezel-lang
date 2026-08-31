// Codegen + runtime — issue #994: an `owned` parameter that is used as a BORROW
// argument to another call must remain in the unwind-disposal set, so it is
// auto-disposed when the method later throws (§28.3 exception-path discharge).
//
// §28.3 (line ~4910): an `owned` parameter live when an exception propagates out
// of its method is auto-disposed during stack unwinding (the implicit
// try/finally). §28 (line ~4899, the "Local-variable read `x` where `x` is owned"
// table row): a read in a BORROW-consuming position — "a non-`owned` argument, an
// indexer or property access" — borrows the value and `x` RETAINS ownership; only
// a read in an owned-consuming position (an `owned` argument, an assignment to an
// owned slot, a `return` under an `owned` return type) transfers it out.
//
// THE BUG: the codegen unwind-eligibility walker (UnwindIneligibleWalker in
// LlvmBackend_Setup.cpp) treated EVERY non-`out` call argument as consuming. So
// passing an owned parameter `a` as a plain borrow argument `C.DoNothing(a)`
// wrongly marked `a` unwind-INELIGIBLE; a later `throw` then left `a` un-disposed
// — a silent LEAK on the exception path. (A field/property read on `a`, already a
// borrow in the walker, did not trigger it; only a method-call argument did.) The
// fix consults the callee's parameter modes: a borrow parameter retains the
// caller's ownership, so a borrow argument no longer taints the owned param.
//
// DISPOSE-EXACTLY-ONCE DISCIPLINE (no-GC): the owned param must be disposed on
// EXACTLY ONE path per propagation. On the THROW path the unwind handler disposes
// it (the leak this fixes). On the NORMAL path the body must itself discharge the
// param (sema §28.3 requires it — an undischarged owned param is a leak the
// ownership pass rejects), and the unwindOnly disposal is skipped on normal exit,
// so there is no double-free. A consuming argument (`owned` parameter callee, or
// `move`) DOES transfer the value out, so the caller's unwind must NOT dispose it
// — the fix spares only borrow arguments, never consuming ones.
//
// Each Cell counts net-live instances: the constructor increments the static
// counter, the destructor decrements it. `Main` returns the counter, so the exit
// code is the number of Cells still alive — 0 means every Cell was disposed
// exactly once (no leak, no double-free), 1 means a Cell leaked.
//
// Two checks (mirroring closure_refcount_test.cpp):
//   1. compileAndRun: the native exit code is the LOAD-BEARING leak detector here
//      (the leaked Cell stays pointer-reachable at exit, so LSan does not flag it,
//      but the live-count exit code does — buggy=1, fixed=0).
//   2. runUnderAsan: re-lower + link against an ASan-built runtime and run under
//      ASan to verify MEMORY SAFETY (no use-after-free / double-free) on the
//      normal and consuming paths. Skipped gracefully when no toolchain.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>

#include "codegen/LlvmBackend.hpp"
#include "driver/Driver.hpp"
#include "lexer/Lexer.hpp"
#include "ownership/Ownership.hpp"
#include "parser/Parser.hpp"
#include "sema/DefiniteAssignment.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "support/Diagnostic.hpp"
#include "test_harness.hpp"
#include "native_test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

namespace {

namespace fs = std::filesystem;

// Compile `source` to a temp native executable via the real Driver pipeline and
// run it; returns the exit code, or -1 on compile/link/run failure.
int compileAndRun(const std::string& source) {
    static int counter = 0;
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_994_" + std::to_string(counter++);
    std::string src = (dir / (stem + ".bzl")).string();
    std::string exe = (dir / stem).string();
    {
        std::ofstream out(src);
        out << source;
    }
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileFile(src, exe);
    int code = -1;
    if (r.success) {
        int status = std::system(exe.c_str());
        if (status != -1 && WIFEXITED(status)) code = WEXITSTATUS(status);
    }
    std::remove(src.c_str());
    std::remove(exe.c_str());
    return code;
}

// Re-lower + link against the ASan runtime and run under ASan. Verifies MEMORY
// SAFETY (no use-after-free / double-free); a leak is tolerated (detect_leaks=0)
// — the live-count exit code in compileAndRun is the leak detector. Returns:
//   the program exit code on a clean run, 99 on an ASan-reported corruption,
//   or -1 when the toolchain is unavailable / a build step failed (SKIP).
int runUnderAsan(const std::string& source) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;  // toolchain unavailable

    static int counter = 0;
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_994_asan_" + std::to_string(counter++);
    std::string objPath = (dir / (stem + ".o")).string();
    std::string exe     = (dir / stem).string();
    {
        bezel::DiagnosticEngine diags;
        bezel::Lexer lexer(source, diags);
        bezel::Parser parser(lexer, diags);
        auto unit = parser.parseCompilationUnit();
        if (!unit) return -2;
        bezel::sema::SemaContext ctx(diags);
        bezel::sema::collectSymbols(*unit, ctx);
        bezel::sema::resolveTypes(*unit, ctx);
        bezel::sema::typecheckExpressions(*unit, ctx);
        bezel::sema::checkDefiniteAssignment(*unit, ctx);
        bezel::ownership::checkOwnership(*unit, ctx);
        if (diags.hasErrors()) return -2;
        llvm::LLVMContext llvmCtx;
        auto module = bezel::codegen::emitModule(*unit, ctx, llvmCtx);
        if (!module) return -2;
        std::string err;
        if (!bezel::codegen::emitObjectFile(*module, objPath, err)) return -2;
    }
    auto cleanup = [&] {
        std::remove(objPath.c_str());
        std::remove(exe.c_str());
    };
    std::string linkCmd = "clang++ -fsanitize=address -g " + objPath + " " +
                          rtObj + " -lm -lpthread -lstdc++ -o " + exe +
                          " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) { cleanup(); return -3; }
    // exitcode=99: a DISTINCT ASan failure code so a UAF/double-free can never be
    // confused with the program legitimately returning its live-count exit code.
    std::string runCmd =
        "ASAN_OPTIONS=detect_leaks=0:exitcode=99:abort_on_error=0 " + exe +
        " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return -4;
    if (!WIFEXITED(status)) return 99;  // crash/abort == corruption
    return WEXITSTATUS(status);
}

// Assert the live-count exit code AND (when the toolchain is present) that ASan
// reports no memory-safety corruption (the run exits with the same code, not 99).
void expectLiveCountAndNoCorruption(const std::string& source, int expected) {
    BEZEL_EXPECT_EQ(compileAndRun(source), expected);
    int asan = runUnderAsan(source);
    if (asan == -1) return;            // toolchain unavailable: skip the ASan leg
    BEZEL_EXPECT_EQ(asan, expected);   // no corruption (would be 99)
}

// --- Programs ------------------------------------------------------------

// §28.3 / §28 (HEADLINE #994): the owned param `a` is passed as a BORROW argument
// (`DoNothing`'s parameter is not `owned`) before the method throws. `a` is live
// and still owned on the exception edge, so the unwind handler must dispose it.
// Before the fix the borrow argument wrongly marked `a` unwind-ineligible → `a`
// leaked → Cell.n stays 1. Fixed: disposed on unwind → Cell.n == 0.
const char* kBorrowArgThenThrow =
    "module M; "
    "class AppErr: Exception end AppErr; "
    "class Cell "
    "  public static n: int := 0; "
    "  public constructor() n := n + 1; end constructor; "
    "  destructor n := n - 1; end destructor; "
    "end Cell; "
    "class C "
    "  public static method DoNothing(c: Cell) end DoNothing; "
    "  public static method Test(owned a: Cell) "
    "    C.DoNothing(a); "
    "    throw new AppErr(); "
    "  end Test; "
    "  public static method Main(): int "
    "    try C.Test(new Cell()); "
    "    catch AppErr => return Cell.n; "
    "    end; "
    "    return -1; "
    "  end Main; "
    "end C;";

// §28.3 (CONTROL): the same throw with NO prior borrow-call. This already worked
// (the base-case unwind disposal); it must keep working. Cell.n == 0.
const char* kThrowNoBorrowControl =
    "module M; "
    "class AppErr: Exception end AppErr; "
    "class Cell "
    "  public static n: int := 0; "
    "  public constructor() n := n + 1; end constructor; "
    "  destructor n := n - 1; end destructor; "
    "end Cell; "
    "class C "
    "  public static method Test(owned a: Cell) "
    "    throw new AppErr(); "
    "  end Test; "
    "  public static method Main(): int "
    "    try C.Test(new Cell()); "
    "    catch AppErr => return Cell.n; "
    "    end; "
    "    return -1; "
    "  end Main; "
    "end C;";

// §28.3 (NORMAL PATH, no double-free): the owned param is borrow-passed and then
// explicitly disposed on the normal exit path. The unwindOnly disposal is skipped
// on normal exit, so `a` is disposed EXACTLY once (Cell.n == 0). The fix keeping
// `a` unwind-eligible must NOT cause it to be disposed twice here.
const char* kBorrowArgThenDisposeNormal =
    "module M; "
    "class Cell "
    "  public static n: int := 0; "
    "  public constructor() n := n + 1; end constructor; "
    "  destructor n := n - 1; end destructor; "
    "end Cell; "
    "class C "
    "  public static method DoNothing(c: Cell) end DoNothing; "
    "  public static method Test(owned a: Cell) "
    "    C.DoNothing(a); "
    "    a.Dispose(); "
    "  end Test; "
    "  public static method Main(): int "
    "    C.Test(new Cell()); "
    "    return Cell.n; "
    "  end Main; "
    "end C;";

// §28.3 — the borrow callee actually USES the borrow (reads a field of `c`), then
// the caller throws. The value must be valid inside the borrow callee AND
// disposed on unwind. Cell.n == 0.
const char* kBorrowArgReadsFieldThenThrow =
    "module M; "
    "class AppErr: Exception end AppErr; "
    "class Cell "
    "  public static n: int := 0; "
    "  public v: int := 7; "
    "  public constructor() n := n + 1; end constructor; "
    "  destructor n := n - 1; end destructor; "
    "end Cell; "
    "class C "
    "  public static method Read(c: Cell): int return c.v; end Read; "
    "  public static method Test(owned a: Cell): int "
    "    var r: int := C.Read(a); "
    "    throw new AppErr(); "
    "  end Test; "
    "  public static method Main(): int "
    "    try _ := C.Test(new Cell()); "
    "    catch AppErr => return Cell.n; "
    "    end; "
    "    return -1; "
    "  end Main; "
    "end C;";

// §28.8 (CONSUMING, no double-free): the owned param `a` is TRANSFERRED to an
// `owned`-parameter callee (`Take(owned c)`), which sinks it into a `using var`
// that disposes it. After the transfer `a` is moved-from; the later throw must
// NOT make the caller's unwind dispose it again (that would double-free). The fix
// spares only BORROW arguments, so this consuming case still transfers. Cell.n
// == 0 (the sink disposed it once) and ASan reports no double-free.
const char* kConsumingArgTransfersThenThrow =
    "module M; "
    "class AppErr: Exception end AppErr; "
    "class Cell "
    "  public static n: int := 0; "
    "  public constructor() n := n + 1; end constructor; "
    "  destructor n := n - 1; end destructor; "
    "end Cell; "
    "class Sink "
    "  private owned _c: Cell?; "
    "  public constructor(owned c: Cell) this._c := c; end constructor; "
    "end Sink; "
    "class C "
    "  public static method Take(owned c: Cell) "
    "    using var s := new Sink(owned c); "  // §22.3 v0.169: named owned binding
    "  end Take; "
    "  public static method Test(owned a: Cell) "
    "    C.Take(owned a); "  // §22.3 v0.169: named owned binding transfers
    "    throw new AppErr(); "
    "  end Test; "
    "  public static method Main(): int "
    "    try C.Test(new Cell()); "
    "    catch AppErr => return Cell.n; "
    "    end; "
    "    return -1; "
    "  end Main; "
    "end C;";

} // namespace

BEZEL_TEST(owned_param_borrow_arg_994, borrow_arg_then_throw_disposes_on_unwind) {
    // The fix: Cell.n == 0 (was 1 — the owned param leaked on the throw path).
    expectLiveCountAndNoCorruption(kBorrowArgThenThrow, 0);
}

BEZEL_TEST(owned_param_borrow_arg_994, throw_no_borrow_control_still_disposes) {
    expectLiveCountAndNoCorruption(kThrowNoBorrowControl, 0);
}

BEZEL_TEST(owned_param_borrow_arg_994, borrow_arg_then_dispose_normal_once) {
    // Normal path: disposed exactly once (no double-free from the unwind entry).
    expectLiveCountAndNoCorruption(kBorrowArgThenDisposeNormal, 0);
}

BEZEL_TEST(owned_param_borrow_arg_994, borrow_arg_reads_field_then_throw) {
    expectLiveCountAndNoCorruption(kBorrowArgReadsFieldThenThrow, 0);
}

BEZEL_TEST(owned_param_borrow_arg_994, consuming_arg_still_transfers_no_double_free) {
    // The consuming (owned-param callee) transfer is unchanged: the callee owns
    // it, the caller's unwind does NOT dispose it — no double-free.
    expectLiveCountAndNoCorruption(kConsumingArgTransfersThenThrow, 0);
}
