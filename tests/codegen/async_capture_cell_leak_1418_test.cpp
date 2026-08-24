// Async / capture-cell disposal — the capture CELL of a frame-local that a
// lambda captures (the `calloc` boxLocalSlot allocates so the outer scope and
// the lambda share the variable) must be freed at scope end in an ASYNC method
// just as in a synchronous one (spec §27.5 captured-variable sharing; §28.9
// runtime-managed reclaim; §30 async state machine).
//
// Two defects are pinned here:
//
//   #1418 — an async method's lambda-captured value local leaked its capture
//   cell.  Root cause (codegen): emitFunctionBody's async routing returned
//   before Setup ran collectCellFreeEligible / collectBoxCaptureLocals and set
//   the *EligibleFn_ keys to the resume function, so lowerLambda never recorded
//   the captured cell in lastLambdaCapturedCells_ and lowerLocalDecl never
//   registered its boxedCellFree.  Fixed by mirroring the synchronous
//   eligibility pre-scans in emitAsyncMethod / emitAsyncLambda.
//
//   #1418 (return-slot sibling) — `return count` where `count` is a captured
//   value local ALSO leaked in a SYNCHRONOUS method: the return-value ownership-
//   transfer exclusion (meant for a returned `using var`) erased the local's
//   boxedCellFree because the returned local's slot IS the shared cell-holder.
//   Returning a value local yields a COPY of the cell's contents, not the cell,
//   so the cell must still be freed.  Fixed by preserving boxedCellFree /
//   boxedCellFreeDeferred entries in that exclusion.
//
// Each ASan test compiles a shape to a NATIVE binary linked against an
// ASan+LSan-instrumented runtime and asserts zero leaks; each JIT test asserts
// the behavioural result is unchanged.  The recipe mirrors
// cts_register_timer_1413_test.cpp / taskscope_asan_808_test.cpp.

#include <cstdlib>
#include <filesystem>
#include <string>
#include <sys/wait.h>

#include "codegen/LlvmBackend.hpp"
#include "lexer/Lexer.hpp"
#include "ownership/Ownership.hpp"
#include "parser/Parser.hpp"
#include "sema/DefiniteAssignment.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration)
#include "support/codegen_support.hpp"
#include "support/Diagnostic.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"
#include "native_test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

namespace fs = std::filesystem;
using bezel::test::runAot;

namespace {

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit.
static const std::filesystem::path& wtDirAsynccell1418() {
    return wtScratchDir("wt-asynccell1418");
}

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// AddressSanitizer / LeakSanitizer verification (mirrors
// cts_register_timer_1413_test.cpp).  Returns 0 = clean; 1 = sanitizer fired or
// wrong exit code; -1 = toolchain unavailable (SKIP).
int runUnderAsan(const std::string& source, int expectedExitCode) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;

    auto dir = wtDirAsynccell1418();
    std::string stem = unique_stem("bezel_asynccell1418");
    std::string objPath = (dir / (stem + ".o")).string();
    std::string exe     = (dir / stem).string();

    {
        bezel::DiagnosticEngine diags;
        bezel::Lexer lexer(source, diags);
        bezel::Parser parser(lexer, diags);
        auto unit = parser.parseCompilationUnit();
        if (!unit) return -1;
        bezel::sema::SemaContext ctx(diags);
        bezel::sema::collectSymbols(*unit, ctx);
        bezel::sema::resolveTypes(*unit, ctx);
        bezel::sema::typecheckExpressions(*unit, ctx);
        bezel::sema::checkDefiniteAssignment(*unit, ctx);
        bezel::ownership::checkOwnership(*unit, ctx);
        if (diags.hasErrors()) return -1;
        llvm::LLVMContext llvmCtx;
        auto module = bezel::codegen::emitModule(*unit, ctx, llvmCtx);
        if (!module) return -1;
        std::string err;
        if (!bezel::codegen::emitObjectFile(*module, objPath, err)) return -1;
    }

    auto cleanup = [&] {
        std::remove(objPath.c_str());
        std::remove(exe.c_str());
    };
    std::string linkCmd = "clang++ -fsanitize=address -g " + objPath + " " +
                          rtObj + " -lm -lpthread -lstdc++ -o " + exe +
                          " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) { cleanup(); return -1; }
    std::string runCmd = "ASAN_OPTIONS=detect_leaks=1:exitcode=99 " + exe +
                         " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return -1;
    if (!WIFEXITED(status)) return 1;              // crashed/aborted
    int code = WEXITSTATUS(status);
    if (code == 99) return 1;                      // sanitizer reported a leak
    return (code == expectedExitCode) ? 0 : 1;
}

