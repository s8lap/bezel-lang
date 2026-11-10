// Regression for #8383 — `Console.In.ReadToEnd()` LOWERED TO NOTHING.
//
// per stdlib §7.4 (v0.355) the abstract `TextReader` declares
//
//     public method ReadToEnd(): string;
//
// and per stdlib §2.1 (v0.355) `Console.In` is that class's ONE instance
// ("`TextReader` has no concrete subclass in this library: its one instance is
// `Console.In`"). §7.4's `ReadToEnd` returns "the text as read" — the bytes
// of standard input INCLUDING their newlines.
//
// THE GAP. The member was DOCUMENTED but had NO lowering arm at all: the
// compiler accepted the call, sema left it untyped (the built-in `TextReader`
// carries no member surface there — `Console.In` is left untyped outside a
// call-argument position, ExprTypecheck_Exprs.cpp's `consoleStreamArgumentType`
// comment), and codegen emitted NOTHING for it. The local it initialised was
// zero-initialised behind #2434's warning, the later `Console.WriteLine` of it
// emitted nothing behind #3079's warning, and the program exited 0 having
// printed NOTHING — the silent-wrong-code shape (spec §21.9's wording is the
// SYMPTOM, not the defect).
//
// MEASURED on this tree's base (the issue's own repro, verbatim):
//
//     var text := Console.In.ReadToEnd();
//     Console.WriteLine(text.Length);
//
// with `printf 'hello\nworld\n' | ./a.out` printed nothing and exited 0; with
// an EXPLICIT `var text: string := ...` it SEGFAULTED (the null slot is
// dereferenced by `text.Length`).
//
// THE FIX (this lane). A `fgets`-on-`stdin` loop that accumulates the bytes
// INCLUDING their newlines into one heap buffer, built into the same
// {data, len, width=1, rc=1} string shape `emitConsoleReadLine` builds — so
// `ReadToEnd` and `ReadLine` agree on what a byte of standard input is. NOT a
// loop of `ReadLine()` calls: `emitConsoleReadLine` STRIPS the trailing '\n'
// (stdlib §2.1's `Console.ReadLine` returns the line without its terminator),
// so a `ReadLine` loop would answer 10 for "hello\nworld\n" where §7.4's
// `ReadToEnd` answers 12.
//
// THE ROWS. The first block is the FAIL-WITHOUT block: every row in it is RED
// on the base and GREEN on the tip (the base arm is built with THIS file in
// place — the "fail-without" arm). The second block is the CONTROL block: the
// rows there pin behaviour that must NOT move (they are GREEN on both arms).
//
// THE CONTROL BLOCK'S `Console.In.ReadLine()` ROW is a "must not move" pin on
// a KNOWN, SEPARATE, PRE-EXISTING gap: `TextReader.ReadLine()` (stdlib §7.4's
// other member) has no lowering arm either, so `Console.In.ReadLine()` still
// degrades to a zero-initialised slot behind #2434's warning on BOTH arms. It
// is NOT #8383's member and is deliberately NOT widened into this lane; the row
// exists to prove this lane's `ReadToEnd` arm did not change it, and the gap
// itself is reported in the lane handoff.

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
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
#include "support/force_sanitize.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

