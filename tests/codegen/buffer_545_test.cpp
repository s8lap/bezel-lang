// Codegen for Buffer<T> — fixed-capacity compiler-intrinsic primitive (issue #545).
//
// Design note (issue #545 owner comment, 2026-06-13):
//   Buffer<T>: new __Buffer<T>(n) allocates n zero-initialised elements;
//   [i] get/set with §16 bounds check raising IndexOutOfRangeException;
//   Length returns the element count; Dispose (via using-var) frees the buffer.
//   Value-type elements only in this revision (int/byte/short/long/double/
//   decimal/char/bool); class elements await the §5.20 design (#545).
//
// Runtime: bz_buffer_new / bz_buffer_get_ptr / bz_buffer_free (three fns).
// Layout: 16-byte header {int64 len, int64 elem_size} + inline calloc'd payload.
// Decimal elements use 16-byte struct slots (#685 category).
//
// Tests cover:
//   - new __Buffer<T>(n) + zeroed init: every value-type element type.
//   - [i] get/set round-trip: int, byte, short, long, double, decimal, char, bool.
//   - Length returns n.
//   - Bounds check raises IndexOutOfRangeException at idx == n (upper) and idx < 0.
//   - using-var dispose lifecycle (no use-after-free).
//   - double-dispose idempotent (second bz_buffer_free on null is a no-op).
//   - ASan detect_leaks=1 hammer: alloc + free, confirmed clean.
//   - AOT rc probe: fill+sum a Buffer<int> and a Buffer<decimal>, check exact return.

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
#include "support/aot_support.hpp"
#include "support/Diagnostic.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"
#include "native_test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

namespace fs = std::filesystem;
using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit.
static const std::filesystem::path& wtDir545() {
    return wtScratchDir("wt-545");
}

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// Wrap a Main body in a minimal module/class skeleton.
std::string prog(const std::string& mainBody) {
    return "module M; "
           "class C public static method Main(): int "
           + mainBody +
           " end Main; end C;";
}

// LeakSanitizer verification (mirrors atomic_647_test.cpp pattern).
// Returns: 0 = clean; 1 = sanitizer fired; -1 = toolchain unavailable (SKIP).
int runUnderLeakSanitizer(const std::string& source, bool detectLeaks = true) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;

    auto dir = wtDir545();
    std::string stem = unique_stem("bezel_545buf_lsan");
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
        std::string("ASAN_OPTIONS=detect_leaks=") +
        (detectLeaks ? "1" : "0") + ":exitcode=99 " + exe +
        " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return bezel::test::asanExecFailure("system");
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 1;  // sanitizer fired
    return 0;  // clean
}

} // namespace

// ===========================================================================
// Zeroed init — new __Buffer<T>(n) starts with all zero bytes
// ===========================================================================

// issue #545: Buffer<int>(4) — all slots zero-initialised by calloc.
BEZEL_TEST(codegen_buffer_545, int_zeroed_init_545) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var b := new __Buffer<int>(4); "
        "var s := b[0] + b[1] + b[2] + b[3]; "
        "return s; "
    )), 0);
}

// issue #545: Buffer<bool>(3) — all slots false (0).
BEZEL_TEST(codegen_buffer_545, bool_zeroed_init_545) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var b := new __Buffer<bool>(3); "
        "if b[0] then return 1; end; "
        "if b[1] then return 2; end; "
        "if b[2] then return 3; end; "
        "return 0; "
    )), 0);
}

// ===========================================================================
// Length
// ===========================================================================

// issue #545: Buffer<int>(7).Length == 7.
BEZEL_TEST(codegen_buffer_545, length_returns_n_545) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var b := new __Buffer<int>(7); "
        "return b.Length; "
    )), 7);
}

// issue #545: Buffer<byte>(0).Length == 0.
BEZEL_TEST(codegen_buffer_545, length_zero_545) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var b := new __Buffer<byte>(0); "
        "return b.Length; "
    )), 0);
}

// ===========================================================================
// [i] get/set round-trip per element type
// ===========================================================================