void expectAsanClean(const std::string& source, int expectedExitCode) {
    int a = runUnderAsan(source, expectedExitCode);
    if (a == -1) return;   // toolchain unavailable: SKIP
    BEZEL_EXPECT_EQ(a, 0);
}

// ---- Source shapes -------------------------------------------------------

// #1418 verbatim: an async method whose async lambda captures the value local
// `count`, awaited once, returned directly.  Main returns 0 iff count == 1.
const char* kAsyncReturnCount =
    "module M; import System.Threading; "
    "class Prog "
    "  public static async method Run(): Task<int> "
    "    var count := 0; "
    "    var f := async lambda() => do "
    "      await Task.Delay(2); "
    "      count := count + 1; "
    "    end; "
    "    await f(); "
    "    return count; "
    "  end Run; "
    "  public static async method Main(): Task<int> "
    "    var n := await Prog.Run(); "
    "    if n = 1 then return 0; end; "
    "    return 1; "
    "  end Main; "
    "end Prog;";

// Sync sibling: the return-slot exclusion must not erase the capture cell's
// free when `return count` names a captured value local.
const char* kSyncReturnCount =
    "module M; "
    "class Prog "
    "  public static method Run(): int "
    "    var count := 0; "
    "    var f := lambda() => do count := count + 1; end; "
    "    f(); "
    "    return count; "
    "  end Run; "
    "  public static method Main(): int "
    "    var n := Prog.Run(); "
    "    if n = 1 then return 0; end; "
    "    return 1; "
    "  end Main; "
    "end Prog;";

// Multiple captures: two value locals shared with one async lambda; both cells
// must be freed (1 + 10 = 11).
const char* kAsyncMultiCapture =
    "module M; import System.Threading; "
    "class Prog "
    "  public static async method Run(): Task<int> "
    "    var a := 0; var b := 0; "
    "    var f := async lambda() => do "
    "      await Task.Delay(2); "
    "      a := a + 1; b := b + 10; "
    "    end; "
    "    await f(); "
    "    return a + b; "
    "  end Run; "
    "  public static async method Main(): Task<int> "
    "    var n := await Prog.Run(); "
    "    if n = 11 then return 0; end; "
    "    return 1; "
    "  end Main; "
    "end Prog;";

// Double await of the same captured async lambda: the single shared cell is
// freed exactly once (no double-free, no leak); count == 2.
const char* kAsyncDoubleAwait =
    "module M; import System.Threading; "
    "class Prog "
    "  public static async method Run(): Task<int> "
    "    var count := 0; "
    "    var f := async lambda() => do "
    "      await Task.Delay(2); "
    "      count := count + 1; "
    "    end; "
    "    await f(); "
    "    await f(); "
    "    return count; "
    "  end Run; "
    "  public static async method Main(): Task<int> "
    "    var n := await Prog.Run(); "
    "    if n = 2 then return 0; end; "
    "    return 1; "
    "  end Main; "
    "end Prog;";

// Async method, a non-async capturing lambda declared in a nested block, called
// directly (no subscription).  The captured `using var box`'s cell is freed via
// the deferred (owner-disposal-first) path once the async eligibility pre-scan
// runs (box.V == 1).
const char* kAsyncCaptureAndCall =
    "module M; import System.Threading; "
    "class Box public V: int := 0; public method Bump() V := V + 1; end Bump; end Box; "
    "class C "
    "  public static async method Main(): Task<int> "
    "    using var box := new Box(); "
    "    do var f := lambda() => do box.Bump(); end; f(); end; "
    "    await Task.Delay(5); "
    "    if box.V = 1 then return 0; end; "
    "    return 1; "
    "  end Main; "
    "end C;";

