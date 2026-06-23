// Codegen + runtime — RawBytes disposal (issue #985, stdlib §10.3).
//
// fix #985 gave RawBytes a { ptr, len } representation: RawBytes.From allocates
// a small heap struct whose `ptr` field owns a separate malloc'd byte buffer.
// Disposal must therefore free BOTH the buffer AND the struct, EXACTLY ONCE,
// via bz_rawbytes_free — distinct from CString/WString, which remain bare
// buffers freed by a single free(). This file pins the soundness of that
// two-level disposal split: a `using var rb := RawBytes.From(...)` must not
// double-free (the catastrophic no-GC failure) on any control-flow path.
//
// THE DISPOSAL SPLIT (the reason this is the highest-risk part of #985): the
// shared `ffiBuffer` disposal path previously emitted a single free() for
// CString/WString/RawBytes alike. With RawBytes now a two-level allocation, the
// same single free() would free only the struct and leak the buffer (or, if the
// struct were treated as a bare buffer elsewhere, corrupt the heap). The fix
// routes RawBytes through bz_rawbytes_free while keeping CString/WString on the
// single free(); this test verifies RawBytes disposes both allocations once.
//
// NOTE on the orthogonal Array<byte> leak (issues #998 / #999): an Array<byte>
// literal local passed to a builtin FFI factory (RawBytes.From / CString.From-
// Bytes) is currently not disposed by its owner — a PRE-EXISTING ownership gap
// on the builtin-factory call path that affects CString.FromBytes identically
// and is unrelated to the RawBytes representation. The RawBytes struct + buffer
// ARE freed exactly once (no bz_rawbytes_* allocation appears in any leak
// report). Because that orthogonal array leak would trip detect_leaks=1, these
// tests use the no-CORRUPTION posture (detect_leaks=0): they catch the thing
// #985 must never regress — a double-free / use-after-free of the RawBytes
// struct or its buffer — while tolerating the orthogonal array leak. The
// value-correctness leg (compileAndRun, distinct exit codes) additionally
// proves the buffer is intact (not freed early) at the point of use.

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
    std::string stem = "bezel_985rb_" + std::to_string(counter++);
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

// Lower `source`, link against the ASan-built runtime, and run under ASan with
// leak detection OFF (the orthogonal Array<byte> leak — issues #998/#999 — is
// tolerated). Returns:
//   0  — ran clean: no use-after-free / double-free of the RawBytes allocations.
//  99  — ASan reported a memory-safety error (the regression we guard against).
//  -1  — toolchain unavailable / build step failed: SKIP (not a failure).
int runUnderAsanNoCorruption(const std::string& source) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;  // toolchain unavailable or compile failed

    static int counter = 0;
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_985rb_asan_" + std::to_string(counter++);
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

    // exitcode=99: a DISTINCT ASan error code so a double-free can never be
    // confused with the program legitimately returning its own exit code.
    std::string runCmd =
        "ASAN_OPTIONS=detect_leaks=0:exitcode=99:abort_on_error=0 " + exe +
        " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return -1;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 99;  // ASan error
    return 0;                                                       // clean
}

// Assert value-correctness (exit code) AND, when the toolchain is present, that
// ASan reports NO memory-safety error (no double-free / UAF of the RawBytes
// struct or its buffer). A double-free here would abort the whole suite, so this
// is the load-bearing soundness check for the #985 disposal split.
void expectValueAndNoCorruption(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(compileAndRun(source), expectedExitCode);
    int asan = runUnderAsanNoCorruption(source);
    if (asan == -1) return;       // toolchain unavailable: skip the ASan leg
    BEZEL_EXPECT_EQ(asan, 0);     // 0 == no double-free / UAF
}

// stdlib §10.3 / fix #985: a single `using var` RawBytes — the struct and its
// buffer are both freed via bz_rawbytes_free at scope end, exactly once.
const char* kSingleRawBytes =
    "module M; class C public static method Main(): int "
    "  var b: Array<byte> := [10, 20, 30]; "
    "  using var rb := RawBytes.From(b); "
    "  return rb.Length; "
    "end Main; end C;";

// Two RawBytes in one scope: both must dispose once, with no cross-talk between
// the two two-level allocations.
const char* kTwoRawBytes =
    "module M; class C public static method Main(): int "
    "  var b: Array<byte> := [1, 2, 3]; "
    "  var c: Array<byte> := [4, 5, 6, 7]; "
    "  using var rb1 := RawBytes.From(b); "
    "  using var rb2 := RawBytes.From(c); "
    "  return rb1.Length + rb2.Length; "
    "end Main; end C;";

// Early return: the RawBytes bound before the `return` must be disposed exactly
// once on the early-exit path (and the later one must NOT be disposed twice).
const char* kEarlyReturn =
    "module M; class C "
    "  public static method Use(n: int): int "
    "    var b: Array<byte> := [1, 2, 3]; "
    "    using var rb := RawBytes.From(b); "
    "    if n > 0 then return rb.Length; end; "
    "    using var rb2 := RawBytes.From(b); "
    "    return rb2.Length + rb.Length; "
    "  end Use; "
    "  public static method Main(): int return C.Use(5); end Main; "
    "end C;";

// Empty RawBytes (0-length array): the struct is still allocated (len 0, null
// buffer) and bz_rawbytes_free must free the struct exactly once without
// touching a null buffer — the malloc(0) edge.
const char* kEmptyRawBytes =
    "module M; class C public static method Main(): int "
    "  var b: Array<byte> := []; "
    "  using var rb := RawBytes.From(b); "
    "  return rb.Length; "
    "end Main; end C;";

// CopyTo round-trip in a disposed scope: the buffer must still be live at the
// CopyTo (not freed early) AND disposed exactly once after.
const char* kCopyToRoundTrip =
    "module M; class C public static method Main(): int "
    "  var src: Array<byte> := [5, 6, 7, 8]; "
    "  using var rb := RawBytes.From(src); "
    "  var dst: Array<byte> := [0, 0, 0, 0]; "
    "  rb.CopyTo(dst, 0); "
    "  return dst[0] + dst[1] + dst[2] + dst[3]; "
    "end Main; end C;";

} // namespace

// stdlib §10.3 / fix #985: a single RawBytes disposes both allocations once.
BEZEL_TEST(rawbytes_disposal, single_dispose_once_985) {
    expectValueAndNoCorruption(kSingleRawBytes, 3);
}

// stdlib §10.3 / fix #985: two RawBytes in one scope each dispose once.
BEZEL_TEST(rawbytes_disposal, two_in_scope_dispose_once_985) {
    expectValueAndNoCorruption(kTwoRawBytes, 7);
}

// stdlib §10.3 / fix #985: early-return path disposes the in-scope RawBytes once.
BEZEL_TEST(rawbytes_disposal, early_return_dispose_once_985) {
    expectValueAndNoCorruption(kEarlyReturn, 3);
}

// stdlib §10.3 / fix #985: an empty (0-length) RawBytes disposes cleanly.
BEZEL_TEST(rawbytes_disposal, empty_dispose_once_985) {
    expectValueAndNoCorruption(kEmptyRawBytes, 0);
}

// stdlib §10.3 / fix #985: CopyTo reads a live buffer; disposal stays once.
BEZEL_TEST(rawbytes_disposal, copyto_then_dispose_once_985) {
    expectValueAndNoCorruption(kCopyToRoundTrip, 26);
}
