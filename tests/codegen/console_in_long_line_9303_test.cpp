// Regression for #9303 — `Console.In.ReadLine()` split a line longer than 4095
// bytes in two.
//
// THE ISSUE (stdlib §7.4 / §2.1, v0.359; follow-up of #9260). The built-in
// line reader read through an `fgets` buffer of 4096 bytes, so a 5000-char line
// came back as TWO lines (4095 + 905 chars), and a line of exactly 4095 bytes
// came back as itself plus a spurious EMPTY line (the buffer held the 4095
// bytes and the '\n' was read as a line of its own). stdlib §7.4
// `ReadLine(): string?` ("null at EOF") and §2.1 `Console.ReadLine` return the next LINE: a line is delimited by its
// terminator, not by an implementation buffer.
//
// THE FIX. ONE reader behind every spelling of a line read —
// `Console.ReadLine()`, `Console.In.ReadLine()` and the typed `TextReader`
// spelling all lower through `Emitter::emitConsoleReadLine`, which now calls the
// runtime's `bz_console_read_line`: it reads to the '\n' terminator whatever the
// length. CR handling is the open spec-question #9305 and is NOT touched.
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

const std::filesystem::path& tr9303ScratchDir() {
    return wtScratchDir("wt-tr9303");
}

std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

RunOutcome runWithStdin(const std::string& source,
                        const std::string& stdinText) {
    auto dir = tr9303ScratchDir();
    std::string stem = unique_stem("bezel_tr9303");
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

std::string xs(int n) { return std::string(static_cast<std::size_t>(n), 'x'); }

}  // namespace

// ===========================================================================
// #9303 — a line is delimited by its terminator, not by a buffer.
// ===========================================================================

// 4095 bytes + '\n' filled the old 4096-byte buffer exactly: the '\n' came back
// as a SECOND, empty line. Then a 4096-byte line, a 10000-byte line, a short
// line. Base: 4095, 0, 4095, 1, ... (the lines split at every 4095 bytes).
BEZEL_TEST(console_in_long_line_9303, long_lines_are_one_line_each_7_4) {
    auto o = runWithStdin(
        printLengths("Console.In.ReadLine()", 5),
        xs(4095) + "\n" + xs(4096) + "\n" + xs(10000) + "\n" + "y\n");
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_EQ(o.stdoutText,
                    std::string("4095\n4096\n10000\n1\nnull\n"));
    expectNoLeak(o);
}

// The issue's own repro: a 5000-char line is 5000, then null.
BEZEL_TEST(console_in_long_line_9303, a_5000_char_line_is_one_line_7_4) {
    auto o = runWithStdin(printLengths("Console.In.ReadLine()", 2),
                          xs(5000) + "\n");
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_EQ(o.stdoutText, std::string("5000\nnull\n"));
    expectNoLeak(o);
}

// §2.1's `Console.ReadLine()` is the same reader: one 10000-byte line, then a
// short one, and an unterminated long last line.
BEZEL_TEST(console_in_long_line_9303, console_read_line_long_lines_2_1) {
    auto o = runWithStdin(printLengths("Console.ReadLine()", 4),
                          xs(10000) + "\nz\n" + xs(4097));
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_EQ(o.stdoutText, std::string("10000\n1\n4097\nnull\n"));
    expectNoLeak(o);
}

