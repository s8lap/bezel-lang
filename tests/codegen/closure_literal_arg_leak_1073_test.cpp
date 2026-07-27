// Codegen / leak — a LAMBDA LITERAL passed DIRECTLY as a borrowed method
// argument is never released (§28.9). Issue #1073.
//
// Spec: §28.9 (closure-cell ARC), §27.3 (lambda expressions), §28.3 (callee
// parameter borrow). Spec revision v0.220.
//
// THE BUG. A lambda literal passed directly as a method argument is a `+1`
// closure producer (the call to the lambda constructor mints exactly one
// reference). The callee parameter is a BORROW (Bezel parameters are borrows
// by default), so the callee has not transferred any reference out of the
// caller's transient. Without a call-site release, the temporary's `+1` is
// never dropped, and the closure cell + env block leak (LSan: 40 + 8 bytes).
//
// THE FIX. In `Emitter::lowerCall` (LlvmBackend_Calls2.cpp), after the call
// returns, scan each non-static argument for a `+1` closure producer
// (isPlusOneClosureProducer) and release the temporary unless the parameter
// is `owned` (ownership transfers IN to the callee), the callee is an async
// method/iterator (its frame captures the closure without retaining — a
// release would UAF), or the callee is a sync iterator (already handled by
// the #1864 release). This extends the #1864 closure ARGUMENT-PASS release
// from sync-iterator callees only to plain (non-iterator) callees — the
// closure analogue of the #429 string ARGUMENT-PASS release.
//
// OBSERVABILITY. The test compiles to a NATIVE binary, links with
// `-fsanitize=address` and runs with `ASAN_OPTIONS=detect_leaks=1:
// exitcode=99`. The pre-fix binary exits 99 (LSan reports a leak); the
// post-fix binary exits cleanly. A trailing `Console.WriteLine` is REQUIRED:
// LSan is reachability-based and only reports the closure-frame leak when a
// later expression clobbers the register that still held the dead closure
// pointer (per the #1864 test convention).
//
// Completes the slot-A-12 WIP for issue #1073 (LlvmBackend_Calls2.cpp).

#include <cstdio>
#include <cstdlib>
#include <filesystem>
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

// (A) #1073 repro — lambda literal passed directly to a borrow-parameter
// method. The closure cell + env leak ~48 bytes pre-fix. The trailing
// `Console.WriteLine` is REQUIRED: LSan is reachability-based and only
// reports the closure-frame leak when a later expression reuses the
// register that still held the dead closure pointer (see #1864 test
// comment for the same convention). Single call: box.V == 1.
const char* kLambdaLiteralArg =
    "module M; "
    "class Box public V: int := 0; "
    "  public method Add(x: int) V := V + x; end Add; "
    "end Box; "
    "class Sink public method Take(h: () -> ()) h(); end Take; end Sink; "
    "class C public static method Main(): int "
    "  using var box := new Box(); "
    "  using var s := new Sink(); "
    "  s.Take(lambda() => do box.Add(1); end); "
    "  Console.WriteLine(\"v=\" + box.V.ToString()); "  // trailing stmt — surfaces LSan
    "  return box.V; "
    "end Main; end C;";

// (B) Same shape in a loop — 50 evaluations amplify the leak from a single
// 40-byte cell to a clear 2000-byte LSan headline. Pre-fix this is the
// reliable FAIL signal: `Direct leak of 2000 byte(s)` over 50 allocations.
// Post-fix: ASan-clean. box.V == 50.
const char* kLambdaLiteralArgLoop =
    "module M; "
    "class Box public V: int := 0; "
    "  public method Add(x: int) V := V + x; end Add; "
    "end Box; "
    "class Sink public method Take(h: () -> ()) h(); end Take; end Sink; "
    "class C public static method Main(): int "
    "  using var box := new Box(); "
    "  using var s := new Sink(); "
    "  var i := 0; "
    "  while i < 50 do "
    "    s.Take(lambda() => do box.Add(1); end); "
    "    i := i + 1; "
    "  end; "
    "  Console.WriteLine(\"v=\" + box.V.ToString()); "  // trailing stmt — surfaces LSan
    "  return box.V; "
    "end Main; end C;";

// returns:
//   0  -> program ran under ASan and was leak-clean (matched expected exit code)
//   1  -> program produced wrong exit code
//  -1  -> toolchain unavailable (no clang); SKIP
//  -2  -> compile/link failure
int runUnderAsan(const std::string& source, int expectedExitCode) {
    namespace fs = std::filesystem;
    static int counter = 0;
    const std::string stem = "bezel_1073_arg_san_" + std::to_string(counter++);
    auto dir = fs::temp_directory_path();
    std::string objPath = (dir / (stem + ".o")).string();
    std::string exe = (dir / stem).string();
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;  // toolchain unavailable: SKIP
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
    if (std::system(linkCmd.c_str()) != 0) { cleanup(); return -2; }
    std::string runCmd = "ASAN_OPTIONS=detect_leaks=1:exitcode=99 " + exe +
                         " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return -2;
    if (!WIFEXITED(status)) return 1;       // crashed/aborted
    int code = WEXITSTATUS(status);
    if (code == 99) return 1;               // sanitizer reported a leak
    return (code == expectedExitCode) ? 0 : 1;
}

}  // namespace

// =====================================================================
// (A) Single call — ASan-clean, exit code 1. §28.9 / §27.3 / #1073.
// =====================================================================
BEZEL_TEST(codegen_1073_closure_literal_arg, single_call_lsan_clean_spec_28_9) {
    int a = runUnderAsan(kLambdaLiteralArg, 1);
    if (a == -1) return;    // toolchain unavailable: SKIP
    if (a == -2) { BEZEL_EXPECT_TRUE(false); return; }
    BEZEL_EXPECT_EQ(a, 0);
}

// =====================================================================
// (B) Looped call — 50 evaluations, no cumulative closure leak. §28.9 / #1073.
// =====================================================================
BEZEL_TEST(codegen_1073_closure_literal_arg, loop_50_calls_lsan_clean_spec_28_9) {
    int a = runUnderAsan(kLambdaLiteralArgLoop, 50);
    if (a == -1) return;    // toolchain unavailable: SKIP
    if (a == -2) { BEZEL_EXPECT_TRUE(false); return; }
    BEZEL_EXPECT_EQ(a, 0);
}
