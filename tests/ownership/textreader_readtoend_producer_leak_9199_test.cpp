// Regression for #9199 — the string `Console.In.ReadToEnd()` returns and a
// local BINDS is NEVER RELEASED (one LeakSanitizer leak of the read text per
// read).
//
// THE ISSUE (leak, stdlib §7.4 / §4.6 / §28.3 / §28.9, v0.359). The issue's
// own repro, verbatim:
//
//     module P; import System;
//     public class Program
//       public static method Main(): int
//         var s: string := Console.In.ReadToEnd();
//         Console.WriteLine(s.Length);
//         return 0;
//       end Main;
//     end Program;
//
// run with `printf 'hello\n' | ./a.out` under `bezelc --sanitize=address` and
// `ASAN_OPTIONS=detect_leaks=1`. The issue's own measurement:
//
//   Observed: prints `6`. LeakSanitizer reports one allocation, the size of
//             the input.
//   Expected: prints `6` with no leak.
//
// MEASURED on the base (r30-c1 @ 623aaa9f2), the issue's own program with
// "hello\n" on standard input:
//
//     stdout:        6
//     LeakSanitizer: 39 byte(s) leaked in 2 allocation(s)
//                    (a 32-byte `bz.string` header + a 7-byte data buffer)
//     exit status:   1
//
// and a 200-iteration loop of the same shape:
//
//     6606 byte(s) leaked in 400 allocation(s)
//
// (the first read takes the whole 6-byte input; the other 199 read the
// remaining EOF and produce the empty text).
//
// THE GAP. stdlib §7.4 (v0.359) declares, on the abstract `TextReader`,
//
//     public method ReadToEnd(): string;
//
// and stdlib §2.1 (v0.359) declares `Console.In` as that class's ONE instance
// ("`TextReader` has no concrete subclass in this library: its one instance is
// `Console.In`"). The `ReadToEnd` lowering (`emitTextReaderReadToEnd`,
// `src/codegen/LlvmBackend_Calls2.cpp`) mints the result the way every fresh
// `string` production is minted: a `malloc`'d `bz.string` header whose rc is
// set to 1 (`initStringRc`) — the producer's reference. §4.6: "`string` is
// runtime-managed: its memory is reclaimed automatically"; §28.9: "the type's
// lifetime is managed by the runtime without user involvement"; §28.3's
// producers arm: "Fresh productions are owned". So the CALLER owns exactly
// ONE reference and must discharge it.
//
// The consumer-side classification that decides this is
// `Emitter::isPlusOneStringProducer` (`src/codegen/LlvmBackend_Setup.cpp`) —
// the ONE predicate every consumer asks ("did this expression hand over a +1
// string?"). It has arms for `Path.*` (#7865), `File.ReadAllText` (#8016),
// `Convert.ToString` (#7464), `string.Join` / `From` / `Repeat` (#2228),
// `Environment.Get` (#7950) — and NO arm for the §7.4 `TextReader.ReadToEnd()`.
// So every consumer treated the result as a BORROW and nothing ever discharged
// the producer's +1: a bound local was not even a release candidate, a call
// argument was never released at the end of the full expression, and a
// discarded statement never gave the reference back.
//
// THE FIX. ONE arm in `isPlusOneStringProducer`, whose discriminator is the
// LOWERING'S OWN (`classifyTextReaderReadToEnd`,
// `src/codegen/LlvmBackend_TextReader.hpp`) — the one place the lowering and
// the consumer-side classification both read, so the two cannot drift. It
// admits BOTH receivers §7.4 admits: the SYNTACTIC `Console.In` and a VALUE
// of the built-in `TextReader` type. A USER's own `ReadToEnd` (its callee
// resolves to a MethodSymbol) and a USER class named `TextReader` are both
// excluded, exactly as the lowering excludes them.
//
// THE RUNNER. `runAot` opens fd 0 on /dev/null, so a program that READS
// standard input sees EOF and cannot be observed through it. This runner
// compiles through the REAL Driver (the shipping `bezelc` AOT path — the same
// `Driver::compileFile` `runAot` drives), with the generated-code
// AddressSanitizer ON (the `runAotChecked` family's own memory net) and
// LeakSanitizer ON (`detect_leaks=1:exitcode=99:abort_on_error=0`), and runs
// the executable with fd 0 opened READ-ONLY on a file holding the row's
// standard input, capturing stdout and stderr. The same shape as the fd-0
// trap of `tests/codegen/textreader_readtoend_8383_test.cpp` and of
// `tests/ownership/textreader_readtoend_string_member_9057_test.cpp`, with
// fd 0 carrying INPUT instead of catching OUTPUT.
//
// WHY 200 ITERATIONS. A one-shot leak can hide under the sanitizer's noise;
// every row below loops the read 200 times, so the base's leak is ~6.6 kB in
// 400 allocations and cannot be missed.

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

const std::filesystem::path& rte9199ScratchDir() {
    return wtScratchDir("wt-tr9199");
}

std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

