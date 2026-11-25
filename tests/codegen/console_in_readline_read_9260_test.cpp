// Regression for #9260 — `Console.In.ReadLine()` / `Console.In.Read()` had NO
// lowering arm: they degraded to a ZERO-INITIALISED slot and read NOTHING.
//
// THE ISSUE (silent wrong code, stdlib §7.4 / §2.1, v0.359). stdlib §7.4
// (v0.359) declares, on the abstract `TextReader`,
//
//     public abstract method Read(): int;         // next char as int, or -1
//                                                  //   at EOF
//     public abstract method ReadLine(): string?; // null at EOF
//
// and stdlib §2.1 (v0.359) declares `Console.In` as that class's ONE
// instance ("`TextReader` has no concrete subclass in this library: its one
// instance is `Console.In`"). §7.4's THIRD member, `ReadToEnd()`, got its
// lowering arm in #8383; these TWO did not.
//
// MEASURED on the base (r30-c1 @ 009b0c5de), the issue's own program,
//
//     var l := Console.In.ReadLine();
//     if l <> null then Console.WriteLine($"[{l}]"); else Console.WriteLine("null"); end;
//
// with `printf 'hello\nworld\n' | ./a.out` printed `[0]` and exited 0 — the
// line of standard input was never read — and the sibling member
// `var n := Console.In.Read();` printed `0` where §7.4 says 104 (the code
// point of 'h'). BOTH compiles also carried the `internal:` degrade note
// ("lowerLocalDecl's initializer expression failed to lower to a value for
// this local's slot (degraded to a zero-initialized value) — #2434").
//
// THE FIX (this lane), in the three places the sibling `ReadToEnd()` needed
// them (#9057's semantic arm, #8383's lowering arm, #9199's producer arm):
//
//   * sema types the two calls (`string?` / `int`) — the #9057 arm's twin,
//     so the local they initialise INFERs a type and the `l <> null`
//     narrowing has a nullable type to narrow;
//   * codegen lowers them — `ReadLine()` through §2.1's own reader
//     (`emitConsoleReadLine`: the same `fgets`-on-`stdin` shape, the same
//     trailing-'\n' strip, the same NULL at EOF), `Read()` through
//     `getchar()` (the next code point, -1 at EOF);
//   * `Emitter::isPlusOneStringProducer` recognises the minted line as a +1
//     string PRODUCTION, so the caller discharges the producer's reference
//     (the #9199 arm's twin — the #9213 arm's §2.1 sibling).
//
// THE RECEIVER. stdlib §2.1 makes `Console.In` the class's ONE instance, so
// the TYPED spelling (`var r: TextReader := Console.In; r.ReadLine();`) is
// the SAME read of standard input and is lowered by the SAME arm — the
// spelling #8383's arm serves for `ReadToEnd()`. On the base that spelling
// was REFUSED with a clear diagnostic ("`ReadLine` on built-in type
// `TextReader` produced no value … #2457") while the syntactic
// `Console.In` spelling was ACCEPTED and silently degraded — the two
// spellings of ONE member disagreeing, which is what this fix removes.
//
// THE ROWS. The first block is the FAIL-WITHOUT block: every row in it is
// RED on the base and GREEN on the tip. The second block is the CONTROL
// block: the rows there pin behaviour that must NOT move (they are GREEN on
// both arms), and each is RED under a plausible wrong fix named in the
// commit body.
//
// THE RUNNER. `runAot` opens fd 0 on /dev/null, so a program that READS
// standard input sees EOF and cannot be observed through it; this runner
// compiles through the REAL Driver (the shipping `bezelc` AOT path — the
// same `Driver::compileFile` `runAot` drives), with the generated-code
// AddressSanitizer ON and LeakSanitizer ON, and runs the executable with
// fd 0 opened READ-ONLY on a file holding the row's standard input, capturing
// stdout and stderr. The same harness the #9213 rows use.

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

const std::filesystem::path& tr9260ScratchDir() {
    return wtScratchDir("wt-tr9260");
}

std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

