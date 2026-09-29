// Codegen — disposal of an `owned` field/local whose type is a BUILTIN exception
// (issue #405, §28.5 / §28.4 / §31.1). A builtin exception (Exception and its
// language-built-in subtypes, §31.4) is an ordinary owned heap class instance:
// `new InvalidOperationException("x")` produces an owned value (§31.1 "Exception
// ownership"). When such a value is stored in an `owned` field (§28.5) or a
// scope-bound `using var` local (§28.10) and the enclosing object / scope is
// disposed, the field must be auto-disposed (§28.4 step 2) and its heap block
// reclaimed (#380), exactly like a user class.
//
// The bug: builtin exception types have no user TypeSymbol (rt->declaration ==
// null), so isOwnedReferenceField rejected them and the owner's compiler-
// generated Dispose emitted NO disposal for such a field at all — the owned
// builtin exception leaked. A USER exception class (real TypeSymbol, real
// disposer) WAS disposed; that asymmetry is the bug. Fix: recognize a builtin-
// exception-typed owned field/local and dispatch + reclaim it through the same
// dynamic-dispose path (its vtable slot 1 is the shared no-op `bz$exDispose`,
// which owns nothing — the message string is runtime-managed per §28.5/§28.9 and
// is NOT freed; only the 16-byte exception block is reclaimed).
//
// CRITICAL — no double-free with in-flight exceptions (#383 / #387). An
// exception that is thrown / caught / re-thrown is disposed by the exception
// machinery (the runtime owns it while in flight). The auto-dispose wired here
// fires ONLY for an OWNED FIELD / LOCAL whose value is never thrown; the #411
// per-object idempotency flag is the backstop. The kInFlightNotDoubleFreed case
// throws + catches a builtin exception and asserts no double-free / no UAF.
//
// Two complementary check kinds (mirroring free_on_dispose_test.cpp):
//   * exit-code tests (always run): compile to a native executable and assert
//     the exit code (proves the program runs the intended path).
//   * LeakSanitizer native tests (run only with a sanitizer toolchain): lower to
//     an object file, link against an ASan-instrumented runtime, run under LSan
//     — proves the block is reclaimed with no leak / double-free / use-after-free.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>

#include "codegen/LlvmBackend.hpp"
#include "support/emit_object_support.hpp"
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
// run it; returns the exit code, or -1 if compilation/link/run failed.
int compileAndRun(const std::string& source) {
    static int counter = 0;
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_405_" + std::to_string(counter++);
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

// LeakSanitizer verification. Lowers `source` to an object file (full front-end),
// links against an ASan-instrumented runtime, runs under LSan. With `detectLeaks`
// (default) a leak is a sanitizer failure; with `detectLeaks=false` only the
// memory-safety checks (use-after-free, double-free, heap overflow) remain — used
// to assert that an in-flight exception path has NO double-free / no UAF. Returns:
//   0  — clean (no problem of the kinds being detected).
//   1  — sanitizer reported a problem: test failure.
//  -1  — toolchain unavailable / build step failed: SKIP (not a failure).
// Uses the memoized ASan runtime object (compiled once per process).
int runUnderLeakSanitizer(const std::string& source, bool detectLeaks = true) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;  // toolchain unavailable or compile failed

    static int counter = 0;
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_405_lsan_" + std::to_string(counter++);
    std::string objPath = (dir / (stem + ".o")).string();
    std::string exe     = (dir / stem).string();

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
        if (!bezel::test::emitObjectFileForTest(*module, objPath, err))
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
        std::string("ASAN_OPTIONS=detect_leaks=") +
        (detectLeaks ? "1" : "0") + ":exitcode=99 " + exe +
        " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return bezel::test::asanExecFailure("system");
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 1;  // sanitizer
    return 0;                                                      // clean
}

// Assert the exit code (always) and, if the toolchain is present, that
// LeakSanitizer reports no leak/corruption.
void expectDisposedAndLeakFree(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(compileAndRun(source), expectedExitCode);
    int lsan = runUnderLeakSanitizer(source);
    if (lsan == -1) return;       // toolchain unavailable: skip the LSan leg
    BEZEL_EXPECT_EQ(lsan, 0);     // 0 == LSan clean (no leak, no UAF/DF)
}

// Assert the exit code (always) and, with leak detection OFF, that there is NO
// double-free / use-after-free. Used for the in-flight builtin-exception case,
// whose disposal is owned by the exception machinery — the soundness property is
// that the auto-dispose wiring never double-frees an in-flight/caught exception.
void expectNoUseAfterFreeOrDoubleFree(const std::string& source,
                                      int expectedExitCode) {
    BEZEL_EXPECT_EQ(compileAndRun(source), expectedExitCode);
    int asan = runUnderLeakSanitizer(source, /*detectLeaks=*/false);
    if (asan == -1) return;       // toolchain unavailable: skip the ASan leg
    BEZEL_EXPECT_EQ(asan, 0);     // 0 == no UAF / no double-free / no overflow
}