RunOutcome runWithStdin(const std::string& source,
                        const std::string& stdinText) {
    auto dir = rte9199ScratchDir();
    std::string stem = unique_stem("bezel_tr9199");
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
// same shape as `assertNoLeak` in the neighbouring leak rows
// (`tests/codegen/closure_local_call_init_returned_leak_8925_test.cpp`).
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

// The standard input every row reads: "hello\n" — six bytes, the newline
// INCLUDED, because §7.4's `ReadToEnd` returns the text AS READ (the same
// fact the #8383 rows pin).
const char* kStdin = "hello\n";

// A 200-iteration loop around one read. `readStmt` is the statement that
// reads; `bodyStmt` is what the loop does with what it read.
std::string loopProgram(const std::string& readStmt, const std::string& bodyStmt) {
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

}  // namespace

// ===========================================================================
// THE FAIL-WITHOUT BLOCK — every row RED on the base, GREEN on the tip.
// ===========================================================================

// THE ISSUE'S OWN PROGRAM, looped 200 times. The typed local
// (`var s: string := Console.In.ReadToEnd();`) is the spelling the issue
// names first. Base: 200 leaked read texts (the 32-byte header + the 7-byte
// buffer each), so LeakSanitizer exits 99 and the row fails.
BEZEL_TEST(textreader_readtoend_producer_leak_9199,
           the_typed_local_binding_releases_the_read_text_4_6) {
    auto o = runWithStdin(
        loopProgram("var s: string := Console.In.ReadToEnd();",
                    "total := total + s.Length;"),
        kStdin);
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    // The FIRST read takes the whole input ("hello\n" — six bytes, the
    // newline included); the other 199 read the remaining EOF.
    BEZEL_EXPECT_EQ(o.stdoutText, std::string("6\n"));
    expectNoLeak(o);
}

// The INFERRED spelling the issue names second (`var s :=`). Same read, same
// leak on the base, same release on the tip.
BEZEL_TEST(textreader_readtoend_producer_leak_9199,
           the_inferred_local_binding_releases_the_read_text_21_1) {
    auto o = runWithStdin(
        loopProgram("var s := Console.In.ReadToEnd();",
                    "total := total + s.Length;"),
        kStdin);
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_EQ(o.stdoutText, std::string("6\n"));
    expectNoLeak(o);
}

// The DISCARDED statement: §21.9 permits an expression statement of a
// non-`()` type ("the result is silently discarded"), and §28.9 leaves the
// string's lifetime to the runtime "without user involvement" — so the
// statement must GIVE THE REFERENCE BACK. Base: one leaked read text per
// statement.
BEZEL_TEST(textreader_readtoend_producer_leak_9199,
           the_discarded_readtoend_statement_releases_the_read_text_21_9) {
    auto o = runWithStdin(
        loopProgram("Console.In.ReadToEnd();", "total := total + 1;"),
        kStdin);
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_EQ(o.stdoutText, std::string("200\n"));
    expectNoLeak(o);
}

// The TYPED-RECEIVER spelling of the same member: `Console.In` is the class's
// ONE instance (stdlib §2.1), so `r.ReadToEnd()` on a value of the built-in
// `TextReader` type is the same read, through the same lowering arm. Base:
// the same leak.
BEZEL_TEST(textreader_readtoend_producer_leak_9199,
           the_textreader_typed_receiver_releases_the_read_text_7_4) {
    auto o = runWithStdin(
        loopProgram("var r: TextReader := Console.In;\n"
                    "      var s := r.ReadToEnd();",
                    "total := total + s.Length;"),
        kStdin);
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_EQ(o.stdoutText, std::string("6\n"));
    expectNoLeak(o);
}

// ===========================================================================
// THE CONTROL BLOCK — GREEN on the base AND on the tip. These pin the two
// shapes the fix must NOT have moved.
// ===========================================================================

// THE `ReadLine()` TWIN the issue asks about. `TextReader.ReadLine()`
// (stdlib §7.4's other member) had NO lowering arm at all — the gap the
// #8383 lane deliberately did not widen — so `Console.In.ReadLine()` degraded
// to a zero-initialised slot behind #2434's warning and read NOTHING. It
// therefore minted no string and leaked nothing, on the base and on the tip.
// The row pins THAT (the leak property this issue is about), not the
// degraded output: a future fix that gives `ReadLine()` a lowering must not
// make it leak.
//
// fix #9260 (stdlib §7.4, v0.359) IS that future fix: §7.4's `ReadLine()`
// now HAS a lowering arm, so every one of the 200 reads MINTS a line — and
// the row's expectation is UNCHANGED (the stdout is still "200" and the leak
// oracle still says zero), which is exactly what the row was written to pin:
// the lowering must not make the read line leak.
BEZEL_TEST(textreader_readtoend_producer_leak_9199,
           the_in_readline_twin_leaks_nothing_7_4) {
    auto o = runWithStdin(
        loopProgram("var l := Console.In.ReadLine();",
                    "total := total + 1;"),
        kStdin);
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    expectNoLeak(o);
}

// A USER CLASS's own `ReadToEnd` is the USER's member (stdlib §1's
// Extensibility), not §7.4's built-in one. The fix's arm must NOT claim it:
// the user's method owns its own result's lifetime. The row pins that the
// user's member still runs (its own line is printed), still returns its own
// text, and still leaks nothing.
BEZEL_TEST(textreader_readtoend_producer_leak_9199,
           a_user_read_to_end_is_the_users_own_member_7_4) {
    const char* src =
        "module M;\n"
        "public class R\n"
        "  public method ReadToEnd(): string\n"
        "    Console.WriteLine(\"user\");\n"
        "    return \"user-text\";\n"
        "  end ReadToEnd;\n"
        "end R;\n"
        "public class P\n"
        "  public static method Main(): int\n"
        "    var total := 0;\n"
        "    var i := 0;\n"
        "    while i < 200 do\n"
        "      var r := new R();\n"
        "      var s := r.ReadToEnd();\n"
        "      total := total + s.Length;\n"
        "      i := i + 1;\n"
        "    end;\n"
        "    Console.WriteLine(total);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end P;\n";
    auto o = runWithStdin(src, kStdin);
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    // 200 iterations × "user-text" (9 characters) — the user's member ran 200
    // times, and the standard input was NOT read behind it.
    std::string expected;
    for (int k = 0; k < 200; ++k) expected += "user\n";
    expected += "1800\n";
    BEZEL_EXPECT_EQ(o.stdoutText, expected);
    expectNoLeak(o);
}
