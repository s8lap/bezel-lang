// Codegen + runtime — Encoding (stdlib §3.2). Implements the UTF-8 concrete
// encoding reached through the `Encoding.Utf8` singleton:
//
//   public abstract class Encoding
//     public abstract method GetBytes(s: string): owned Array<byte>;       // §3.2
//     public abstract method GetString(bytes: Array<byte>): string;        // §3.2
//     public abstract method GetString(bytes: Array<byte>,
//                                       index: int, count: int): string;    // §3.2
//     public static readonly property Utf8: Encoding;                       // §3.2
//   end Encoding;
//
// Scope of THIS milestone: `Encoding.Utf8` only. The Utf16/Utf32/Ascii
// singletons are rejected in sema (their GetBytes/GetString semantics —
// endianness, surrogate handling, Ascii out-of-range — are not pinned down by
// §3.2), so a sound implementation is deferred rather than guessed (CLAUDE.md §4).
//
// Three layers:
//   - Runtime: bz_encoding_utf8_get_bytes / bz_encoding_utf8_get_string
//     (bezel_runtime.cpp). GetBytes produces an owned Array<byte> (bz_array_t*,
//     elem_size 1); GetString produces a fresh +1 managed string (rc := 1).
//   - Codegen: tryLowerEncodingCall (LlvmBackend_Calls.cpp) — marshals the
//     `Encoding.Utf8.GetBytes/GetString` two-level member-access shape.
//   - Sema: types `Encoding.Utf8` (the singleton) and the GetBytes/GetString
//     results (ExprTypecheck_Calls.cpp).
//
// Value tests run through the JIT harness (exit code = Main()'s return). The
// GetString path ALSO gets a no-corruption (ASan) check: a string produced by a
// call is conservatively left leaked by the codegen (the documented string
// lifetime model — see string_refcount_test.cpp), so we assert no UAF / double-
// free (expectValueAndNoCorruption) rather than leak-freedom. The GetBytes path,
// whose owned Array<byte> IS released by `using var`, gets a full leak-free check.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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
#include "support/Diagnostic.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

using bezel::test::runJit;

