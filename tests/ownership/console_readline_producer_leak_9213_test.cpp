// Regression for #9213 — the string `Console.ReadLine()` returns and a local
// BINDS is NEVER RELEASED (one LeakSanitizer leak of the read line per read).
//
// THE ISSUE (leak, stdlib §2.1 / §4.6 / §28.3 / §28.9, v0.359). The issue's
// own repro, verbatim:
//
//     module P; import System;
//     public class Program
//       public static method Main(): int
//         var s := Console.ReadLine();
//         if s <> null then
//           Console.WriteLine(s.Length);
//         end;
//         return 0;
//       end Main;
//     end Program;
//
// run with `printf 'hello\n' | ./a.out` under `bezelc --sanitize=address` and
// `ASAN_OPTIONS=detect_leaks=1:exitcode=99:abort_on_error=0`.
//
// MEASURED on the base (r30-c1 @ 2b3839abd), the issue's own program with
// "hello\n" on standard input:
//
//     stdout:        5
//     LeakSanitizer: 38 byte(s) leaked in 2 allocation(s)
//                    (a 32-byte `bz.string` header + a 6-byte data buffer)
//     exit status:   1  (99 with the issue's own ASAN_OPTIONS)
//
// and this suite's 200-iteration loop of the same shape (multi-line stdin):
//
//     7600 byte(s) leaked in 400 allocation(s)
//     (200 headers of 32 B + 200 data buffers of 6 B)
//
// THE GAP. stdlib §2.1 (v0.359) declares, on the built-in `Console`,
//
//     public static method ReadLine(): string?;     // null at EOF
//
// and the `ReadLine` lowering (`emitConsoleReadLine`, src/codegen/
// LlvmBackend_Calls.cpp) reads one line from standard input into a heap buffer
// and mints the result the way every fresh string production is minted: a
// `malloc`'d `bz.string` header whose rc is set to 1 (`initStringRc`) — the
// producer's reference. §4.6: "`string` is runtime-managed: its memory is
// reclaimed automatically"; §28.3's producers arm: "Fresh productions are
// owned"; §28.9: "the implementation reclaims the production". So the CALLER
// owns exactly ONE reference and must discharge it.
//
// The consumer-side classification that decides this is
// `Emitter::isPlusOneStringProducer` (`src/codegen/LlvmBackend_Setup.cpp`) —
// the ONE predicate every consumer asks ("did this expression hand over a +1
// string?"). It has arms for `Path.*` (#7865), `File.ReadAllText` (#8016),
// `Convert.ToString` (#7464), `string.Join` / `From` / `Repeat` (#2228),
// `Environment.Get` (#7950) and — since #9199 — the §7.4
// `TextReader.ReadToEnd()` — and NO arm for the §2.1 `Console.ReadLine()`.
// So every consumer treated the result as a BORROW and nothing ever discharged
// the producer's +1: a bound local was not even a release candidate, a call
// argument was never released at the end of the full expression, a discarded
// statement never gave the reference back, and a `return` of the line minted a
// second reference the caller never dropped.
//
// THE FIX. ONE arm in `isPlusOneStringProducer`, whose discriminator is the
// LOWERING'S OWN (`isBuiltinConsoleReadLine`,
// `src/codegen/LlvmBackend_Console.hpp`) — the one place the lowering and the
// consumer-side classification both read, so the two cannot drift (the same
// discipline `LlvmBackend_TextReader.hpp` states for the §7.4 reader's member).
// The discriminator admits the SYNTACTIC receiver `Console` (the built-in
// CLASS), and excludes a VALUE named `Console` (fix #8126, §2.7 rule 1,
// v0.355) and a USER class named `Console` — exactly as the lowering excludes
// them.
//
// THE NULL AT EOF. `ReadLine(): string?` returns NULL at end of input, and a
// NULL result mints NOTHING. The arm says only WHETHER the call can mint a
// fresh production, never whether THIS run produced a value, and the runtime's
// string release is null-safe — so the EOF case discharges nothing and
// crashes nothing. The `the_null_result_at_eof_is_not_released_2_1` row below
// pins that.
//
// THE RUNNER. `runAot` opens fd 0 on /dev/null, so a program that READS
// standard input sees EOF and cannot be observed through it. This runner
// compiles through the REAL Driver (the shipping `bezelc` AOT path — the same
// `Driver::compileFile` `runAot` drives), with the generated-code
// AddressSanitizer ON (the `runAotChecked` family's own memory net) and
// LeakSanitizer ON (`detect_leaks=1:exitcode=99:abort_on_error=0`), and runs
// the executable with fd 0 opened READ-ONLY on a file holding the row's
// standard input, capturing stdout and stderr. The same shape as the fd-0 trap
// of `tests/codegen/textreader_readtoend_8383_test.cpp` and of
// `tests/ownership/textreader_readtoend_string_member_9057_test.cpp`, with
// fd 0 carrying INPUT instead of catching OUTPUT.
//
// WHY 200 ITERATIONS. A one-shot leak can hide under the sanitizer's noise;
// every row below loops the read 200 times, and the stdin carries 200 lines so
// EVERY read mints a string. The base's leak is ~7.6 kB in 400 allocations and
// cannot be missed.

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
// leak oracle armed.
// ---------------------------------------------------------------------------

