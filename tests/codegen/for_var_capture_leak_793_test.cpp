// fix #793 / §27.5 / §16.5: a for-loop variable captured by a closure is boxed
// into a FRESH heap cell each iteration (§16.5 fresh binding, via
// boxLocalSlot(forLoopVar=true)). Those per-iteration cells were never freed — an
// O(iterations) leak. When collectCellFreeEligible proves the for-var is captured
// ONLY by non-escaping lambdas (it already classifies for-vars, keyed by the
// ForStmt), each iteration's cell is now freed at the end of that iteration's
// body — after the capturing closure is released (LIFO: an outer per-iteration
// disposal scope wraps the body's inner scope). An ESCAPING capture's cell is NOT
// freed (it outlives the loop): it keeps leaking, the safe direction — verified to
// produce no UAF / double-free.
//
// LSan-verified (native compile path).

#include <cstdio>
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
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"
#include "native_test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

namespace {
namespace fs = std::filesystem;

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit.
static const std::filesystem::path& wtDir793() {
    return wtScratchDir("wt-793");
}

int compileAndRun(const std::string& source) {
    auto dir = wtDir793();
    std::string stem = unique_stem("bezel_793");
    std::string src = (dir / (stem + ".bzl")).string();
    std::string exe = (dir / stem).string();
    { std::ofstream out(src); out << source; }
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

// 0 = LSan clean; 1 = sanitizer reported leak/UAF/DF; -1 = toolchain unavailable.
int runUnderLeakSanitizer(const std::string& source) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;
    auto dir = wtDir793();
    std::string stem = unique_stem("bezel_793_lsan");
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
    auto cleanup = [&] { std::remove(objPath.c_str()); std::remove(exe.c_str()); };
    std::string linkCmd = "clang++ -fsanitize=address -g " + objPath + " " +
                          rtObj + " -lm -lpthread -lstdc++ -o " + exe +
                          " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) { cleanup(); return -1; }
    std::string runCmd =
        "ASAN_OPTIONS=detect_leaks=1:exitcode=99 " + exe + " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return -1;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 1;
    return 0;
}

void expectValueAndLeakFree(const std::string& source, int expected) {
    BEZEL_EXPECT_EQ(compileAndRun(source), expected);
    int lsan = runUnderLeakSanitizer(source);
    if (lsan == -1) return;
    BEZEL_EXPECT_EQ(lsan, 0);
}

// Assert correctness AND no MEMORY-SAFETY error (UAF/double-free), tolerating a
// leak (the escaping-capture cell is deliberately not freed).
void expectValueAndNoCorruption(const std::string& source, int expected) {
    BEZEL_EXPECT_EQ(compileAndRun(source), expected);
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return;
    auto dir = wtDir793();
    std::string stem = unique_stem("bezel_793_safe");
    std::string objPath = (dir / (stem + ".o")).string();
    std::string exe     = (dir / stem).string();
    {
        bezel::DiagnosticEngine diags;
        bezel::Lexer lexer(source, diags);
        bezel::Parser parser(lexer, diags);
        auto unit = parser.parseCompilationUnit();
        if (!unit) return;
        bezel::sema::SemaContext ctx(diags);
        bezel::sema::collectSymbols(*unit, ctx);
        bezel::sema::resolveTypes(*unit, ctx);
        bezel::sema::typecheckExpressions(*unit, ctx);
        bezel::sema::checkDefiniteAssignment(*unit, ctx);
        bezel::ownership::checkOwnership(*unit, ctx);
        if (diags.hasErrors()) return;
        llvm::LLVMContext llvmCtx;
        auto module = bezel::codegen::emitModule(*unit, ctx, llvmCtx);
        if (!module) return;
        std::string err;
        if (!bezel::codegen::emitObjectFile(*module, objPath, err)) return;
    }
    auto cleanup = [&] { std::remove(objPath.c_str()); std::remove(exe.c_str()); };
    std::string linkCmd = "clang++ -fsanitize=address -g " + objPath + " " +
                          rtObj + " -lm -lpthread -lstdc++ -o " + exe +
                          " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) { cleanup(); return; }
    std::string runCmd =
        "ASAN_OPTIONS=detect_leaks=0:exitcode=99:abort_on_error=0 " + exe +
        " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return;
    BEZEL_EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == expected);
}
}  // namespace