namespace {

namespace fs = std::filesystem;

int run(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}

// True if the program failed to compile (a diagnostic was reported and no
// module was produced). Used by the rejection tests for Utf16/Utf32/Ascii.
bool compileFails(const std::string& source) {
    bezel::DiagnosticEngine diags;
    bezel::Lexer lexer(source, diags);
    bezel::Parser parser(lexer, diags);
    auto unit = parser.parseCompilationUnit();
    if (!unit) return true;
    bezel::sema::SemaContext ctx(diags);
    bezel::sema::collectSymbols(*unit, ctx);
    bezel::sema::resolveTypes(*unit, ctx);
    bezel::sema::typecheckExpressions(*unit, ctx);
    bezel::sema::checkDefiniteAssignment(*unit, ctx);
    bezel::ownership::checkOwnership(*unit, ctx);
    return diags.hasErrors();
}

std::string runtimeSourcePath() {
    fs::path here(__FILE__);                       // tests/codegen/<this>
    fs::path root = here.parent_path()             // tests/codegen
                        .parent_path()             // tests
                        .parent_path();            // repo root
    return (root / "src" / "runtime" / "bezel_runtime.cpp").string();
}

// Lower `source` to an object file via the sema+codegen pipeline; returns true
// on success (object written to objPath).
bool lowerToObject(const std::string& source, const std::string& objPath) {
    bezel::DiagnosticEngine diags;
    bezel::Lexer lexer(source, diags);
    bezel::Parser parser(lexer, diags);
    auto unit = parser.parseCompilationUnit();
    if (!unit) return false;
    bezel::sema::SemaContext ctx(diags);
    bezel::sema::collectSymbols(*unit, ctx);
    bezel::sema::resolveTypes(*unit, ctx);
    bezel::sema::typecheckExpressions(*unit, ctx);
    bezel::sema::checkDefiniteAssignment(*unit, ctx);
    bezel::ownership::checkOwnership(*unit, ctx);
    if (diags.hasErrors()) return false;
    llvm::LLVMContext llvmCtx;
    auto module = bezel::codegen::emitModule(*unit, ctx, llvmCtx);
    if (!module) return false;
    std::string err;
    return bezel::codegen::emitObjectFile(*module, objPath, err);
}

// Build + link `source` against an ASan-built runtime and run it under ASan.
// `detectLeaks` selects leak detection. Returns:
//   the program's exit code on a clean run,
//   99                       on an ASan-reported error (UAF / double-free / leak),
//   -1                       if the toolchain is unavailable (SKIP).
int runUnderAsan(const std::string& source, bool detectLeaks) {
    if (std::system("clang++ --version > /dev/null 2>&1") != 0) return -1;
    static int counter = 0;
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_enc_" + std::to_string(counter++);
    std::string objPath = (dir / (stem + ".o")).string();
    std::string rtObj   = (dir / (stem + "_rt.o")).string();
    std::string exe     = (dir / stem).string();
    auto cleanup = [&] {
        std::remove(objPath.c_str());
        std::remove(rtObj.c_str());
        std::remove(exe.c_str());
    };
    if (!lowerToObject(source, objPath)) { cleanup(); return -1; }
    std::string rtCmd =
        "clang++ -std=c++20 -g -fsanitize=address -pthread -c " +
        runtimeSourcePath() + " -o " + rtObj + " > /dev/null 2>&1";
    if (std::system(rtCmd.c_str()) != 0) { cleanup(); return -1; }
    std::string linkCmd = "clang++ -fsanitize=address -g " + objPath + " " +
                          rtObj + " -lm -lpthread -lstdc++ -o " + exe +
                          " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) { cleanup(); return -1; }
    std::string opts = detectLeaks ? "detect_leaks=1" : "detect_leaks=0";
    std::string runCmd = "ASAN_OPTIONS=" + opts +
                         ":exitcode=99:abort_on_error=0 " + exe +
                         " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return -1;
    if (!WIFEXITED(status)) return 99;
    return WEXITSTATUS(status);
}

// Assert the value AND that the GetBytes byte-array path is fully leak-free
// (the owned Array<byte> is discharged by `using var`; no string is produced).
void expectValueAndLeakFree(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(run(source), expectedExitCode);
    int code = runUnderAsan(source, /*detectLeaks=*/true);
    if (code == -1) return;                 // toolchain unavailable: skip
    BEZEL_EXPECT_EQ(code, expectedExitCode);  // no leak, no corruption
}

// Assert the value AND that the GetString allocating path has no memory-safety
// error (UAF / double-free). A leak is tolerated: a string produced by a call is
// conservatively left leaked by codegen (the documented string lifetime model),
// so we run with leak detection OFF and require a clean exit at the right value.
void expectValueAndNoCorruption(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(run(source), expectedExitCode);
    int code = runUnderAsan(source, /*detectLeaks=*/false);
    if (code == -1) return;                 // toolchain unavailable: skip
    BEZEL_EXPECT_EQ(code, expectedExitCode);  // no UAF / double-free
}

// === GetBytes: byte values (§3.2) ======================================

// §3.2: Encoding.Utf8.GetBytes("Hello") yields the 5 ASCII bytes H,e,l,l,o.
BEZEL_TEST(codegen_encoding, utf8_getbytes_ascii_length_spec_3_2) {
    expectValueAndLeakFree(
        "module M; class C public static method Main(): int "
        "  var s := \"Hello\"; "
        "  using var b: Array<byte> := Encoding.Utf8.GetBytes(s); "
        "  return b.Length; "
        "end Main; end C;", 5);
}

// §3.2: the byte at index 0 of GetBytes("Hello") is the ASCII code of 'H' (72).
BEZEL_TEST(codegen_encoding, utf8_getbytes_ascii_byte_value_spec_3_2) {
    expectValueAndLeakFree(
        "module M; class C public static method Main(): int "
        "  var s := \"Hello\"; "
        "  using var b: Array<byte> := Encoding.Utf8.GetBytes(s); "
        "  return int(b.Get(0)); "
        "end Main; end C;", 72);
}

// §3.2: every ASCII byte of GetBytes("ABC") matches the ASCII codes (65,66,67).
BEZEL_TEST(codegen_encoding, utf8_getbytes_ascii_all_bytes_spec_3_2) {
    expectValueAndLeakFree(
        "module M; class C public static method Main(): int "
        "  var s := \"ABC\"; "
        "  using var b: Array<byte> := Encoding.Utf8.GetBytes(s); "
        "  var ok := b.Get(0) = byte(65) and b.Get(1) = byte(66) and b.Get(2) = byte(67); "
        "  return (1 if ok else 0); "
        "end Main; end C;", 1);
}

// §3.2: the empty string yields an empty byte array.
BEZEL_TEST(codegen_encoding, utf8_getbytes_empty_spec_3_2) {
    expectValueAndLeakFree(
        "module M; class C public static method Main(): int "
        "  var s := \"\"; "
        "  using var b: Array<byte> := Encoding.Utf8.GetBytes(s); "
        "  return b.Length; "
        "end Main; end C;", 0);
}

// §3.2: a non-ASCII code point (é = U+00E9) UTF-8-encodes to two bytes
// (0xC3 0xA9 = 195, 169), so "café" (4 code points) yields 5 bytes.
BEZEL_TEST(codegen_encoding, utf8_getbytes_multibyte_length_spec_3_2) {
    expectValueAndLeakFree(
        "module M; class C public static method Main(): int "
        "  var s := \"caf\\u00e9\"; "
        "  using var b: Array<byte> := Encoding.Utf8.GetBytes(s); "
        "  return b.Length; "
        "end Main; end C;", 5);
}

// §3.2: the two continuation bytes of é (U+00E9) are 0xC3 (195) and 0xA9 (169).
BEZEL_TEST(codegen_encoding, utf8_getbytes_multibyte_bytes_spec_3_2) {
    expectValueAndLeakFree(
        "module M; class C public static method Main(): int "
        "  var s := \"caf\\u00e9\"; "
        "  using var b: Array<byte> := Encoding.Utf8.GetBytes(s); "
        "  var ok := b.Get(3) = byte(195) and b.Get(4) = byte(169); "
        "  return (1 if ok else 0); "
        "end Main; end C;", 1);
}

// === GetString and round-trip (§3.2) ===================================

// §3.2: GetString(GetBytes(s)) == s for an ASCII string (lossless round-trip).
BEZEL_TEST(codegen_encoding, utf8_roundtrip_ascii_spec_3_2) {
    expectValueAndNoCorruption(
        "module M; class C public static method Main(): int "
        "  var s := \"Hello, World\"; "
        "  using var b: Array<byte> := Encoding.Utf8.GetBytes(s); "
        "  using var back: string := Encoding.Utf8.GetString(b); "
        "  return (1 if back = s else 0); "
        "end Main; end C;", 1);
}

// §3.2: the round-tripped string has the same code-point length as the source.
BEZEL_TEST(codegen_encoding, utf8_roundtrip_ascii_length_spec_3_2) {
    expectValueAndNoCorruption(
        "module M; class C public static method Main(): int "
        "  var s := \"Hello\"; "
        "  using var b: Array<byte> := Encoding.Utf8.GetBytes(s); "
        "  using var back: string := Encoding.Utf8.GetString(b); "
        "  return back.Length; "
        "end Main; end C;", 5);
}

// §3.2: GetString(GetBytes(s)) == s for a multi-byte UTF-8 string (é = U+00E9).
BEZEL_TEST(codegen_encoding, utf8_roundtrip_multibyte_spec_3_2) {
    expectValueAndNoCorruption(
        "module M; class C public static method Main(): int "
        "  var s := \"caf\\u00e9 \\u00fcber\"; "
        "  using var b: Array<byte> := Encoding.Utf8.GetBytes(s); "
        "  using var back: string := Encoding.Utf8.GetString(b); "
        "  return (1 if back = s else 0); "
        "end Main; end C;", 1);
}

// §3.2: the three-argument GetString(bytes, index, count) decodes the sub-range
// [index, index+count). GetString(GetBytes("Hello"), 1, 3) == "ell".
BEZEL_TEST(codegen_encoding, utf8_getstring_index_count_spec_3_2) {
    expectValueAndNoCorruption(
        "module M; class C public static method Main(): int "
        "  var s := \"Hello\"; "
        "  using var b: Array<byte> := Encoding.Utf8.GetBytes(s); "
        "  using var sub: string := Encoding.Utf8.GetString(b, 1, 3); "
        "  return (1 if sub = \"ell\" and sub.Length = 3 else 0); "
        "end Main; end C;", 1);
}

// §3.2: GetString of an empty byte array is the empty string.
BEZEL_TEST(codegen_encoding, utf8_getstring_empty_spec_3_2) {
    expectValueAndNoCorruption(
        "module M; class C public static method Main(): int "
        "  var s := \"\"; "
        "  using var b: Array<byte> := Encoding.Utf8.GetBytes(s); "
        "  using var back: string := Encoding.Utf8.GetString(b); "
        "  return back.Length; "
        "end Main; end C;", 0);
}

// === Deferred singletons rejected (§3.2 / CLAUDE.md §4) =================

// §3.2: Encoding.Ascii is not yet implemented — the program must fail to compile
// (decline-with-reasons rather than guess unspecified semantics).
BEZEL_TEST(codegen_encoding, ascii_singleton_rejected_spec_3_2) {
    BEZEL_EXPECT_TRUE(compileFails(
        "module M; class C public static method Main(): int "
        "  var s := \"Hi\"; "
        "  using var b: Array<byte> := Encoding.Ascii.GetBytes(s); "
        "  return b.Length; "
        "end Main; end C;"));
}

// §3.2: Encoding.Utf16 is not yet implemented — must fail to compile.
BEZEL_TEST(codegen_encoding, utf16_singleton_rejected_spec_3_2) {
    BEZEL_EXPECT_TRUE(compileFails(
        "module M; class C public static method Main(): int "
        "  var s := \"Hi\"; "
        "  using var b: Array<byte> := Encoding.Utf16.GetBytes(s); "
        "  return b.Length; "
        "end Main; end C;"));
}

// §3.2: Encoding.Utf32 is not yet implemented — must fail to compile.
BEZEL_TEST(codegen_encoding, utf32_singleton_rejected_spec_3_2) {
    BEZEL_EXPECT_TRUE(compileFails(
        "module M; class C public static method Main(): int "
        "  var s := \"Hi\"; "
        "  using var b: Array<byte> := Encoding.Utf32.GetBytes(s); "
        "  return b.Length; "
        "end Main; end C;"));
}

// §3.2: an `Encoding` value is reachable ONLY as the immediate receiver of a
// direct `Encoding.Utf8.GetBytes/GetString` call — there is no first-class
// singleton object yet. Binding `Encoding.Utf8` to a local and calling through
// it must fail to compile (a clear decline-with-reasons, not a miscompile).
BEZEL_TEST(codegen_encoding, indirect_singleton_use_rejected_spec_3_2) {
    BEZEL_EXPECT_TRUE(compileFails(
        "module M; class C public static method Main(): int "
        "  var e := Encoding.Utf8; "
        "  var s := \"Hi\"; "
        "  using var b: Array<byte> := e.GetBytes(s); "
        "  return b.Length; "
        "end Main; end C;"));
}

}  // namespace