struct RunOutcome {
    bool compiled = false;
    int exitCode = -1;
    std::string stdoutText;
    std::string stderrText;
};

const std::filesystem::path& cr9213ScratchDir() {
    return wtScratchDir("wt-tr9213");
}

std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

RunOutcome runWithStdin(const std::string& source,
                        const std::string& stdinText) {
    auto dir = cr9213ScratchDir();
    std::string stem = unique_stem("bezel_tr9213");
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
    // The `runAotChecked` family's memory net: the generated code is
    // instrumented and LeakSanitizer checks it at exit. `abort_on_error=0` so
    // a sanitizer finding PRINTS a report and exits 99 rather than SIGABRT,
    // and `exitcode=99` so the finding is distinguishable from the program's
    // own exit status (the #4532 lesson).
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
// same shape as `expectNoLeak` in the neighbouring leak rows
// (`tests/ownership/textreader_readtoend_producer_leak_9199_test.cpp`).
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

// The standard input every row reads: 200 lines of "hello\n". Every one of
// the 200 reads gets a line (the newline is stripped by the read, so each line
// is the five characters "hello"), so EVERY read MINTS a string — the leak on
// the base is 200 headers + 200 data buffers, which no sanitizer noise can
// hide.
const int kIterations = 200;
const char* kLine = "hello\n";

std::string multiLineStdin() {
    std::string s;
    s.reserve(static_cast<std::size_t>(kIterations) * 6);
    for (int k = 0; k < kIterations; ++k) s += kLine;
    return s;
}

// A 200-iteration loop around one read. `readStmt` is the statement that
// reads; `bodyStmt` is what the loop does with what it read.
std::string loopProgram(const std::string& readStmt,
                        const std::string& bodyStmt) {
    return "module P;\n"
           "public class Program\n"
           "  public static method Main(): int\n"
           "    var total := 0;\n"
           "    var i := 0;\n"
           "    while i < 200 do\n"
           "      " + readStmt + "\n" +
           "      " + bodyStmt + "\n" +
           "      i := i + 1;\n"
           "    end;\n"
           "    Console.WriteLine(total);\n"
           "    return 0;\n"
           "  end Main;\n"
           "end Program;\n";
}

// The expected stdout of the loop rows: 200 reads of a five-character line.
const char* kExpectedTotal = "1000\n";

}  // namespace

// ===========================================================================
// THE FAIL-WITHOUT BLOCK — every row RED on the base, GREEN on the tip.
// ===========================================================================

// THE ISSUE'S OWN PROGRAM, looped 200 times. The INFERRED local
// (`var s := Console.ReadLine();`) is the spelling the issue names. Base: 200
// leaked read lines (the 32-byte header + the 6-byte buffer each), so
// LeakSanitizer exits 99 and the row fails.
BEZEL_TEST(console_readline_producer_leak_9213,
           the_bound_local_releases_the_read_line_4_6) {
    auto o = runWithStdin(
        loopProgram("var s := Console.ReadLine();",
                    "if s <> null then total := total + s.Length; end;"),
        multiLineStdin());
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_EQ(o.stdoutText, std::string(kExpectedTotal));
    expectNoLeak(o);
}

// The TYPED spelling: `var s: string? := Console.ReadLine();` — the §2.1
// declared result type is `string?`, so the binding says it out loud. Same
// read, same leak on the base, same release on the tip.
BEZEL_TEST(console_readline_producer_leak_9213,
           the_typed_local_binding_releases_the_read_line_2_1) {
    auto o = runWithStdin(
        loopProgram("var s: string? := Console.ReadLine();",
                    "if s <> null then total := total + s.Length; end;"),
        multiLineStdin());
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_EQ(o.stdoutText, std::string(kExpectedTotal));
    expectNoLeak(o);
}