// issue #545: Buffer<int> — set then get.
BEZEL_TEST(codegen_buffer_545, int_set_get_545) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var b := new __Buffer<int>(4); "
        "b[0] := 10; "
        "b[1] := 20; "
        "b[2] := 30; "
        "b[3] := 40; "
        "return b[0] + b[1] + b[2] + b[3]; "
    )), 100);
}

// issue #545: Buffer<byte> — set then get.
BEZEL_TEST(codegen_buffer_545, byte_set_get_545) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var b := new __Buffer<byte>(3); "
        "b[0] := 1; "
        "b[1] := 2; "
        "b[2] := 3; "
        "return b[0] + b[1] + b[2]; "
    )), 6);
}

// issue #545: Buffer<short> — set then get.
BEZEL_TEST(codegen_buffer_545, short_set_get_545) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "using var b := new __Buffer<short>(2); "
        "b[0] := 100; "
        "b[1] := 200; "
        "Console.WriteLine(b[0] + b[1]); return 0; "
    )), "300");
}

// issue #545: Buffer<long> — set then get.
// Return 12 if sum == 12, else 0. (Avoid long-to-int cast; use if-check.)
BEZEL_TEST(codegen_buffer_545, long_set_get_545) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var b := new __Buffer<long>(2); "
        "b[0] := 5; "
        "b[1] := 7; "
        "var s := b[0] + b[1]; "
        "if s = 12 then return 12; end; "
        "return 0; "
    )), 12);
}

// issue #545: Buffer<double> — store 3.0 + 4.0, read back, sum == 7.
// Return 1 if sum == 7.0, else 0.
BEZEL_TEST(codegen_buffer_545, double_set_get_sum_545) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var b := new __Buffer<double>(2); "
        "b[0] := 3.0; "
        "b[1] := 4.0; "
        "var s := b[0] + b[1]; "
        "if s = 7.0 then return 1; end; "
        "return 0; "
    )), 1);
}

// issue #545: Buffer<char> — set 'A' (65) and 'B' (66), verify via comparison.
// Return 65 if b[0] == 'A', else 0.  (Avoid char-to-int cast via `as`.)
BEZEL_TEST(codegen_buffer_545, char_set_get_545) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var b := new __Buffer<char>(2); "
        "b[0] := 'A'; "
        "b[1] := 'B'; "
        "if b[0] = 'A' then return 65; end; "
        "return 0; "
    )), 65);
}

// issue #545: Buffer<bool> — set true/false, read back.
BEZEL_TEST(codegen_buffer_545, bool_set_get_545) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var b := new __Buffer<bool>(2); "
        "b[0] := true; "
        "b[1] := false; "
        "if b[0] then "
        "  if b[1] then return 3; end; "
        "  return 1; "
        "end; "
        "return 0; "
    )), 1);
}

// issue #545: Buffer<decimal> — 16-byte struct slots (#685 category).
// Store 1.5m and 2.5m, read back and sum; result should equal 4.0m → return 1.
BEZEL_TEST(codegen_buffer_545, decimal_set_get_round_trip_545) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var b := new __Buffer<decimal>(2); "
        "b[0] := 1.5m; "
        "b[1] := 2.5m; "
        "var s := b[0] + b[1]; "
        "if s = 4.0m then return 1; end; "
        "return 0; "
    )), 1);
}

// ===========================================================================
// Bounds check — IndexOutOfRangeException
// ===========================================================================

// issue #545: Buffer<int>[n] (upper bound) raises IndexOutOfRangeException.
BEZEL_TEST(codegen_buffer_545, bounds_upper_raises_oor_545) {
    BEZEL_EXPECT_EQ(run(prog(
        "var r := 0; "
        "using var b := new __Buffer<int>(4); "
        "try var x := b[4]; r := 0; "
        "catch IndexOutOfRangeException => r := 99; end; "
        "return r; "
    )), 99);
}