// Eligibility-state save/restore: an async lambda (`g`) lowered inline
// RECOMPUTES the capture-cell-free eligibility sets for its own body and keys
// them to its resume fn; a SUBSEQUENT capturing lambda (`h`) in the same
// enclosing async method must still see the enclosing method's eligibility, or
// `y`'s cell would be mis-registered (leak or double-free).  Guards the
// save/restore in lowerLambda's async branch (y == 1).
const char* kAsyncLambdaThenCapture =
    "module M; import System.Threading; "
    "class Prog "
    "  public static async method Run(): Task<int> "
    "    var g := async lambda() => do await Task.Delay(1); end; "
    "    var y := 0; "
    "    var h := lambda() => do y := y + 1; end; "
    "    h(); "
    "    await g(); "
    "    return y; "
    "  end Run; "
    "  public static async method Main(): Task<int> "
    "    var n := await Prog.Run(); "
    "    if n = 1 then return 0; end; "
    "    return 1; "
    "  end Main; "
    "end Prog;";

// Non-capturing async lambda: no cell is allocated, so nothing to free — must
// stay clean (guards against the fix registering a spurious free).
const char* kAsyncNonCapturing =
    "module M; import System.Threading; "
    "class Prog "
    "  public static async method Run(): Task<int> "
    "    var f := async lambda() => do await Task.Delay(2); end; "
    "    await f(); "
    "    return 0; "
    "  end Run; "
    "  public static async method Main(): Task<int> "
    "    return await Prog.Run(); "
    "  end Main; "
    "end Prog;";

}  // namespace

// ===========================================================================
// ASan / LSan probes (§27.5 / §28.9 / §30) — zero leaks.
// ===========================================================================

// #1418: the async capture cell is reclaimed at scope end (was a 4-byte leak).
BEZEL_TEST(async_capture_cell_1418_asan, async_return_captured_local_no_leak) {
    expectAsanClean(kAsyncReturnCount, 0);
}

// #1418 sync sibling: `return count` no longer erases the cell free.
BEZEL_TEST(async_capture_cell_1418_asan, sync_return_captured_local_no_leak) {
    expectAsanClean(kSyncReturnCount, 0);
}

// Two captured value locals → two cells, both reclaimed.
BEZEL_TEST(async_capture_cell_1418_asan, async_multi_capture_no_leak) {
    expectAsanClean(kAsyncMultiCapture, 0);
}

// The shared cell of a twice-awaited async lambda is freed exactly once.
BEZEL_TEST(async_capture_cell_1418_asan, async_double_await_no_leak) {
    expectAsanClean(kAsyncDoubleAwait, 0);
}

// Async method + nested non-async capturing lambda: the captured using-var's
// cell is reclaimed via the deferred owner path (async eligibility pre-scan).
BEZEL_TEST(async_capture_cell_1418_asan, async_capture_and_call_no_leak) {
    expectAsanClean(kAsyncCaptureAndCall, 0);
}

// A non-capturing async lambda allocates no cell — stays clean.
BEZEL_TEST(async_capture_cell_1418_asan, async_non_capturing_stays_clean) {
    expectAsanClean(kAsyncNonCapturing, 0);
}

// Eligibility save/restore: a capturing lambda after an inline async lambda in
// the same async method still frees its cell.
BEZEL_TEST(async_capture_cell_1418_asan, async_lambda_then_capture_no_leak) {
    expectAsanClean(kAsyncLambdaThenCapture, 0);
}

// ===========================================================================
// JIT behaviour (§27.5 shared-cell semantics; §30 async values) — the fixes
// change memory discipline only; the observed values are unchanged.
// ===========================================================================

BEZEL_TEST(async_capture_cell_1418_jit, async_return_captured_local_value) {
    BEZEL_EXPECT_EQ(run(kAsyncReturnCount), 0);
}

BEZEL_TEST(async_capture_cell_1418_jit, sync_return_captured_local_value) {
    BEZEL_EXPECT_EQ(run(kSyncReturnCount), 0);
}

BEZEL_TEST(async_capture_cell_1418_jit, async_multi_capture_value) {
    BEZEL_EXPECT_EQ(run(kAsyncMultiCapture), 0);
}

BEZEL_TEST(async_capture_cell_1418_jit, async_double_await_value) {
    BEZEL_EXPECT_EQ(run(kAsyncDoubleAwait), 0);
}

BEZEL_TEST(async_capture_cell_1418_jit, async_capture_and_call_value) {
    BEZEL_EXPECT_EQ(run(kAsyncCaptureAndCall), 0);
}

BEZEL_TEST(async_capture_cell_1418_jit, async_lambda_then_capture_value) {
    BEZEL_EXPECT_EQ(run(kAsyncLambdaThenCapture), 0);
}
