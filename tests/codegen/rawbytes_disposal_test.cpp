// Codegen + runtime — RawBytes disposal (issue #985, stdlib §10.3) and
// source Array<byte> disposal on the builtin byte-FFI factory call path
// (issue #998, §28.3).
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
// fix #998 / §28.3: a plain `var b: Array<byte>` (non-using) passed as a borrow
// to RawBytes.From(b) or CString.FromBytes(b) was never disposed at scope end.
// The lowerByteArrayFactoryArg helper now registers `b` for scope-end collection
// disposal when the argument is a plain var local (isUsing=false), using the same
// buildCollectionTempDisposal machinery as the collection-literal path. Guards
// prevent double-registration when the same local appears in multiple FFI calls
// within the same scope, or when it is already a `using var` with disposal
// registered via registerUsingDisposal.
//
// NOTE on kCopyToRoundTrip: the `var dst` array passed to rb.CopyTo(dst, 0) is
// NOT a FFI-factory argument and is not covered by the #998 fix — that disposal
// gap is tracked separately. kCopyToRoundTrip keeps detect_leaks=0 (the existing
// #985 posture) to allow the dst leak while still catching double-free / UAF of
// the RawBytes struct or its buffer.

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
#include "support/unique_stem.hpp"

namespace {

namespace fs = std::filesystem;

// Compile `source` to a temp native executable via the real Driver pipeline and
// run it; returns the exit code, or -1 on compile/link/run failure.
int compileAndRun(const std::string& source) {
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bezel_985rb");
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
//
// #3653: NOT converted to tests/support/run_under_asan.hpp. A hybrid of the
// two shared shapes: Family C's ASAN_OPTIONS (detect_leaks=0 +
// abort_on_error=0) paired with Family B's coarse 0/99 verdict rather than
// Family C's raw exit-code passthrough. Neither shared function reproduces
// this exact combination without a behavior change.
int runUnderAsanNoCorruption(const std::string& source) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;  // toolchain unavailable or compile failed

    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bezel_985rb_asan");
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
        if (!bezel::test::emitObjectFileForTest(*module, objPath, err)) return -2;
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

    // exitcode=99: a DISTINCT ASan error code so a double-free can never be
    // confused with the program legitimately returning its own exit code.
    std::string runCmd =
        "ASAN_OPTIONS=detect_leaks=0:exitcode=99:abort_on_error=0 " + exe +
        " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return bezel::test::asanExecFailure("system");
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

// #998 / §28.3: run under ASan with leak detection ON (detect_leaks=1). Returns:
//   0  — ran clean: no leak, no double-free, no UAF.
//  99  — ASan / LSan reported an error.
//  -1  — toolchain unavailable / build step failed: SKIP.
int runUnderLeakSanitizer(const std::string& source) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;

    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bezel_998rb_lsan");
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
        if (!bezel::test::emitObjectFileForTest(*module, objPath, err)) return -2;
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
        "ASAN_OPTIONS=detect_leaks=1:exitcode=99:abort_on_error=0 " + exe +
        " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return bezel::test::asanExecFailure("system");
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 99;
    return 0;
}

// #998 / §28.3: assert value-correctness AND leak-freedom (detect_leaks=1).
// The source Array<byte> must be freed exactly once at scope end (no leak, no
// double-free). Callers must ensure ALL heap locals in the probe are disposed —
// only use this where every non-using Array<byte> is a FFI-factory argument.
void expectValueAndLeakFree(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(compileAndRun(source), expectedExitCode);
    int lsan = runUnderLeakSanitizer(source);
    if (lsan == -1) return;        // toolchain unavailable: skip the LSan leg
    BEZEL_EXPECT_EQ(lsan, 0);      // 0 == no leak, no double-free, no UAF
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

// ── #998 / §28.3: source Array<byte> disposal regression tests ───────────────
//
// Each probe uses a plain `var b: Array<byte>` (non-using) as the source array
// so that fix #998 is the SOLE mechanism disposing it. Every other heap local
// is either a `using var` or disposed explicitly, ensuring the only possible
// leak under LSan is the source array — so detect_leaks=1 proves disposal is
// exactly-once (not zero, not two).

// #998 / §28.3: plain `var` source array passed to RawBytes.From — leak-free.
// Pre-fix: bz_array_alloc of b leaked (32B struct + 16B buffer). Post-fix: the
// lowerByteArrayFactoryArg helper registers b's slot for scope-end bz_array_free
// on the normal AND unwind paths.
const char* k998RawBytesVarSource =
    "module M; class C public static method Main(): int "
    "  var b: Array<byte> := [10, 20, 30]; "
    "  using var rb := RawBytes.From(b); "
    "  return rb.Length; "
    "end Main; end C;";

// #998 / §28.3: plain `var` source array passed to CString.FromBytes — leak-free.
// CString.FromBytes shares the same lowerByteArrayFactoryArg codepath as
// RawBytes.From, so the fix covers it identically.
const char* k998CStringFromBytesVarSource =
    "module M; class C public static method Main(): int "
    "  var b: Array<byte> := [65, 66, 67, 0]; "
    "  using var cs := CString.FromBytes(b); "
    "  return cs.Length; "
    "end Main; end C;";

// #998 / §28.3: same local passed to two RawBytes.From calls in one scope —
// no double-free (the alreadyReg guard prevents a second disposal entry).
const char* k998MultipleFFICallsSameLocal =
    "module M; class C public static method Main(): int "
    "  var b: Array<byte> := [1, 2, 3]; "
    "  using var rb1 := RawBytes.From(b); "
    "  using var rb2 := RawBytes.From(b); "
    "  return rb1.Length + rb2.Length; "
    "end Main; end C;";

// #998 / §28.3: `using var b` form — disposal already registered via
// registerUsingDisposal; the isUsing guard in lowerByteArrayFactoryArg must NOT
// add a second disposal entry (no double-free).
const char* k998UsingVarSourceNoDoubleFree =
    "module M; class C public static method Main(): int "
    "  using var b: Array<byte> := [10, 20, 30]; "
    "  using var rb := RawBytes.From(b); "
    "  return rb.Length; "
    "end Main; end C;";

// #998 / §28.3: two distinct source arrays, each passed to RawBytes.From —
// both disposed exactly once (two separate disposal entries).
const char* k998TwoDistinctSources =
    "module M; class C public static method Main(): int "
    "  var b: Array<byte> := [1, 2, 3]; "
    "  var c: Array<byte> := [4, 5]; "
    "  using var rb1 := RawBytes.From(b); "
    "  using var rb2 := RawBytes.From(c); "
    "  return rb1.Length + rb2.Length; "
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

// fix #998 / §28.3: plain `var` source Array<byte> is disposed exactly once at
// scope end when passed to RawBytes.From (no leak of the bz_array_t + buffer).
BEZEL_TEST(rawbytes_disposal, var_source_rawbytes_leak_free_998) {
    expectValueAndLeakFree(k998RawBytesVarSource, 3);
}

// fix #998 / §28.3: plain `var` source Array<byte> is disposed exactly once at
// scope end when passed to CString.FromBytes (covers the CString factory path).
BEZEL_TEST(rawbytes_disposal, var_source_cstring_frombytes_leak_free_998) {
    expectValueAndLeakFree(k998CStringFromBytesVarSource, 3);
}

// fix #998 / §28.3: same local passed to multiple FFI calls — alreadyReg guard
// prevents double disposal; no double-free under ASan (detect_leaks=1).
BEZEL_TEST(rawbytes_disposal, multiple_ffi_calls_same_local_no_double_free_998) {
    expectValueAndLeakFree(k998MultipleFFICallsSameLocal, 6);
}

// fix #998 / §28.3: `using var b` form — isUsing guard prevents adding a second
// disposal entry on top of registerUsingDisposal's; no double-free.
BEZEL_TEST(rawbytes_disposal, using_var_source_no_double_free_998) {
    expectValueAndLeakFree(k998UsingVarSourceNoDoubleFree, 3);
}

// fix #998 / §28.3: two distinct plain `var` source arrays — each gets its own
// disposal entry; both freed exactly once (no leak, no double-free).
BEZEL_TEST(rawbytes_disposal, two_distinct_sources_leak_free_998) {
    expectValueAndLeakFree(k998TwoDistinctSources, 5);
}
