// Regression for #9312 — `Console.In.ReadToEnd()` built its string from raw
// bytes.
//
// THE ISSUE (stdlib §7.4 / §2.1, v0.359; sibling of #9302). `ReadToEnd()`
// stored every BYTE of standard input as one code point of a width-1 string,
// so `é` (UTF-8 C3 A9) came back as TWO characters and printed as mojibake,
// while `ReadLine()` (decoded since #9302) answers ONE. spec §3 makes `char` a
// Unicode scalar value and §1.1 makes strings UTF-8 text.
//
// THE FIX. `ReadToEnd` decodes the UTF-8 into the §4.6 narrowest width, the
// same decoding `bz_console_read_line` applies (runtime `bz_console_read_to_end`).
//
// NOT PINNED. Malformed UTF-8 (spec-silent, #9302) and CR handling (#9305).
//
// THE RUNNER is the #9302 rows' harness — the REAL AOT path, generated-code
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

const std::filesystem::path& tr9312ScratchDir() {
    return wtScratchDir("wt-tr9312");
}

std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

RunOutcome runWithStdin(const std::string& source,
                        const std::string& stdinText) {
    auto dir = tr9312ScratchDir();
    std::string stem = unique_stem("bezel_tr9312");
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

}  // namespace

// The issue's repro: `é` is ONE character. Base: Length 2.
BEZEL_TEST(console_readtoend_utf8_9312, two_byte_code_point_is_one_char_7_4) {
    auto o = runWithStdin(
        mainWith("    var s := Console.In.ReadToEnd();\n"
                 "    Console.WriteLine(s.Length);\n"
                 "    Console.WriteLine(s);\n"),
        "\xC3\xA9");
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_EQ(o.stdoutText, std::string("1\n\xC3\xA9\n"));
    expectNoLeak(o);
}

// A 4-byte code point (U+1F600) is one character, per-character output intact.
BEZEL_TEST(console_readtoend_utf8_9312, four_byte_code_point_is_one_char_7_4) {
    auto o = runWithStdin(
        mainWith("    var s := Console.In.ReadToEnd();\n"
                 "    Console.WriteLine(s.Length);\n"
                 "    var k := 0;\n"
                 "    while k < s.Length do Console.WriteLine(s[k]); k := k + 1; end;\n"),
        "a\xF0\x9F\x98\x80" "b\n");
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_EQ(o.stdoutText,
                    std::string("4\na\n\xF0\x9F\x98\x80\nb\n\n\n"));
    expectNoLeak(o);
}

// A large input: 50000 `é` is 100 KB and 50000 characters.
BEZEL_TEST(console_readtoend_utf8_9312, large_input_is_decoded_whole_7_4) {
    std::string e;
    for (int k = 0; k < 50000; ++k) e += "\xC3\xA9";
    auto o = runWithStdin(
        mainWith("    var s := Console.In.ReadToEnd();\n"
                 "    Console.WriteLine(s.Length);\n"),
        e);
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_EQ(o.stdoutText, std::string("50000\n"));
    expectNoLeak(o);
}

// ReadLine then ReadToEnd on ONE stream: the line, then the rest AS READ.
BEZEL_TEST(console_readtoend_utf8_9312, read_line_then_read_to_end_interleave_7_4) {
    auto o = runWithStdin(
        mainWith("    var l := Console.In.ReadLine();\n"
                 "    var s := Console.In.ReadToEnd();\n"
                 "    if l <> null then Console.WriteLine(l.Length); end;\n"
                 "    Console.WriteLine(s.Length);\n"
                 "    Console.WriteLine(s);\n"),
        "\xC3\xA9" "a\n\xC3\xA9" "\xF0\x9F\x98\x80" "\n");
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_EQ(o.stdoutText,
                    std::string("2\n3\n\xC3\xA9\xF0\x9F\x98\x80\n\n"));
    expectNoLeak(o);
}

// The leak oracle: ReadToEnd mints one decoded string per call; a 250-pass
// loop over a fresh (exhausted-after-first) stream releases every one.
BEZEL_TEST(console_readtoend_utf8_9312, read_to_end_loop_does_not_leak_4_6) {
    std::string in;
    for (int k = 0; k < 20; ++k) in += "h\xC3\xA9llo\n";
    auto o = runWithStdin(
        "module P; import System;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var total := 0;\n"
        "    var i := 0;\n"
        "    while i < 250 do\n"
        "      var s := Console.In.ReadToEnd();\n"
        "      total := total + s.Length;\n"
        "      i := i + 1;\n"
        "    end;\n"
        "    Console.WriteLine(total);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n",
        in);
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_EQ(o.stdoutText, std::string("120\n"));
    expectNoLeak(o);
}