RunOutcome runWithStdin(const std::string& source,
                        const std::string& stdinText) {
    auto dir = tr9260ScratchDir();
    std::string stem = unique_stem("bezel_tr9260");
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

// The standard input the 200-iteration rows read: 200 lines of "hello\n".
// Every one of the 200 reads gets a line (the newline is stripped by the
// read, so each line is the five characters "hello"), so EVERY read MINTS a
// string — a one-shot leak could hide under the sanitizer's noise, this one
// cannot.
const int kIterations = 200;
const char* kLine = "hello\n";

std::string multiLineStdin() {
    std::string s;
    s.reserve(static_cast<std::size_t>(kIterations) * 6);
    for (int k = 0; k < kIterations; ++k) s += kLine;
    return s;
}

// A 200-iteration loop around one read. `readStmt` is the statement that
// reads; `bodyStmt` is what the loop does with what it read. The shape the
// #9213 rows use.
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

// The issue's own repro, verbatim (§2.1 of the issue).
const char* kRepro =
    "module P; import System;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    var l := Console.In.ReadLine();\n"
    "    if l <> null then\n"
    "      Console.WriteLine($\"[{l}]\");\n"
    "    else\n"
    "      Console.WriteLine(\"null\");\n"
    "    end;\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// The issue's sibling member, verbatim.
const char* kReadRepro =
    "module P; import System;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    var n := Console.In.Read();\n"
    "    Console.WriteLine(n);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

}  // namespace

// ===========================================================================
// THE FAIL-WITHOUT BLOCK — every row RED on the base, GREEN on the tip.
// ===========================================================================

// THE ISSUE'S OWN PROGRAM, verbatim. §7.4's `ReadLine()` returns the line
// WITHOUT its terminator, so the line is "hello" — and the program prints
// "[hello]". Base: "[0]" (the degraded slot, the zero-initialised value) —
// the standard input was never read.
BEZEL_TEST(console_in_readline_read_9260, the_in_readline_reads_the_line_7_4) {
    auto o = runWithStdin(kRepro, "hello\nworld\n");
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_EQ(o.stdoutText, std::string("[hello]\n"));
    expectNoLeak(o);
}

// THE SIBLING MEMBER. §7.4's `Read(): int` is "next char as int, or -1 at
// EOF" — the code point of 'h' is 104. Base: "0".
BEZEL_TEST(console_in_readline_read_9260,
           the_in_read_returns_the_next_code_point_7_4) {
    auto o = runWithStdin(kReadRepro, "hello\n");
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_EQ(o.stdoutText, std::string("104\n"));
    expectNoLeak(o);
}

// §7.4's `ReadLine(): string?` is NULL at the end of the input — the
// "null at EOF" the standard library writes beside the declaration. Base:
// "[0]" (the degraded slot is 0, not null — the call read nothing, so it
// could not answer "null" either).
BEZEL_TEST(console_in_readline_read_9260, the_in_readline_is_null_at_eof_7_4) {
    auto o = runWithStdin(kRepro, "");
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_EQ(o.stdoutText, std::string("null\n"));
    expectNoLeak(o);
}

// §7.4's `Read(): int` is -1 at the end of the input ("next char as int, or
// -1 at EOF"). Base: "0".
BEZEL_TEST(console_in_readline_read_9260, the_in_read_is_minus_one_at_eof_7_4) {
    auto o = runWithStdin(kReadRepro, "");
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_EQ(o.stdoutText, std::string("-1\n"));
    expectNoLeak(o);
}

// THE LOCAL IS TYPED `string?` — the #9057 story: an UNTYPED initializer
// infers nothing for the local it binds (§21.1), so the §7.4 `string?` the
// member declares never reached the `l <> null` narrowing, and the §9.6
// string member on the narrowed local had no `string` receiver to dispatch
// on. The row reads the LENGTH of the line the reader returned. Base: the
// degraded slot's length is not the line's, and the `Console.WriteLine` of
// it emits nothing (#3079's note) — the base printed nothing at all.
BEZEL_TEST(console_in_readline_read_9260,
           the_in_readline_local_is_a_narrowable_string_7_4) {
    const char* src =
        "module P;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var l := Console.In.ReadLine();\n"
        "    if l <> null then\n"
        "      Console.WriteLine(l.Length);\n"
        "    end;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n";
    auto o = runWithStdin(src, "hello\nworld\n");
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_EQ(o.stdoutText, std::string("5\n"));
    expectNoLeak(o);
}

