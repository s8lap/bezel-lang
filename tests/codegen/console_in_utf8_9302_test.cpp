// Regression for #9302 — the standard-input readers read BYTES, not characters.
//
// THE ISSUE (stdlib §7.4 / §2.1, v0.359; follow-up of #9260).
// `Console.In.Read()` returned one BYTE of standard input, not the next `char`:
// for the input `é\n` (UTF-8 C3 A9 0A) a read-to-EOF loop printed 195, 169, 10
// (count 3), where the next code points are 233, 10 (count 2). stdlib §7.4:
// `Read(): int` is "next char as int, or -1 at EOF"; spec §3 makes `char` a
// Unicode scalar value and §1.1 makes strings UTF-8 text, so a multi-byte
// sequence is ONE read. §2.1's `ReadKey()` is "one key (code point)". The same
// holds for a LINE: `ReadLine()` returned the bytes of `é` as two width-1 code
// points (Length 2, not 1), so a line with non-ASCII text was wrong text.
//
// THE FIX. ONE decoding behind every spelling — `Console.ReadLine()`,
// `Console.In.ReadLine()` and the typed `TextReader` spelling (all
// `Emitter::emitConsoleReadLine` -> the runtime's `bz_console_read_line`), and
// `Console.In.Read()` / `Console.ReadKey()` (`Emitter::emitConsoleReadChar` ->
// `bz_console_read_char`): the UTF-8 is decoded by the strict decoder
// `string.From(CString)` uses, into a string of the §4.6 narrowest width.
//
// NOT PINNED. Malformed UTF-8 on standard input: neither §2.1 nor §7.4 says
// what a reader does with it (the spec-question named on #9302), so no row
// asserts it. CR handling is the open spec-question #9305 and is not touched.
//
// THE RUNNER is the #9260 rows' harness — the REAL AOT path, generated-code
// AddressSanitizer + LeakSanitizer ON, fd 0 carrying the row's input.

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "codegen/LlvmBackend.hpp"
#include "driver/Driver.hpp"
#include "lexer/Lexer.hpp"
#include "parser/Parser.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "support/Diagnostic.hpp"
#include "support/aot_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