// PASSED AS AN ARGUMENT: §28.3's unbound owned temporary is disposed at the
// end of the full expression, so the read line the argument position produced
// must be given back there. The callee takes it as a BORROW (a plain
// parameter), so it holds no reference of its own to release. Base: one leaked
// line per call.
BEZEL_TEST(console_readline_producer_leak_9213,
           the_passed_as_argument_releases_the_read_line_2_1) {
    const char* src =
        "module P;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var total := 0;\n"
        "    var i := 0;\n"
        "    while i < 200 do\n"
        "      total := total + Take(Console.ReadLine());\n"
        "      i := i + 1;\n"
        "    end;\n"
        "    Console.WriteLine(total);\n"
        "    return 0;\n"
        "  end Main;\n"
        "  public static method Take(x: string?): int\n"
        "    if x <> null then\n"
        "      return x.Length;\n"
        "    end;\n"
        "    return 0;\n"
        "  end Take;\n"
        "end Program;\n";
    auto o = runWithStdin(src, multiLineStdin());
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_EQ(o.stdoutText, std::string(kExpectedTotal));
    expectNoLeak(o);
}

// RETURNED: a method whose declared return type is `string?` mints exactly
// +1 for the returned value (§28.3's RETURN-escape row), so the callee must
// TRANSFER the read line's single reference rather than mint a second one.
// Base: the callee retained the line on top of the producer's +1 and the
// caller's bound local released only its own reference — one unreleased
// reference per read.
BEZEL_TEST(console_readline_producer_leak_9213,
           the_returned_read_line_releases_the_line_28_3) {
    const char* src =
        "module P;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var total := 0;\n"
        "    var i := 0;\n"
        "    while i < 200 do\n"
        "      var s := ReadOne();\n"
        "      if s <> null then\n"
        "        total := total + s.Length;\n"
        "      end;\n"
        "      i := i + 1;\n"
        "    end;\n"
        "    Console.WriteLine(total);\n"
        "    return 0;\n"
        "  end Main;\n"
        "  public static method ReadOne(): string?\n"
        "    return Console.ReadLine();\n"
        "  end ReadOne;\n"
        "end Program;\n";
    auto o = runWithStdin(src, multiLineStdin());
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_EQ(o.stdoutText, std::string(kExpectedTotal));
    expectNoLeak(o);
}

// THE DISCARDED statement: §21.9 permits an expression statement of a
// non-`()` type ("the result is silently discarded"), and §28.9 leaves the
// string's lifetime to the runtime "without user involvement" — so the
// statement must GIVE THE REFERENCE BACK. Base: one leaked read line per
// statement.
BEZEL_TEST(console_readline_producer_leak_9213,
           the_discarded_readline_statement_releases_the_read_line_21_9) {
    auto o = runWithStdin(
        loopProgram("Console.ReadLine();", "total := total + 1;"),
        multiLineStdin());
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_EQ(o.stdoutText, std::string("200\n"));
    expectNoLeak(o);
}

// ===========================================================================
// THE CONTROL BLOCK — GREEN on the base AND on the tip. These pin the two
// shapes the fix must NOT have moved.
// ===========================================================================

// THE NULL AT EOF. `ReadLine(): string?` returns NULL at end of input, and a
// NULL result mints NOTHING — so there is nothing to release and nothing to
// crash on. The row pins that the fix's release of a `Console.ReadLine()`
// result is null-safe: it reads past the end of the (empty) input, prints
// "null", and leaks nothing — on the base and on the tip.
BEZEL_TEST(console_readline_producer_leak_9213,
           the_null_result_at_eof_is_not_released_2_1) {
    const char* src =
        "module P;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var s := Console.ReadLine();\n"
        "    if s = null then\n"
        "      Console.WriteLine(\"null\");\n"
        "    else\n"
        "      Console.WriteLine(s.Length);\n"
        "    end;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n";
    // EMPTY standard input: the very first read is already at EOF.
    auto o = runWithStdin(src, std::string());
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_EQ(o.stdoutText, std::string("null\n"));
    expectNoLeak(o);
}

// THE `Console.In.ReadLine()` TWIN the issue asks about. `TextReader.ReadLine()`
// (stdlib §7.4's other member) has NO lowering arm at all — the gap the
// #8383 lane deliberately did not widen — so `Console.In.ReadLine()` degrades
// to a zero-initialised slot behind #2434's warning and reads NOTHING. It
// therefore mints no string and leaks nothing, on the base and on the tip.
// The row pins THAT (the leak property this issue is about), not the degraded
// output: a future fix that gives `ReadLine()` a lowering must not make it
// leak.
BEZEL_TEST(console_readline_producer_leak_9213,
           the_console_in_readline_twin_leaks_nothing_7_4) {
    auto o = runWithStdin(
        loopProgram("var l := Console.In.ReadLine();",
                    "total := total + 1;"),
        multiLineStdin());
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_EQ(o.stdoutText, std::string("200\n"));
    expectNoLeak(o);
}