// THE TYPED SPELLING — stdlib §2.1's `Console.In` is the class's ONE
// instance, so `r.ReadLine()` on a value of the built-in `TextReader` type
// is the SAME read of standard input, through the SAME arm. Base: a compile
// REJECT ("`ReadLine` on built-in type `TextReader` produced no value, and
// no dispatch arm emitted any code for this call … #2457") — the two
// spellings of ONE member disagreeing, the syntactic one accepted and
// silently degraded, the typed one refused.
BEZEL_TEST(console_in_readline_read_9260,
           the_in_readline_at_a_textreader_local_7_4) {
    const char* src =
        "module P;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var r: TextReader := Console.In;\n"
        "    var l := r.ReadLine();\n"
        "    if l <> null then\n"
        "      Console.WriteLine($\"[{l}]\");\n"
        "    else\n"
        "      Console.WriteLine(\"null\");\n"
        "    end;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n";
    auto o = runWithStdin(src, "hello\nworld\n");
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_EQ(o.stdoutText, std::string("[hello]\n"));
    expectNoLeak(o);
}

// THE TYPED SPELLING of the sibling member: `r.Read()` is the next code
// point, 104. Base: the same compile REJECT.
BEZEL_TEST(console_in_readline_read_9260,
           the_in_read_at_a_textreader_local_7_4) {
    const char* src =
        "module P;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var r: TextReader := Console.In;\n"
        "    var n := r.Read();\n"
        "    Console.WriteLine(n);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n";
    auto o = runWithStdin(src, "hello\n");
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_EQ(o.stdoutText, std::string("104\n"));
    expectNoLeak(o);
}

// 200 READS, 200 MINTED LINES, 200 RELEASES. The producer's +1 is the
// reader's own reference (§4.6 / §28.3 / §28.9), so every line the reader
// mints must be given back exactly once — the #9199 arm's twin for §7.4's
// `ReadLine()`. The stdout (1000 = 200 reads × 5 characters) is the
// fail-without half: on the base the loop read NOTHING, so the total was 0.
// The leak oracle is the other half: a fix that lowers the read but forgets
// the producer arm leaks 200 headers + 200 data buffers.
BEZEL_TEST(console_in_readline_read_9260,
           the_in_readline_loop_releases_every_line_4_6) {
    auto o = runWithStdin(
        loopProgram("var l := Console.In.ReadLine();",
                    "if l <> null then total := total + l.Length; end;"),
        multiLineStdin());
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_EQ(o.stdoutText, std::string("1000\n"));
    expectNoLeak(o);
}

// ===========================================================================
// THE CONTROL BLOCK — these must NOT move (GREEN on the base and on the tip).
// ===========================================================================

// A DISCARDED read still gives the line back: §28.3.11's *Unbound owned
// temporaries* rule — a value that is NOT bound, NOT moved, NOT returned and
// NOT transferred into an owning location "is disposed at the end of the full
// expression in which it is produced". The row is a boundary control a
// corpus case cannot express (the corpus runner has no leak oracle), and it
// is RED under a plausible wrong fix: one that lowers the read but leaves
// the `isPlusOneStringProducer` arm out — then the discarded statement
// leaks one line per read.
BEZEL_TEST(console_in_readline_read_9260,
           the_discarded_in_readline_statement_releases_the_line_4_6) {
    auto o = runWithStdin(
        loopProgram("Console.In.ReadLine();", "total := total + 1;"),
        multiLineStdin());
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_EQ(o.stdoutText, std::string("200\n"));
    expectNoLeak(o);
}

// stdlib §2.1's OWN `Console.ReadLine()` — the §2.1 sibling the issue names
// as ALREADY CORRECT — is unchanged: it still reads a line and still strips
// its terminator ('hello', not 'hello\n'), and it still releases the line it
// read (the #9213 arm). The row is a boundary control a corpus case cannot
// express (the leak oracle), and it is RED under a plausible wrong fix: one
// that routes the §7.4 `ReadLine()` through §2.1's reader by RENAMING it —
// then §2.1's own member would be the one that mints the §7.4 shape.
BEZEL_TEST(console_in_readline_read_9260,
           console_read_line_still_reads_a_line_2_1) {
    const char* src =
        "module P;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var s := Console.ReadLine();\n"
        "    if s <> null then\n"
        "      Console.WriteLine($\"[{s}]\");\n"
        "    else\n"
        "      Console.WriteLine(\"null\");\n"
        "    end;\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n";
    auto o = runWithStdin(src, "hello\nworld\n");
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_EQ(o.stdoutText, std::string("[hello]\n"));
    expectNoLeak(o);
}
