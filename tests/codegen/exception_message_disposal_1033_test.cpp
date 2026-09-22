// Codegen + runtime — disposal of a built-in exception's owned Message string
// (issue #1033, §31.1 / §31.4 / §28.9).
//
// §31.4 declares `public readonly property Message: string` on the base
// `Exception`, and §31.1 ("Exception ownership") states that `new
// SomeException(...)` produces an owned instance the runtime DISPOSES after the
// matching catch arm completes. Per §28.9 a class string field holds an
// INDEPENDENT +1 (retained/transferred at the store site) released exactly once
// on disposal — the same model applies to the exception's Message (and
// ArgumentException's ParamName): the exception OWNS it.
//
// The bug (#1033): the built-in exception construction path stored the lowered
// message argument at byte offset 8 (`new Exception(msg)` in
// LlvmBackend_Exprs2.cpp) but the built-in exception disposer was the shared
// NO-OP `bz$exDispose` (it serves the value-type boxes too, which carry no
// string at offset 8). So a fresh +1 message — an interpolation `$"...{n}..."`,
// a `+` concat, a slice, a primitive `.ToString()` — whose single reference was
// transferred into the slot was NEVER released: every `throw new
// Exception(<fresh string>)` leaked the message. A literal/sentinel message was
// clean (immortal) and a local-string message was clean only because the local
// kept its own +1 (the exception held a dangling borrow — sound only while the
// local outlived the exception).
//
// The fix: a SEPARATE disposer `bz$exDisposeMsg` (wired into the built-in
// exception vtable slot 2; boxes keep the no-op) releases the Message@8 (and
// ParamName@32) string slots and NULLs them — mirroring the class string-field
// disposal loop (LlvmBackend_Disposal.cpp). The construction path retains a
// BORROW initializer (so the slot owns an independent +1, no dangling borrow)
// and transfers a +1 PRODUCER (no retain), exactly like lowerStringFieldStore.
// The CauseChain@24 is intentionally NOT touched (the documented async
// double-dispose concern, §31.4/#811 note).
//
// These tests mirror the string/closure refcount tests:
//   * exit-code tests (always run): compile to a native exe, assert exit code
//     (proves the intended path ran and the exception was caught/disposed).
//   * LeakSanitizer native tests (with a sanitizer toolchain): lower to an
//     object file, link against the ASan-instrumented runtime, run under LSan —
//     proves the message is reclaimed (no leak) with no UAF/double-free. Before
//     the fix, kInterpolatedMessageLoop leaked ~N message strings under LSan.

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
static const std::filesystem::path& wtDir1033() {
    return wtScratchDir("wt-1033");
}

// Compile `source` to a temp native executable via the real Driver pipeline and
// run it; returns the exit code, or -1 on compile/link/run failure.
int compileAndRun(const std::string& source) {
    auto dir = wtDir1033();
    std::string stem = unique_stem("bezel_1033");
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

// LeakSanitizer verification for a single program. Returns:
//   0  — ran clean: no leak, no use-after-free / double-free.
//   1  — sanitizer reported a problem (leak or corruption): test failure.
//  -1  — toolchain unavailable / build step failed: SKIP (not a failure).
int runUnderLeakSanitizer(const std::string& source) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;  // toolchain unavailable or compile failed

    auto dir = wtDir1033();
    std::string stem = unique_stem("bezel_1033_lsan");
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
        "ASAN_OPTIONS=detect_leaks=1:exitcode=99 " + exe + " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return bezel::test::asanExecFailure("system");
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 1;  // sanitizer
    return 0;                                                      // clean
}

// Assert correctness (exit code) AND, when the toolchain is present, that LSan
// reports no leak/UAF/double-free.
void expectValueAndLeakFree(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(compileAndRun(source), expectedExitCode);
    int lsan = runUnderLeakSanitizer(source);
    if (lsan == -1) return;       // toolchain unavailable: skip the LSan leg
    BEZEL_EXPECT_EQ(lsan, 0);     // 0 == LSan clean (no leak, no UAF/DF)
}

// --- Programs ------------------------------------------------------------

// #1033 / §31.1 / §28.9 (the issue's repro, looped to amplify the leak): each
// iteration throws `new ArgumentException($"error {i}")` — a fresh +1
// interpolation message — and the catch arm suppresses (§31.2.5), disposing the
// exception. Before the fix the +1 message leaked every iteration; LSan reports
// ~N leaked strings. Returns 0.
const char* kInterpolatedMessageLoop =
    "module M; class C "
    "  public static method Main(): int "
    "    var i: int := 0; "
    "    while i < 5000 do "
    "      try "
    "        throw new ArgumentException($\"error {i}\"); "
    "      catch "
    "        var e: Exception => (); "
    "      end; "
    "      i := i + 1; "
    "    end; "
    "    return 0; "
    "  end Main; "
    "end C;";