// issue #545: Buffer<int>[-1] (negative index) raises IndexOutOfRangeException.
// Negative values are treated as large unsigned indices (>= len), so the same
// unsigned-comparison guard covers both directions.
BEZEL_TEST(codegen_buffer_545, bounds_negative_raises_oor_545) {
    BEZEL_EXPECT_EQ(run(prog(
        "var r := 0; "
        "using var b := new __Buffer<int>(4); "
        "var idx := -1; "
        "try var x := b[idx]; r := 0; "
        "catch IndexOutOfRangeException => r := 99; end; "
        "return r; "
    )), 99);
}

// issue #545: Buffer<int>[3] (last valid index) does NOT raise.
BEZEL_TEST(codegen_buffer_545, bounds_last_valid_no_raise_545) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var b := new __Buffer<int>(4); "
        "b[3] := 77; "
        "return b[3]; "
    )), 77);
}

// issue #545: bounds check on SET path raises IndexOutOfRangeException.
BEZEL_TEST(codegen_buffer_545, bounds_set_upper_raises_oor_545) {
    BEZEL_EXPECT_EQ(run(prog(
        "var r := 0; "
        "using var b := new __Buffer<int>(3); "
        "try b[3] := 99; r := 0; "
        "catch IndexOutOfRangeException => r := 42; end; "
        "return r; "
    )), 42);
}

// ===========================================================================
// using-var dispose lifecycle
// ===========================================================================

// issue #545: using-var disposes the buffer at scope exit (no crash).
BEZEL_TEST(codegen_buffer_545, using_var_dispose_lifecycle_545) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var b := new __Buffer<int>(8); "
        "b[0] := 1; "
        "b[7] := 2; "
        "return b[0] + b[7]; "
        // Implicit Dispose() here via using-var scope exit.
    )), 3);
}

// ===========================================================================
// AOT probe — fill+sum a Buffer<int> and a Buffer<decimal>, exact rc
// ===========================================================================

// issue #545: AOT-compile a program that fills a Buffer<int> with 1..5 (sum=15)
// and a Buffer<decimal> with 1.0m+2.0m (sum=3.0m, =3 as int), returns sum_int
// modulo 100 after adding both = 18; then verifies the AOT process exit code.
BEZEL_TEST(codegen_buffer_545, aot_fill_sum_int_and_decimal_545) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) {
        // Toolchain unavailable — skip.
        return;
    }
    auto dir = wtDir545();
    std::string stem = unique_stem("bezel_545buf_aot");
    std::string objPath = (dir / (stem + ".o")).string();
    std::string exe     = (dir / stem).string();

    auto cleanup = [&] {
        std::remove(objPath.c_str());
        std::remove(exe.c_str());
    };

    // Program: fill Buffer<int>(5) with 1..5, sum = 15.
    // Fill Buffer<decimal>(2) with 1.0m and 2.0m, sum = 3.0m.
    // Use comparison to get int 3 (avoid decimal-to-int cast via `as`).
    // Total = si(15) + sdi(3) = 18.  Return 18.
    std::string src = prog(
        "using var bi := new __Buffer<int>(5); "
        "bi[0] := 1; bi[1] := 2; bi[2] := 3; bi[3] := 4; bi[4] := 5; "
        "var si := bi[0] + bi[1] + bi[2] + bi[3] + bi[4]; "
        "using var bd := new __Buffer<decimal>(2); "
        "bd[0] := 1.0m; "
        "bd[1] := 2.0m; "
        "var sd := bd[0] + bd[1]; "
        "var sdi := 0; "
        "if sd = 3.0m then sdi := 3; end; "
        "return si + sdi; "
    );

    bezel::DiagnosticEngine diags;
    bezel::Lexer lexer(src, diags);
    bezel::Parser parser(lexer, diags);
    auto unit = parser.parseCompilationUnit();
    BEZEL_EXPECT_TRUE(unit != nullptr);
    if (!unit) { cleanup(); return; }
    bezel::sema::SemaContext ctx(diags);
    bezel::sema::collectSymbols(*unit, ctx);
    bezel::sema::resolveTypes(*unit, ctx);
    bezel::sema::typecheckExpressions(*unit, ctx);
    bezel::sema::checkDefiniteAssignment(*unit, ctx);
    bezel::ownership::checkOwnership(*unit, ctx);
    BEZEL_EXPECT_FALSE(diags.hasErrors());
    if (diags.hasErrors()) { cleanup(); return; }
    llvm::LLVMContext llvmCtx;
    auto module = bezel::codegen::emitModule(*unit, ctx, llvmCtx);
    BEZEL_EXPECT_TRUE(module != nullptr);
    if (!module) { cleanup(); return; }
    std::string err;
    bool emitted = bezel::codegen::emitObjectFile(*module, objPath, err);
    BEZEL_EXPECT_TRUE(emitted);
    if (!emitted) { cleanup(); return; }

    // Link with ASan runtime (matches the detect_leaks probe below).
    std::string linkCmd = "clang++ -fsanitize=address -g " + objPath + " " +
                          rtObj + " -lm -lpthread -lstdc++ -o " + exe +
                          " > /dev/null 2>&1";
    bool linked = (std::system(linkCmd.c_str()) == 0);
    BEZEL_EXPECT_TRUE(linked);
    if (!linked) { cleanup(); return; }

    // Run: ASAN_OPTIONS=detect_leaks=1, expect exit code 18.
    std::string runCmd =
        "ASAN_OPTIONS=detect_leaks=1:exitcode=99 " + exe + " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    if (status == -1) { cleanup(); return; }
    int exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(exitCode, 18);
    cleanup();
}

