// Codegen — Console.Write/WriteLine of non-ASCII string literals must emit
// valid UTF-8 to stdout (issue #592, HIGH miscompile).
//
// Root cause (before this fix): emitConsoleWrite called write(fd, data, len)
// where `data` was the raw per-string-width data pointer (uint8_t* for width 1,
// uint16_t* for width 2, uint32_t* for width 4) and `len` was the code-point
// COUNT, not the byte count.  A width-1 string stores Latin-1 code points in
// uint8_t, so U+00E9 ('é') became the single raw byte 0xE9 — not valid UTF-8.
// A width-2 string stores code points in uint16_t, so U+4E16 ('世') became the
// two raw bytes 0x16 0x4E (the LE encoding of the uint16 value) instead of the
// three UTF-8 bytes 0xE4 0xB8 0x96.
//
// Fix (LlvmBackend_Calls.cpp, Emitter::emitConsoleWrite): re-encode via the
// existing runtime helper bz_string_to_utf8z (already used by the FFI
// CString marshalling path), determine the byte count with strlen, write the
// UTF-8 bytes, then free the malloc-allocated buffer.
//
// Spec derivation:
//   §4.6 — string is Unicode; terminal output is expected to be UTF-8
//   §4.6 / issue #528 / issue #535 — per-string storage width (1/2/4 bytes
//         per code point, PEP 393 model)
//   stdlib §2.1 — Console.WriteLine / Console.Write contract

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "driver/Driver.hpp"
#include "support/codegen_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "native_test_harness.hpp"
#include "test_harness.hpp"

#include "codegen/LlvmBackend.hpp"
#include "lexer/Lexer.hpp"
#include "ownership/Ownership.hpp"
#include "parser/Parser.hpp"
#include "sema/DefiniteAssignment.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include <sys/wait.h>

namespace {

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit.
static const std::filesystem::path& wtDir592() {
    return wtScratchDir("wt-592");
}

// Compile `source` to a temp native executable via the Driver and capture its
// stdout. Returns the raw captured bytes, or an empty string on failure.
// Uses a stable per-test counter so concurrent runs produce distinct paths.
std::string compileAndCapture(const std::string& source) {
    auto dir = wtDir592();
    std::string stem = unique_stem("bezel_592");
    std::string src = (dir / (stem + ".bzl")).string();
    std::string exe = (dir / stem).string();
    { std::ofstream out(src); out << source; }

    bezel::Driver driver;
    bezel::CompileResult r = driver.compileFile(src, exe);
    std::string out;
    if (r.success) {
        if (FILE* pipe = popen(exe.c_str(), "r")) {
            std::array<char, 512> buf;
            while (std::size_t n = std::fread(buf.data(), 1, buf.size(), pipe))
                out.append(buf.data(), n);
            pclose(pipe);
        }
    }
    std::remove(src.c_str());
    std::remove(exe.c_str());
    return out;
}

// Lower `source` to an object file, link against the ASan-instrumented
// runtime, and run under LeakSanitizer.  Returns 0 (clean), 1 (leak
// detected), or -1 (toolchain unavailable → SKIP).
int runUnderLeakSanitizer(const std::string& source) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;

    auto dir = wtDir592();
    std::string stem = unique_stem("bezel_592_lsan");
    std::string objPath = (dir / (stem + ".o")).string();
    std::string exe = (dir / stem).string();

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
    std::string runCmd =
        "ASAN_OPTIONS=detect_leaks=1:exitcode=99 " + exe + " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return -1;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 1;  // leak
    return 0;                                                       // clean
}

const char* kPrefix =
    "module M; class C public static method Main(): int ";
const char* kSuffix = " end Main; end C;";

std::string wrap(const std::string& body) {
    return std::string(kPrefix) + body + kSuffix;
}

}  // namespace

// §4.6: a pure-ASCII string literal must still produce unchanged output after
// the UTF-8 re-encoding path (bz_string_to_utf8z is a no-op for ASCII).
BEZEL_TEST(codegen_console_write_unicode_592, ascii_hello_unchanged) {
    std::string out = compileAndCapture(
        wrap("Console.WriteLine(\"Hello\"); return 0;"));
    BEZEL_EXPECT_EQ(out, std::string("Hello\n"));
}