// --- Programs -------------------------------------------------------------

// §28.5 / §28.4 / §31.1: a class with an `owned` field of BUILTIN exception type
// (InvalidOperationException), constructed via `new …("x")`. Disposed at the
// `using var` owner's scope end, the field's 16-byte exception block is reclaimed
// (the message string "x" is a runtime-managed string constant, NOT freed). The
// program returns 0 on the normal path. Before the fix this object LEAKED (LSan).
const char* kOwnedBuiltinExcField =
    "module M; class Holder "
    "  private owned _e: InvalidOperationException?; "
    "  public constructor() this._e := new InvalidOperationException(\"x\"); "
    "  end constructor; "
    "  public static method Main(): int "
    "    do using var h := new Holder(); end; return 0; "
    "  end Main; "
    "end Holder;";

// §28.5: same shape, but the owned field is typed as the base `Exception?` and
// holds a derived builtin (ArgumentException). Disposal dispatches through the
// object's own vtable slot 1 (no-op dispose) and reclaims the block.
const char* kOwnedExceptionBaseField =
    "module M; class Holder "
    "  private owned _e: Exception?; "
    "  public constructor() this._e := new ArgumentException(\"bad\"); "
    "  end constructor; "
    "  public static method Main(): int "
    "    do using var h := new Holder(); end; return 0; "
    "  end Main; "
    "end Holder;";

// §28.10: an `owned` LOCAL of builtin exception type via `using var` (not a
// field). Disposed at scope end; block reclaimed. This is the local analogue of
// the owned-field case and exercises the same dispatch+reclaim path.
const char* kUsingVarBuiltinExc =
    "module M; class R public static method Main(): int "
    "  do using var e := new FormatException(\"nope\"); end; return 0; "
    "end Main; end R;";

// Control (§28.5): a USER exception class field. This was ALWAYS disposed (the
// asymmetry that masked #397); its destructor runs once (count 1) and the block
// is reclaimed. Confirms the user path is unchanged and stays leak-free.
const char* kOwnedUserExcField =
    "module M; class MyError "
    "  public static _n: int := 0; "
    "  destructor _n := _n + 1; end destructor; "
    "end MyError; "
    "class Holder "
    "  private owned _e: MyError?; "
    "  public constructor() this._e := new MyError(); end constructor; "
    "  public static method Main(): int "
    "    do using var h := new Holder(); end; return MyError._n; "
    "  end Main; "
    "end Holder;";

// CRITICAL (#383 / #387): an in-flight builtin exception is thrown and caught.
// The runtime/exception machinery owns it while in flight and reclaims it after
// the matching arm completes. The auto-dispose wiring added for owned fields/
// locals must NOT also free this in-flight object (no double-free). Returns 7 on
// the caught path; ASan (leak detection off) confirms no double-free / no UAF.
const char* kInFlightNotDoubleFreed =
    "module M; class R public static method Main(): int "
    "  try throw new InvalidOperationException(\"boom\"); "
    "  catch InvalidOperationException => return 7; "
    "  end; return -1; "
    "end Main; end R;";

}  // namespace

BEZEL_TEST(codegen_owned_exception_field_405, owned_builtin_exception_field_disposed_28_5) {
    // The owned InvalidOperationException field is reclaimed at owner dispose.
    expectDisposedAndLeakFree(kOwnedBuiltinExcField, 0);
}

BEZEL_TEST(codegen_owned_exception_field_405, owned_exception_base_field_disposed_28_5) {
    // An `owned Exception?` field holding a derived builtin is reclaimed.
    expectDisposedAndLeakFree(kOwnedExceptionBaseField, 0);
}

BEZEL_TEST(codegen_owned_exception_field_405, using_var_builtin_exception_disposed_28_10) {
    // An owned builtin-exception LOCAL is reclaimed at scope end.
    expectDisposedAndLeakFree(kUsingVarBuiltinExc, 0);
}

BEZEL_TEST(codegen_owned_exception_field_405, user_exception_field_disposed_control_28_5) {
    // Control: the user exception class field is disposed once (count 1), as it
    // always was — the asymmetry the builtin fix removes.
    expectDisposedAndLeakFree(kOwnedUserExcField, 1);
}

BEZEL_TEST(codegen_owned_exception_field_405, in_flight_builtin_exception_no_double_free_31_1) {
    // The thrown+caught builtin exception is freed by the exception machinery
    // exactly once; the owned-field auto-dispose wiring must not double-free it.
    expectNoUseAfterFreeOrDoubleFree(kInFlightNotDoubleFreed, 7);
}