namespace {

// ---------------------------------------------------------------------------
// The runner — the REAL AOT path, with fd 0 carrying the row's input and the
// leak oracle armed (the #9213 rows' harness).
// ---------------------------------------------------------------------------

struct RunOutcome {
    bool compiled = false;
    int exitCode = -1;
    std::string stdoutText;
    std::string stderrText;
};

const std::filesystem::path& tr9302ScratchDir() {
    return wtScratchDir("wt-tr9302");
}

std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

RunOutcome runWithStdin(const std::string& source,
                        const std::string& stdinText) {
    auto dir = tr9302ScratchDir();
    std::string stem = unique_stem("bezel_tr9302");
    std::string src = (dir / (stem + ".bzl")).string();
    std::string exe = (dir / stem).string();
    std::string outf = (dir / (stem + ".stdout")).string();
    std::string errf = (dir / (stem + ".stderr")).string();
    std::string inf = (dir / (stem + ".stdin")).string();
    {
        std::ofstream out(src);
        out << source;
    }
    {
        std::ofstream in(inf, std::ios::binary);
        in << stdinText;
    }
    RunOutcome o;
    bezel::Driver driver;
    bezel::codegen::CodegenOptions opts;
    // The generated code is instrumented and LeakSanitizer checks it at exit.
    // `abort_on_error=0` so a sanitizer finding PRINTS a report and exits 99
    // rather than SIGABRT, and `exitcode=99` so the finding is
    // distinguishable from the program's own exit status (the #4532 lesson).
    opts.sanitizeAddress = true;
    opts.checkedContracts = true;
    bezel::CompileResult r = driver.compileFile(src, exe, /*deps=*/{}, opts);
    o.compiled = r.success;
    if (r.success) {
        pid_t pid = ::fork();
        if (pid == 0) {
            ::setenv("ASAN_OPTIONS",
                     "detect_leaks=1:exitcode=99:abort_on_error=0", 1);
            int fd0 = ::open(inf.c_str(), O_RDONLY);
            int fd1 = ::open(outf.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
            int fd2 = ::open(errf.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (fd0 < 0 || fd1 < 0 || fd2 < 0) ::_exit(126);
            ::dup2(fd0, 0);
            ::dup2(fd1, 1);
            ::dup2(fd2, 2);
            ::close(fd0);
            ::close(fd1);
            ::close(fd2);
            ::execl(exe.c_str(), exe.c_str(), (char*)nullptr);
            ::_exit(127);
        }
        if (pid > 0) {
            int status = 0;
            ::waitpid(pid, &status, 0);
            if (WIFEXITED(status)) o.exitCode = WEXITSTATUS(status);
            else if (WIFSIGNALED(status)) o.exitCode = 128 + WTERMSIG(status);
            o.stdoutText = slurp(outf);
            o.stderrText = slurp(errf);
        }
    }
    std::remove(src.c_str());
    std::remove(exe.c_str());
    std::remove(outf.c_str());
    std::remove(errf.c_str());
    std::remove(inf.c_str());
    return o;
}

// LeakSanitizer counted nothing: 0 leaked allocations, 0 leaked bytes. LSan
// prints a SUMMARY line only when it finds a leak, so "no summary" is 0. The
// same shape as `expectNoLeak` in the #9213 rows.
void expectNoLeak(const RunOutcome& o) {
    unsigned long long leakedBytes = 0, leakedAllocs = 0;
    const bool sawSummary = bezel::test::aot_detail::parseLeakSummary(
        o.stderrText, leakedBytes, leakedAllocs);
    const unsigned long long leakedAllocations = sawSummary ? leakedAllocs : 0ull;
    const unsigned long long leakedByteCount = sawSummary ? leakedBytes : 0ull;
    BEZEL_EXPECT_EQ(leakedAllocations, 0ull);
    BEZEL_EXPECT_EQ(leakedByteCount, 0ull);
    BEZEL_EXPECT_TRUE(o.stderrText.find("ERROR: LeakSanitizer") ==
                       std::string::npos);
    BEZEL_EXPECT_TRUE(o.stderrText.find("ERROR: AddressSanitizer") ==
                       std::string::npos);
}


std::string mainWith(const std::string& body) {
    return "module P; import System;\n"
           "public class Program\n"
           "  public static method Main(): int\n" + body +
           "    return 0;\n"
           "  end Main;\n"
           "end Program;\n";
}

// Print the next `n` lines of `reader` (`Console.In.ReadLine()` or
// `Console.ReadLine()`): the length, or "null" at EOF.
std::string printLengths(const std::string& reader, int n) {
    std::string b;
    for (int k = 0; k < n; ++k) {
        b += "    var l" + std::to_string(k) + " := " + reader + ";\n"
             "    if l" + std::to_string(k) + " <> null then Console.WriteLine(l" +
             std::to_string(k) + ".Length); else Console.WriteLine(\"null\"); end;\n";
    }
    return mainWith(b);
}

// Read to EOF with `Console.In.Read()`, printing each code point and the count.
const char* kReadAll =
    "module P; import System;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    var n := Console.In.Read();\n"
    "    var k := 0;\n"
    "    while n <> -1 do\n"
    "      k := k + 1;\n"
    "      Console.WriteLine(n);\n"
    "      n := Console.In.Read();\n"
    "    end;\n"
    "    Console.WriteLine($\"count={k} last={n}\");\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

}  // namespace

// ===========================================================================
// #9302 — Read() is the next CODE POINT.
// ===========================================================================

// The issue's own program: `é\n` is two reads (233, 10). Base: 195 169 10, count=3.
BEZEL_TEST(console_in_utf8_9302, read_returns_a_two_byte_code_point_7_4) {
    auto o = runWithStdin(kReadAll, "\xC3\xA9\n");
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_EQ(o.stdoutText, std::string("233\n10\ncount=2 last=-1\n"));
    expectNoLeak(o);
}

// A 3-byte (U+20AC) and a 4-byte (U+1F600) code point are one read each.
BEZEL_TEST(console_in_utf8_9302, read_returns_three_and_four_byte_code_points_7_4) {
    auto o = runWithStdin(kReadAll, "\xE2\x82\xAC\xF0\x9F\x98\x80z");
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_EQ(o.stdoutText,
                    std::string("8364\n128512\n122\ncount=3 last=-1\n"));
    expectNoLeak(o);
}

// EOF without a trailing newline: the last code point is still one read.
BEZEL_TEST(console_in_utf8_9302, read_of_a_final_unterminated_code_point_7_4) {
    auto o = runWithStdin(kReadAll, "a\xC3\xA9");
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_EQ(o.stdoutText, std::string("97\n233\ncount=2 last=-1\n"));
    expectNoLeak(o);
}

// §2.1 `Console.ReadKey()` is "one key (code point)": the SAME reader.
BEZEL_TEST(console_in_utf8_9302, read_key_is_one_code_point_2_1) {
    auto o = runWithStdin(
        mainWith("    var k := Console.ReadKey();\n"
                 "    Console.WriteLine(k);\n"),
        "\xC3\xA9");
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_EQ(o.stdoutText, std::string("\xC3\xA9\n"));
    expectNoLeak(o);
}

// ReadLine decodes the line: `é😀x` is THREE characters, and it prints back as
// the same UTF-8 text. Base: Length 6 (one code point per byte), and the
// printed text is mojibake.
BEZEL_TEST(console_in_utf8_9302, read_line_decodes_the_line_7_4) {
    auto o = runWithStdin(
        mainWith("    var l := Console.In.ReadLine();\n"
                 "    if l <> null then\n"
                 "      Console.WriteLine(l.Length);\n"
                 "      Console.WriteLine(l);\n"
                 "      if l = \"\xC3\xA9\xF0\x9F\x98\x80x\" then Console.WriteLine(\"same\"); end;\n"
                 "    end;\n"),
        "\xC3\xA9\xF0\x9F\x98\x80x\n");
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_EQ(o.stdoutText,
                    std::string("3\n\xC3\xA9\xF0\x9F\x98\x80x\nsame\n"));
    expectNoLeak(o);
}

// §2.1's own `Console.ReadLine()` is the same reader: the spellings agree.
BEZEL_TEST(console_in_utf8_9302, console_read_line_decodes_the_line_2_1) {
    auto o = runWithStdin(
        mainWith("    var l := Console.ReadLine();\n"
                 "    if l <> null then Console.WriteLine(l.Length); end;\n"),
        "\xC3\xA9\xC3\xA9\n");
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_EQ(o.stdoutText, std::string("2\n"));
    expectNoLeak(o);
}

// The typed `TextReader` spelling reads the same way.
BEZEL_TEST(console_in_utf8_9302, typed_text_reader_read_line_decodes_the_line_7_4) {
    auto o = runWithStdin(
        mainWith("    var r: TextReader := Console.In;\n"
                 "    var l := r.ReadLine();\n"
                 "    if l <> null then Console.WriteLine(l.Length); end;\n"),
        "\xC3\xA9\xC3\xA9\xC3\xA9\n");
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_EQ(o.stdoutText, std::string("3\n"));
    expectNoLeak(o);
}

// A final line with no terminator is still a line, decoded.
BEZEL_TEST(console_in_utf8_9302, read_line_of_a_final_unterminated_line_7_4) {
    auto o = runWithStdin(printLengths("Console.In.ReadLine()", 2), "\xC3\xA9");
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_EQ(o.stdoutText, std::string("1\nnull\n"));
    expectNoLeak(o);
}

// Read / ReadLine INTERLEAVE on one stream: Read takes `é`, ReadLine the rest
// of the line, Read the next line's first char, ReadLine the rest (a 4-byte
// code point and a letter).
BEZEL_TEST(console_in_utf8_9302, read_and_read_line_interleave_7_4) {
    auto o = runWithStdin(
        mainWith("    var a := Console.In.Read();\n"
                 "    var l1 := Console.In.ReadLine();\n"
                 "    var b := Console.In.Read();\n"
                 "    var l2 := Console.In.ReadLine();\n"
                 "    Console.WriteLine(a);\n"
                 "    if l1 <> null then Console.WriteLine(l1); end;\n"
                 "    Console.WriteLine(b);\n"
                 "    if l2 <> null then Console.WriteLine(l2); end;\n"),
        "\xC3\xA9" "ab\nb\xF0\x9F\x98\x80" "c\n");
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_EQ(o.stdoutText,
                    std::string("233\nab\n98\n\xF0\x9F\x98\x80" "c\n"));
    expectNoLeak(o);
}

// A long line of NON-ASCII text: 3000 `é` is 6000 bytes and 3000 characters.
BEZEL_TEST(console_in_utf8_9302, a_long_non_ascii_line_is_decoded_whole_7_4) {
    std::string e;
    for (int k = 0; k < 3000; ++k) e += "\xC3\xA9";
    auto o = runWithStdin(printLengths("Console.In.ReadLine()", 2), e + "\n");
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_EQ(o.stdoutText, std::string("3000\nnull\n"));
    expectNoLeak(o);
}

// ===========================================================================
// The leak oracle: ReadLine in a 200-iteration loop over non-ASCII lines mints
// one decoded string per read, and every one is released.
// ===========================================================================
BEZEL_TEST(console_in_utf8_9302, read_line_loop_of_decoded_lines_does_not_leak_4_6) {
    std::string in;
    for (int k = 0; k < 250; ++k) in += "h\xC3\xA9llo\n";
    auto o = runWithStdin(
        "module P;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var total := 0;\n"
        "    var i := 0;\n"
        "    while i < 250 do\n"
        "      var l := Console.In.ReadLine();\n"
        "      if l <> null then total := total + l.Length; end;\n"
        "      i := i + 1;\n"
        "    end;\n"
        "    Console.WriteLine(total);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n",
        in);
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_EQ(o.stdoutText, std::string("1250\n"));
    expectNoLeak(o);
}