// §4.6: a width-1 Latin-extended literal ("café", U+00E9 = 0xC3 0xA9 in
// UTF-8) must NOT be emitted as the raw 0xE9 byte.
// Expected UTF-8: 63 61 66 C3 A9 0A ("café\n").
BEZEL_TEST(codegen_console_write_unicode_592, latin_extended_cafe_utf8) {
    // "café" — é is U+00E9, stored as byte 0xE9 in a width-1 bz_string.
    // Before the fix: write emitted 0xE9 (Latin-1 raw byte, not UTF-8).
    // After  the fix: 0xC3 0xA9 (correct 2-byte UTF-8 sequence for U+00E9).
    std::string out = compileAndCapture(
        wrap("Console.WriteLine(\"caf\xC3\xA9\"); return 0;"));
    // Expected: UTF-8 bytes for "café\n"
    std::string expected = "caf\xC3\xA9\n";
    BEZEL_EXPECT_EQ(out, expected);
}

// §4.6: a CJK literal ("世界", U+4E16 U+754C) lives in a width-2 bz_string.
// Before the fix: write emitted the raw uint16_t bytes (2 bytes per char),
// producing garbled output.  After the fix: valid 3-byte UTF-8 per code point.
// Expected UTF-8: E4 B8 96 E7 95 8C 0A ("世界\n").
BEZEL_TEST(codegen_console_write_unicode_592, cjk_shijie_utf8) {
    std::string out = compileAndCapture(
        wrap("Console.WriteLine(\"\xE4\xB8\x96\xE7\x95\x8C\"); return 0;"));
    std::string expected = "\xE4\xB8\x96\xE7\x95\x8C\n";
    BEZEL_EXPECT_EQ(out, expected);
}

// §4.6: a 4-byte emoji literal (U+1F600 GRINNING FACE) lives in a width-4
// bz_string (code point > U+FFFF).  Before the fix: raw uint32_t bytes.
// After the fix: valid 4-byte UTF-8 sequence F0 9F 98 80.
BEZEL_TEST(codegen_console_write_unicode_592, emoji_grinning_utf8) {
    std::string out = compileAndCapture(
        wrap("Console.WriteLine(\"\xF0\x9F\x98\x80\"); return 0;"));
    std::string expected = "\xF0\x9F\x98\x80\n";
    BEZEL_EXPECT_EQ(out, expected);
}

// §4.6 / §18.5: interpolation with a non-ASCII literal part must also emit
// valid UTF-8 (the interpolation path routes through the same emitConsoleWrite).
BEZEL_TEST(codegen_console_write_unicode_592, interpolation_with_non_ascii) {
    // $"Hello, {name}!" where name = "世界" should print "Hello, 世界!\n"
    std::string out = compileAndCapture(
        wrap("var name := \"\xE4\xB8\x96\xE7\x95\x8C\"; "
             "Console.WriteLine($\"Hello, {name}!\"); return 0;"));
    std::string expected = "Hello, \xE4\xB8\x96\xE7\x95\x8C!\n";
    BEZEL_EXPECT_EQ(out, expected);
}

// §4.6: Console.Write (no newline) must also emit valid UTF-8.
BEZEL_TEST(codegen_console_write_unicode_592, write_no_newline_utf8) {
    std::string out = compileAndCapture(
        wrap("Console.Write(\"\xC3\xA9\"); return 0;"));
    std::string expected = "\xC3\xA9";  // just é, no trailing newline
    BEZEL_EXPECT_EQ(out, expected);
}

// §4.6: the UTF-8 temp buffer allocated by bz_string_to_utf8z must be freed
// after each write — verify no leak under LeakSanitizer.  Two non-ASCII
// writes exercise two malloc/free cycles.
BEZEL_TEST(codegen_console_write_unicode_592, no_leak_after_non_ascii_write) {
    std::string src = wrap(
        "Console.WriteLine(\"\xC3\xA9\"); "
        "Console.WriteLine(\"\xE4\xB8\x96\xE7\x95\x8C\"); "
        "return 0;");
    int lsan = runUnderLeakSanitizer(src);
    if (lsan == -1) return;   // toolchain unavailable: skip
    BEZEL_EXPECT_EQ(lsan, 0); // 0 == LSan clean (no leaked UTF-8 buffer)
}