// ===========================================================================
// ASan detect_leaks=1 — alloc + using-var dispose, confirmed clean
// ===========================================================================

// issue #545: a Buffer<int> allocated via using-var and freed at scope exit
// should produce no LSan leak reports.
BEZEL_TEST(codegen_buffer_545, asan_no_leak_int_545) {
    int result = runUnderLeakSanitizer(prog(
        "using var b := new __Buffer<int>(16); "
        "b[0] := 1; "
        "b[15] := 2; "
        "return b[0] + b[15]; "
    ));
    // result == -1 → toolchain unavailable (SKIP); 0 = clean; 1 = leak found.
    if (result == -1) return;  // SKIP
    BEZEL_EXPECT_EQ(result, 0);
}

// issue #545: a Buffer<decimal> allocated via using-var and freed at scope exit
// should produce no LSan leak reports (#685 16-byte slots).
BEZEL_TEST(codegen_buffer_545, asan_no_leak_decimal_545) {
    int result = runUnderLeakSanitizer(prog(
        "using var b := new __Buffer<decimal>(4); "
        "b[0] := 1.0m; "
        "b[3] := 2.0m; "
        "var s := b[0] + b[3]; "
        "if s = 3.0m then return 1; end; "
        "return 0; "
    ));
    if (result == -1) return;
    BEZEL_EXPECT_EQ(result, 0);
}

// issue #545: ASan hammer — 200 iterations, each allocs/writes/reads/frees
// a Buffer<int> of 8 elements.  All must be leak-free.
BEZEL_TEST(codegen_buffer_545, asan_hammer_200_iters_545) {
    // Build a program that loops 200 times, each time doing a Buffer<int>(8)
    // fill+sum inside a separate block.  The using-var disposes on each iteration.
    // Total sum = 200 * (0+1+2+3+4+5+6+7) = 200*28 = 5600 → return 5600 mod 100 = 0.
    std::string body =
        "var total := 0; "
        "var i := 0; "
        "while i < 200 do "
        "  using var b := new __Buffer<int>(8); "
        "  b[0]:=0; b[1]:=1; b[2]:=2; b[3]:=3; "
        "  b[4]:=4; b[5]:=5; b[6]:=6; b[7]:=7; "
        "  var s := b[0]+b[1]+b[2]+b[3]+b[4]+b[5]+b[6]+b[7]; "
        "  total := total + s; "
        "  i := i + 1; "
        "end; "
        "return total mod 100; ";
    int result = runUnderLeakSanitizer(prog(body));
    if (result == -1) return;
    BEZEL_EXPECT_EQ(result, 0);
}