// #1033 / §28.9: a `+` concat is also a +1 string producer (a binary `+` whose
// result type is `string`). The single +1 reference transferred into the
// exception's Message must be released on disposal. Returns 0.
const char* kConcatMessageLoop =
    "module M; class C "
    "  public static method Main(): int "
    "    var i: int := 0; "
    "    while i < 5000 do "
    "      try "
    "        throw new Exception(\"boom \" + \"bag\"); "
    "      catch "
    "        var e: Exception => (); "
    "      end; "
    "      i := i + 1; "
    "    end; "
    "    return 0; "
    "  end Main; "
    "end C;";

// #1033 / §31.4: ArgumentException's two-argument `(message, paramName)`
// constructor stores the paramName at offset 32 — the SAME leak as Message for
// a fresh +1 paramName. The fix releases ParamName in the disposer too. The
// catch arm reads both `.Message` and `.ParamName` (nullable) to confirm the
// slots stay valid until the arm completes (no premature free / UAF). Returns 0.
const char* kInterpolatedParamNameLoop =
    "module M; class C "
    "  public static method Main(): int "
    "    var i: int := 0; "
    "    while i < 5000 do "
    "      try "
    "        throw new ArgumentException(\"msg\", $\"param {i}\"); "
    "      catch "
    "        var e: ArgumentException => do "
    "          var m: string := e.Message; "
    "          var p: string? := e.ParamName; "
    "          if m = \"\" then return 1; end; "
    "          if p = null then return 1; end; "
    "        end "
    "      end; "
    "      i := i + 1; "
    "    end; "
    "    return 0; "
    "  end Main; "
    "end C;";

// #1033 / §28.9 no-regression: a BORROW initializer (a local string) must be
// RETAINED at construction so the exception owns an independent +1 (not a
// dangling borrow), then released on disposal. The local is read again AFTER
// the exception is disposed to prove no use-after-free (the local keeps its own
// +1, so releasing the exception's retain does not free the local's string).
// Before the fix this was "clean" only by luck (the local outlived the
// exception) and read a dangling borrow inside the arm. Returns 0.
const char* kBorrowMessageLocalNoUaf =
    "module M; class C "
    "  public static method Main(): int "
    "    var s: string := \"local-msg\"; "
    "    try "
    "      throw new Exception(s); "
    "    catch "
    "      var e: Exception => do "
    "        var m: string := e.Message; "
    "        if m = \"\" then return 1; end; "
    "      end "
    "    end; "
    "    if s = \"\" then return 1; end; "
    "    return 0; "
    "  end Main; "
    "end C;";

// #1033 / §28.9 no-regression: a LITERAL message is a sentinel — retain and
// release are no-ops (bz_string_release is a no-op on a sentinel). Disposing
// such an exception must not crash or double-free. Returns 0.
const char* kLiteralMessageNoCrash =
    "module M; class C "
    "  public static method Main(): int "
    "    try "
    "      throw new Exception(\"literal-msg\"); "
    "    catch "
    "      var e: Exception => (); "
    "    end; "
    "    return 0; "
    "  end Main; "
    "end C;";

}  // namespace

// --- Tests ---------------------------------------------------------------

// §31.1 / §31.4 / §28.9 (#1033): the primary repro — a `throw new
// ArgumentException($"...")` loop must not leak the interpolated Message.
BEZEL_TEST(codegen_exception_message_disposal_1033, interpolated_message_reclaimed_31_1) {
    expectValueAndLeakFree(kInterpolatedMessageLoop, 0);
}

// §28.9: a `+` concat message (another +1 producer) is likewise reclaimed.
BEZEL_TEST(codegen_exception_message_disposal_1033, concat_message_reclaimed_28_9) {
    expectValueAndLeakFree(kConcatMessageLoop, 0);
}

// §31.4: ArgumentException's ParamName (a +1 interpolation) is reclaimed too,
// and stays valid while read in the catch arm.
BEZEL_TEST(codegen_exception_message_disposal_1033, interpolated_paramname_reclaimed_31_4) {
    expectValueAndLeakFree(kInterpolatedParamNameLoop, 0);
}

// §28.9 no-regression: a borrow (local) Message is retained+released (no
// dangling borrow) and the local survives the exception's disposal (no UAF).
BEZEL_TEST(codegen_exception_message_disposal_1033, borrow_message_local_no_uaf_28_9) {
    expectValueAndLeakFree(kBorrowMessageLocalNoUaf, 0);
}

// §28.9 no-regression: a literal (sentinel) Message disposes cleanly (retain
// and release are no-ops; no crash, no double-free).
BEZEL_TEST(codegen_exception_message_disposal_1033, literal_message_no_crash_28_9) {
    expectValueAndLeakFree(kLiteralMessageNoCrash, 0);
}
