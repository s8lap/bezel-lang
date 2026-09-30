// Codegen — Console.Write/WriteLine of a primitive must not leak the transient
// string built for the write (issue #421, follow-up to #420). When the argument
// is a numeric/char primitive, codegen converts it to a bz_string via
// emitPrimitiveToString, whose snprintf path heap-allocates BOTH the bz_string
// object and its data buffer. That string exists solely to feed the write and
// is referenced by nothing afterward — before this fix it leaked (~26 bytes per
// `Console.WriteLine(7)`). The fix frees the data buffer and the object after
// the write.
//
// Two complementary checks (mirroring undisposed_leak_test.cpp):
//   * a JIT run (always): proves the program still produces the right exit code
//     (the write itself is unaffected), so the free did not corrupt the output.
//   * a LeakSanitizer native run (only with a sanitizer toolchain): lowers to an
//     object file, links against an ASan-instrumented runtime, runs under LSan.
//     Before the fix LSan reports the leaked transient(s); after, it is clean.
//     This is the load-bearing leg for the leak fix.

#include <cstdio>
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

using bezel::test::runAot;

namespace {

namespace fs = std::filesystem;

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit.
static const std::filesystem::path& wtDir421() {
    return wtScratchDir("wt-421");
}

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// Lower `source` to an object file, link against an ASan-instrumented runtime,
// run under LeakSanitizer. Returns 0 (clean), 1 (sanitizer reported a leak),
// or -1 (toolchain unavailable → SKIP). Mirrors undisposed_leak_test.cpp.
// Uses the memoized ASan runtime object (compiled once per process).
int runUnderLeakSanitizer(const std::string& source) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;  // toolchain unavailable or compile failed

    auto dir = wtDir421();
    std::string stem = unique_stem("bezel_421_lsan");
    std::string objPath = (dir / (stem + ".o")).string();
    std::string exe = (dir / stem).string();

    {
        bezel::DiagnosticEngine diags;
        bezel::Lexer lexer(source, diags);
        bezel::Parser parser(lexer, diags);
        auto unit = parser.parseCompilationUnit();
        if (!unit) return bezel::test::asanBuildFailure("parse");
        bezel::sema::SemaContext ctx(diags);
        bezel::sema::collectSymbols(*unit, ctx);
        bezel::sema::resolveTypes(*unit, ctx);
        bezel::sema::typecheckExpressions(*unit, ctx);
        bezel::sema::checkDefiniteAssignment(*unit, ctx);
        bezel::ownership::checkOwnership(*unit, ctx);
        if (diags.hasErrors()) return bezel::test::asanBuildFailure("sema");
        llvm::LLVMContext llvmCtx;
        auto module = bezel::codegen::emitModule(*unit, ctx, llvmCtx);
        if (!module) return bezel::test::asanBuildFailure("emitModule");
        std::string err;
        if (!bezel::codegen::emitObjectFile(*module, objPath, err))
            return bezel::test::asanBuildFailure("emitObjectFile");
    }

    auto cleanup = [&] {
        std::remove(objPath.c_str());
        std::remove(exe.c_str());
    };
    std::string linkCmd = "clang++ -fsanitize=address -g " + objPath + " " +
                          rtObj + " -lm -lpthread -lstdc++ -o " + exe +
                          " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) {
        cleanup();
        return bezel::test::asanLinkFailure("link");
    }
    std::string runCmd =
        "ASAN_OPTIONS=detect_leaks=1:exitcode=99 " + exe + " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return bezel::test::asanExecFailure("system");
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 1;  // leak
    return 0;                                                      // clean
}

void expectRunsAndLeakFree(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(run(source), expectedExitCode);
    int lsan = runUnderLeakSanitizer(source);
    if (lsan == -1) return;    // toolchain unavailable: skip the LSan leg
    BEZEL_EXPECT_EQ(lsan, 0);  // 0 == LSan clean (no leaked transient string)
}

// Console.WriteLine(<int literal>) — the canonical repro from the issue.
const char* kWriteLineIntLiteral =
    "module M; class C public static method Main(): int "
    "  Console.WriteLine(7); return 0; "
    "end Main; end C;";

// Console.WriteLine(<int local>) — same path via a variable.
const char* kWriteLineIntLocal =
    "module M; class C public static method Main(): int "
    "  var n := 42; Console.WriteLine(n); return 0; "
    "end Main; end C;";

// Console.WriteLine(<double>) — the FP snprintf path also heap-allocates.
const char* kWriteLineDouble =
    "module M; class C public static method Main(): int "
    "  Console.WriteLine(3.5); return 0; "
    "end Main; end C;";