// §27.5 (the fix): a for-var captured by a single non-escaping lambda — each
// iteration's fresh cell is freed at iteration end. Previously O(iterations)
// leaked. LSan-clean; sum 1+2+3+4 = 10.
BEZEL_TEST(for_var_capture_leak_793, single_non_escaping_capture_lsan_clean) {
    expectValueAndLeakFree(
        "module M; class C public static method Main(): int "
        "  var acc: int := 0; "
        "  for var i in 1 .. 4 do var f: () -> int := lambda() => i; acc := acc + f(); end; "
        "  return acc; "
        "end Main; end C;", 10);
}

// §27.5: nested loops, both for-vars captured — each loop level has its own
// per-iteration disposal scope; cells freed at each level independently. 6*6 = 36.
BEZEL_TEST(for_var_capture_leak_793, nested_loops_capture_lsan_clean) {
    expectValueAndLeakFree(
        "module M; class C public static method Main(): int "
        "  var acc: int := 0; "
        "  for var i in 1 .. 3 do for var j in 1 .. 3 do "
        "    var f: () -> int := lambda() => i * j; acc := acc + f(); end; end; "
        "  return acc; "
        "end Main; end C;", 36);
}

// §27.5: a `break` out of the loop frees the current iteration's cell on the
// terminating exit (the loop's recorded scope depth is below the per-iteration
// free scope). 0+1+2+3 = 6.
BEZEL_TEST(for_var_capture_leak_793, break_frees_current_cell_lsan_clean) {
    expectValueAndLeakFree(
        "module M; class C public static method Main(): int "
        "  var acc: int := 0; "
        "  for var i in 0 .. 100 do var f: () -> int := lambda() => i; acc := acc + f(); "
        "    if i = 3 then break; end; end; "
        "  return acc; "
        "end Main; end C;", 6);
}

// §27.5: a `continue` frees the current iteration's cell before the next
// iteration's fresh box. Skips i<2; sum 2+3+4 = 9.
BEZEL_TEST(for_var_capture_leak_793, continue_frees_cell_lsan_clean) {
    expectValueAndLeakFree(
        "module M; class C public static method Main(): int "
        "  var acc: int := 0; "
        "  for var i in 0 .. 4 do if i < 2 then continue; end; "
        "    var f: () -> int := lambda() => i; acc := acc + f(); end; "
        "  return acc; "
        "end Main; end C;", 9);
}

// §27.5: TWO non-escaping lambdas capture the same for-var — they share ONE cell
// (the body-entry pre-box), freed exactly once at iteration end (no double-free).
// (1+2)+(2+4)+(3+6) = 18.
BEZEL_TEST(for_var_capture_leak_793, multi_capture_shared_cell_lsan_clean) {
    expectValueAndLeakFree(
        "module M; class C public static method Main(): int "
        "  var acc: int := 0; "
        "  for var i in 1 .. 3 do "
        "    var f: () -> int := lambda() => i; var g: () -> int := lambda() => i * 2; "
        "    acc := acc + f() + g(); end; "
        "  return acc; "
        "end Main; end C;", 18);
}

// §27.5 (the soundness boundary): an ESCAPING capture — the closure is stored in
// an outer variable that outlives the loop and is invoked AFTER it. The for-var
// is NOT in cellFreeEligible_, so its cell is NOT freed at iteration end (it would
// be a UAF — the escaped closure still reads it). The last cell leaks (tolerated);
// the value is correct (the last i = 4) and there is NO double-free / UAF.
BEZEL_TEST(for_var_capture_leak_793, escaping_capture_no_corruption) {
    expectValueAndNoCorruption(
        "module M; class C public static method Main(): int "
        "  var captured: () -> int := lambda() => 0; "
        "  for var i in 0 .. 4 do captured := lambda() => i; end; "
        "  return captured(); "
        "end Main; end C;", 4);
}