namespace {

// ---------------------------------------------------------------------------
// The runner. `runAot` opens fd 0 on /dev/null, so a program that READS
// standard input sees EOF and cannot be observed; this runner compiles
// through the REAL Driver (the shipping `bezelc` AOT path — the same
// `Driver::compileFile` `runAot` drives) and runs the executable with fd 0
// opened READ-ONLY on a file holding the row's standard input, capturing
// stdout and stderr. The same shape as the fd-0 trap of
// tests/codegen/textwriter_value_7792_test.cpp, with fd 0 carrying INPUT
// instead of catching OUTPUT.
// ---------------------------------------------------------------------------

struct RunOutcome {
    bool compiled = false;
    int exitCode = -1;
    std::string stdoutText;
    std::string stderrText;
};

const std::filesystem::path& rteScratchDir() {
    return wtScratchDir("wt-tr8383");
}

std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

RunOutcome runWithStdin(const std::string& source,
                        const std::string& stdinText) {
    auto dir = rteScratchDir();
    std::string stem = unique_stem("bezel_tr8383");
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
    // The ASan sweep's one bit (support/force_sanitize.hpp) — the generated
    // code is instrumented and the run reports a fired sanitizer as a
    // non-zero exit, exactly as `runAot` does.
    opts.sanitizeAddress = bezel::test::forceSanitizeAddress();
    bezel::CompileResult r = driver.compileFile(src, exe, /*deps=*/{}, opts);
    o.compiled = r.success;
    if (r.success) {
        pid_t pid = ::fork();
        if (pid == 0) {
            if (opts.sanitizeAddress)
                ::setenv("ASAN_OPTIONS",
                         "detect_leaks=0:abort_on_error=0:exitcode=97", 1);
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

// Every diagnostic message of an in-process front-end + codegen run. `runAot`
// drops the diagnostics of a SUCCESSFUL compile, so a Warning's absence can
// only be observed here (the pattern of
// tests/codegen/generic_template_local_degrade_7257_test.cpp and of
// tests/codegen/textwriter_value_7792_test.cpp).
std::vector<std::string> compileMessages(const std::string& src,
                                          bool& rejected) {
    std::vector<std::string> out;
    bezel::DiagnosticEngine diags;
    bezel::Lexer lexer(src, diags);
    bezel::Parser parser(lexer, diags);
    auto unit = parser.parseCompilationUnit();
    if (unit) {
        bezel::sema::SemaContext ctx(diags);
        bezel::sema::collectSymbols(*unit, ctx);
        bezel::sema::resolveTypes(*unit, ctx);
        bezel::sema::typecheckExpressions(*unit, ctx);
        if (!diags.hasErrors()) {
            auto llvmCtx = std::make_unique<llvm::LLVMContext>();
            (void)bezel::codegen::emitModule(*unit, ctx, *llvmCtx,
                                             bezel::codegen::CodegenOptions{});
        }
    }
    rejected = !unit || diags.hasErrors();
    for (const auto& d : diags.diagnostics()) out.push_back(d.message);
    return out;
}

// The issue's repro, verbatim (§2.1 of the issue).
const char* kRepro =
    "module P;\n"
    "public class Program\n"
    "  public static method Main()\n"
    "    var text := Console.In.ReadToEnd();\n"
    "    Console.WriteLine(text.Length);\n"
    "  end Main;\n"
    "end Program;\n";

}  // namespace

// ===========================================================================
// THE FAIL-WITHOUT BLOCK — every row RED on the base, GREEN on the tip.
// ===========================================================================

// The issue's own repro, with the issue's own expectation: 12 — the length
// of "hello\nworld\n", the NEWLINES INCLUDED (§7.4 returns the text as read).
// Base: nothing printed, exit 0.
BEZEL_TEST(textreader_readtoend_8383, read_to_end_length_includes_the_newlines_7_4) {
    auto o = runWithStdin(kRepro, "hello\nworld\n");
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_EQ(o.stdoutText, std::string("12\n"));
}

// The text itself, not just its length: §7.4's `ReadToEnd` returns the bytes
// AS READ, so the two newlines are part of the value. Base: nothing printed.
BEZEL_TEST(textreader_readtoend_8383, read_to_end_returns_the_text_as_read_7_4) {
    const char* src =
        "module P;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    var text := Console.In.ReadToEnd();\n"
        "    Console.WriteLine(text);\n"
        "  end Main;\n"
        "end Program;\n";
    auto o = runWithStdin(src, "hello\nworld\n");
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_EQ(o.stdoutText, std::string("hello\nworld\n\n"));
}

// Standard input at EOF: the empty text, length 0. Base: nothing printed.
BEZEL_TEST(textreader_readtoend_8383, read_to_end_of_empty_stdin_is_empty_7_4) {
    auto o = runWithStdin(kRepro, "");
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_EQ(o.stdoutText, std::string("0\n"));
}

// Input with no trailing newline: the three bytes are still read. Base:
// nothing printed.
BEZEL_TEST(textreader_readtoend_8383, read_to_end_without_a_trailing_newline_7_4) {
    auto o = runWithStdin(kRepro, "abc");
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_EQ(o.stdoutText, std::string("3\n"));
}

// Three lines: 6 bytes (three characters and three newlines). Base: nothing
// printed.
BEZEL_TEST(textreader_readtoend_8383, read_to_end_of_three_lines_7_4) {
    auto o = runWithStdin(kRepro, "a\nb\nc\n");
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_EQ(o.stdoutText, std::string("6\n"));
}

// Two calls: the first reads standard input to EOF, the second reads the
// REST of it (nothing) — each call is its own read-to-EOF. Base: nothing
// printed.
BEZEL_TEST(textreader_readtoend_8383, read_to_end_twice_reads_the_rest_7_4) {
    const char* src =
        "module P;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    var t1 := Console.In.ReadToEnd();\n"
        "    var t2 := Console.In.ReadToEnd();\n"
        "    Console.WriteLine(t1.Length);\n"
        "    Console.WriteLine(t2.Length);\n"
        "  end Main;\n"
        "end Program;\n";
    auto o = runWithStdin(src, "hello\nworld\n");
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_EQ(o.stdoutText, std::string("12\n0\n"));
}

// An EXPLICIT `string` local: on the base the zero-initialised slot is
// dereferenced by `text.Length` and the program SEGFAULTS (exit 139) — the
// loud half of the same silent-wrong-code gap. Tip: 12.
BEZEL_TEST(textreader_readtoend_8383, read_to_end_at_an_explicit_string_local_7_4) {
    const char* src =
        "module P;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    var text: string := Console.In.ReadToEnd();\n"
        "    Console.WriteLine(text.Length);\n"
        "  end Main;\n"
        "end Program;\n";
    auto o = runWithStdin(src, "hello\nworld\n");
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_EQ(o.stdoutText, std::string("12\n"));
}

// The read through a METHOD's return value (§22.3's return handoff): the
// base's degraded null return is dereferenced by the caller's `t.Length` and
// the program SEGFAULTS. Tip: 12.
BEZEL_TEST(textreader_readtoend_8383, read_to_end_through_a_method_result_7_4) {
    const char* src =
        "module P;\n"
        "public class Program\n"
        "  static method Helper(): string\n"
        "    return Console.In.ReadToEnd();\n"
        "  end Helper;\n"
        "  public static method Main()\n"
        "    var t := Helper();\n"
        "    Console.WriteLine(t.Length);\n"
        "  end Main;\n"
        "end Program;\n";
    auto o = runWithStdin(src, "hello\nworld\n");
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_EQ(o.stdoutText, std::string("12\n"));
}

// The member on a value of the built-in `TextReader` type (stdlib §7.4's
// second spelling of the same member — `Console.In` is the class's one
// instance, so the read is the same read). Base: a compile REJECT ("no
// dispatch arm emitted any code for this call", #2457). Tip: 12.
BEZEL_TEST(textreader_readtoend_8383, read_to_end_at_a_textreader_local_7_4) {
    const char* src =
        "module P;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    var r: TextReader := Console.In;\n"
        "    var t := r.ReadToEnd();\n"
        "    Console.WriteLine(t.Length);\n"
        "  end Main;\n"
        "end Program;\n";
    auto o = runWithStdin(src, "hello\nworld\n");
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_EQ(o.stdoutText, std::string("12\n"));
}

// The `internal:` degrade notes are GONE: on the base the repro compiles
// with #2434's "degraded to a zero-initialized value" note and #3079's
// "the argument ... failed to lower ... the whole statement emits nothing"
// note — the two notes spec §21.9's wording hangs on. The diagnostics are
// read IN-PROCESS, because a successful compile's warnings never reach
// `runAot`'s `JitResult`.
BEZEL_TEST(textreader_readtoend_8383, no_internal_degrade_note_21_1) {
    bool rejected = true;
    auto msgs = compileMessages(kRepro, rejected);
    BEZEL_EXPECT_FALSE(rejected);
    int internal = 0;
    for (const auto& m : msgs)
        if (m.find("internal:") != std::string::npos ||
            m.find("#2434") != std::string::npos ||
            m.find("#3079") != std::string::npos)
            ++internal;
    BEZEL_EXPECT_EQ(internal, 0);
}

// ===========================================================================
// THE CONTROL BLOCK — these must NOT move (GREEN on the base and on the tip).
// ===========================================================================

// stdlib §2.1's `Console.ReadLine()` — the workaround the issue names —
// still reads a line and still strips its terminator ('hello', not
// 'hello\n').
BEZEL_TEST(textreader_readtoend_8383, console_read_line_still_strips_the_newline_2_1) {
    const char* src =
        "module P;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    var l := Console.ReadLine();\n"
        "    if l <> null then Console.WriteLine($\"[{l}]\"); end if;\n"
        "  end Main;\n"
        "end Program;\n";
    auto o = runWithStdin(src, "hello\nworld\n");
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_EQ(o.stdoutText, std::string("[hello]\n"));
}

// stdlib §2.1's write family is unchanged: `Write` writes no terminator,
// `WriteLine` writes one, the no-argument `WriteLine` writes the terminator
// alone.
BEZEL_TEST(textreader_readtoend_8383, console_write_family_unchanged_2_1) {
    const char* src =
        "module P;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    Console.Write(\"x\");\n"
        "    Console.WriteLine(42);\n"
        "    Console.WriteLine();\n"
        "  end Main;\n"
        "end Program;\n";
    auto o = runWithStdin(src, "");
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_EQ(o.stdoutText, std::string("x42\n\n"));
}

// stdlib §2.1's `Console.Out` / `Console.Error` are still usable as
// `TextWriter`s, and `Console.Error` still writes to standard error (so it
// does not reach stdout).
BEZEL_TEST(textreader_readtoend_8383, console_out_and_error_still_work_2_1) {
    const char* src =
        "module P;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    Console.Out.WriteLine(\"o\");\n"
        "    Console.Error.WriteLine(\"e\");\n"
        "  end Main;\n"
        "end Program;\n";
    auto o = runWithStdin(src, "");
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_EQ(o.stdoutText, std::string("o\n"));
    BEZEL_EXPECT_EQ(o.stderrText, std::string("e\n"));
}

// A USER `TextWriter` subclass's own `Write` is still dispatched to the
// user's `Write` (stdlib §1's Extensibility / §7.4), not to the built-in's.
BEZEL_TEST(textreader_readtoend_8383, user_textwriter_subclass_still_dispatches_7_4) {
    const char* src =
        "module P;\n"
        "public class MyW: TextWriter\n"
        "  public constructor() end constructor;\n"
        "  public override method Write(value: string) Console.Write(\"[\" + value + \"]\"); end Write;\n"
        "  public override method Write(value: char) Console.Write(\"<c>\"); end Write;\n"
        "  public override method Flush() end Flush;\n"
        "end MyW;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    var w := new MyW();\n"
        "    w.Write(\"a\");\n"
        "    w.Write('z');\n"
        "    w.Flush();\n"
        "    Console.WriteLine();\n"
        "  end Main;\n"
        "end Program;\n";
    auto o = runWithStdin(src, "");
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_EQ(o.stdoutText, std::string("[a]<c>\n"));
}

// THE "MUST NOT MOVE" PIN on the SIBLING gap. `TextReader.ReadLine()`
// (stdlib §7.4's other member) has NO lowering arm either, so
// `Console.In.ReadLine()` still degrades to a zero-initialised slot behind
// #2434's warning on BOTH arms and the narrowed read renders "0". This is a
// KNOWN, SEPARATE, PRE-EXISTING gap — NOT #8383's member, and deliberately
// NOT widened into this lane; it is reported in the lane handoff. The row
// exists to prove this lane's `ReadToEnd` arm did not change it.
BEZEL_TEST(textreader_readtoend_8383, console_in_read_line_is_unchanged_7_4) {
    const char* src =
        "module P;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    var l := Console.In.ReadLine();\n"
        "    Console.WriteLine($\"[{l}]\");\n"
        "  end Main;\n"
        "end Program;\n";
    auto o = runWithStdin(src, "hello\nworld\n");
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_EQ(o.stdoutText, std::string("[0]\n"));
}

// A USER CLASS's own `ReadToEnd` is the USER's member (stdlib §1's
// Extensibility), not §7.4's built-in one: the user's method runs and the
// standard input is NOT read behind it. This is the row that pins the
// built-in arm's "a user-declared member owns itself" discriminator — the
// arm must not hijack a member a user declared.
BEZEL_TEST(textreader_readtoend_8383, a_user_read_to_end_is_the_users_own_member_7_4) {
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
        "    var r := new R();\n"
        "    Console.WriteLine(r.ReadToEnd());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end P;\n";
    auto o = runWithStdin(src, "hello\nworld\n");
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_EQ(o.stdoutText, std::string("user\nuser-text\n"));
}

// A USER SUBCLASS of `TextReader` (stdlib §1's Extensibility) owns its own
// `ReadToEnd`: the override runs, and the built-in arm does not consume the
// standard input behind it. This is the row that pins the built-in arm's
// "a DECLARED receiver type is not the built-in head" discriminator.
BEZEL_TEST(textreader_readtoend_8383, a_user_textreader_subclass_owns_its_read_to_end_7_4) {
    const char* src =
        "module M;\n"
        "public class MyR: TextReader\n"
        "  public constructor() end constructor;\n"
        "  public override method ReadToEnd(): string\n"
        "    Console.WriteLine(\"mine\");\n"
        "    return \"mine-text\";\n"
        "  end ReadToEnd;\n"
        "end MyR;\n"
        "public class P\n"
        "  public static method Main(): int\n"
        "    var r := new MyR();\n"
        "    Console.WriteLine(r.ReadToEnd());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end P;\n";
    auto o = runWithStdin(src, "hello\nworld\n");
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_EQ(o.stdoutText, std::string("mine\nmine-text\n"));
}

// A CALL WITH AN ARGUMENT is not §7.4's member (it declares none), so the
// built-in arm does not claim it: the standard input is NOT read. The row
// pins the arm's arity check. (The call itself still degrades on both arms —
// the built-in `TextReader`'s member surface is a SEMA handback
// (`src/sema/ExprTypecheck_Calls.cpp`, a file this lane does not own), so no
// member of it is typed. This lane's arm is a CODEGEN fix only.)
BEZEL_TEST(textreader_readtoend_8383, a_read_to_end_with_an_argument_is_not_the_builtin_7_4) {
    const char* src =
        "module M;\n"
        "public class P\n"
        "  public static method Main(): int\n"
        "    var text := Console.In.ReadToEnd(\"x\");\n"
        "    Console.WriteLine(\"after\");\n"
        "    return 0;\n"
        "  end Main;\n"
        "end P;\n";
    auto o = runWithStdin(src, "hello\nworld\n");
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_EQ(o.stdoutText, std::string("after\n"));
}