// Console.Write(<int>) (no newline) — same transient-string allocation.
const char* kWriteIntLiteral =
    "module M; class C public static method Main(): int "
    "  Console.Write(7); return 0; "
    "end Main; end C;";

// Several writes in a row — the per-call leak compounds; all must be freed.
const char* kWriteLineLoop =
    "module M; class C public static method Main(): int "
    "  var i := 0; "
    "  while i < 5 do Console.WriteLine(i); i := i + 1; end; "
    "  return 0; "
    "end Main; end C;";

}  // namespace

BEZEL_TEST(codegen_console_leak_421, writeline_int_literal_no_leak) {
    expectRunsAndLeakFree(kWriteLineIntLiteral, 0);
}

BEZEL_TEST(codegen_console_leak_421, writeline_int_local_no_leak) {
    expectRunsAndLeakFree(kWriteLineIntLocal, 0);
}

BEZEL_TEST(codegen_console_leak_421, writeline_double_no_leak) {
    expectRunsAndLeakFree(kWriteLineDouble, 0);
}

BEZEL_TEST(codegen_console_leak_421, write_int_literal_no_leak) {
    expectRunsAndLeakFree(kWriteIntLiteral, 0);
}

BEZEL_TEST(codegen_console_leak_421, writeline_in_loop_no_leak) {
    expectRunsAndLeakFree(kWriteLineLoop, 0);
}

// #1031 / §4.6 / §28.9: Console.WriteLine of a FRESH +1 string (interpolation /
// string concat / slice / numeric ToString) must release it after the write —
// Console output is special-lowered and bypasses the normal call-arg disposal, so
// before the fix the fresh render leaked. A borrow (string local/literal) is NOT a
// plus-one producer and is correctly left alone (no double-free).
const char* kWriteLineInterpFresh_1031 =
    "module M; class C public static method Main(): int "
    "  var n := 42; Console.WriteLine($\"value is {n}\"); return 0; "
    "end Main; end C;";
BEZEL_TEST(codegen_console_leak_1031, writeline_interpolation_released) {
    expectRunsAndLeakFree(kWriteLineInterpFresh_1031, 0);
}

const char* kWriteLineConcatFresh_1031 =
    "module M; class C public static method Main(): int "
    "  var a := \"x\"; Console.WriteLine(a + \"y\"); return 0; "
    "end Main; end C;";
BEZEL_TEST(codegen_console_leak_1031, writeline_concat_released) {
    expectRunsAndLeakFree(kWriteLineConcatFresh_1031, 0);
}

// #1031 regression: a STRING-LOCAL borrow arg must NOT be released (no
// double-free / use-after-free of the shared reference).
const char* kWriteLineStringLocalBorrow_1031 =
    "module M; class C public static method Main(): int "
    "  var s := \"hello\"; Console.WriteLine(s); return 0; "
    "end Main; end C;";
BEZEL_TEST(codegen_console_leak_1031, writeline_string_local_borrow_not_released) {
    expectRunsAndLeakFree(kWriteLineStringLocalBorrow_1031, 0);
}

// #1031 (loop): a fresh interpolation written each iteration must not leak
// per-iteration (the unbounded case).
const char* kWriteLineInterpLoop_1031 =
    "module M; class C public static method Main(): int "
    "  var i := 0; while i < 5 do Console.WriteLine($\"i={i}\"); i := i + 1; end; "
    "  return 0; "
    "end Main; end C;";
BEZEL_TEST(codegen_console_leak_1031, writeline_interpolation_loop_no_unbounded_leak) {
    expectRunsAndLeakFree(kWriteLineInterpLoop_1031, 0);
}

// #1032 (sibling of #1031): Debug.WriteLine of a fresh +1 string must release it
// after the write (Debug output is special-lowered, bypassing the normal call-arg
// disposal). A string-local borrow must NOT be released (no double-free).
const char* kDebugWriteLineInterpFresh_1032 =
    "module M; import System.Diagnostics; class C public static method Main(): int "
    "  var n := 9; Debug.WriteLine($\"d {n}\"); return 0; "
    "end Main; end C;";
BEZEL_TEST(codegen_console_leak_1032, debug_writeline_interpolation_released) {
    expectRunsAndLeakFree(kDebugWriteLineInterpFresh_1032, 0);
}

const char* kDebugWriteLineStringLocalBorrow_1032 =
    "module M; import System.Diagnostics; class C public static method Main(): int "
    "  var s := \"msg\"; Debug.WriteLine(s); return 0; "
    "end Main; end C;";
BEZEL_TEST(codegen_console_leak_1032, debug_writeline_string_local_borrow_not_released) {
    expectRunsAndLeakFree(kDebugWriteLineStringLocalBorrow_1032, 0);
}
