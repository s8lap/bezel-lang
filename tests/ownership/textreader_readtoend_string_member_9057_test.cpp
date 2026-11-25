// Regression for #9057 — a §9.6 STRING MEMBER on a `var`-inferred local
// initialized from `Console.In.ReadToEnd()` LOWERED TO NOTHING.
//
// THE ISSUE (wrong-output, stdlib §7.4 / §9.6, v0.359). The issue's own
// repro, verbatim:
//
//     var a := "AbC";
//     var b := a.ToLower();          // fine — a literal's type is `string`
//     Console.WriteLine(b);
//     var raw := Console.In.ReadToEnd();
//     var c := raw.ToUpper();        // WRONG: lowers to nothing
//     Console.WriteLine(c);
//     var d := raw.Trim();           // WRONG: lowers to nothing
//     Console.WriteLine(d);
//     var e: string := raw;          // fine — the ASCRIPTION gives `e` a type
//     Console.WriteLine(e.ToLower());
//
// and the CHAINED spelling `Console.In.ReadToEnd().ToLower()`.
//
// THE GAP. stdlib §7.4 (v0.359) declares, on the abstract `TextReader`,
//
//     public method ReadToEnd(): string;
//
// and stdlib §2.1 (v0.359) declares `Console.In` as that class's ONE instance
// ("`TextReader` has no concrete subclass in this library: its one instance is
// `Console.In`"). The built-in `TextReader` carries NO member surface in sema
// — it has no `TypeDecl`, so the ordinary member lookup finds nothing — and
// the call was therefore left UNTYPED. The #8383 lane that gave the call its
// CODEGEN lowering arm recorded exactly this as the half it left behind: "sema
// leaves the call untyped (the built-in `TextReader` has no member surface
// there)".
//
// WHY AN UNTYPED CALL IS A WRONG-OUTPUT DEFECT AND NOT A COSMETIC ONE. An
// untyped initializer infers NOTHING for the local it binds (§21.1's `var
// greeting := "Hello"; // type inferred`), and codegen's §9.6 string-member
// dispatch REQUIRES a `string`-typed receiver (`tryLowerStringMethod`'s
// `tt->primitive == PrimitiveType::String` gate,
// `src/codegen/LlvmBackend_Exprs.cpp`). So the type never REACHED the
// dispatch, `raw.ToUpper()` lowered to NOTHING, the local `c` was
// zero-initialised behind #2434's "internal:" warning, and the program
// printed `0`.
//
// MEASURED on the base (r30-c1 @ 3a68f2a02d), the issue's own repro with
// empty standard input, verbatim:
//
//     abc
//     0
//     0
//                                    <- the ASCRIBED twin, already correct
//
// and with the two internal notes:
//
//     nb16.bzl:8:14: warning: internal: lowerLocalDecl's initializer
//       expression failed to lower to a value for this local's slot
//       (degraded to a zero-initialized value) — #2434
//     nb16.bzl:10:14: warning: internal: … — #2434
//
// MEASURED on the fix's tip, the SAME program with "Hello World\n" on
// standard input:
//
//     abc
//     HELLO WORLD
//                                    <- raw.ToUpper(): the text AS READ
//     Hello World
//                                    <- raw.Trim(): the text trimmed
//     hello world
//                                    <- the ASCRIBED twin, unchanged
//
// and NO internal note.
//
// THE TWO SURVIVORS (the issue names them as already correct) — WHY THEY
// SURVIVED. `raw.Length` is the one member that still worked: codegen's
// degenerate-receiver fallback reads the length field of whatever `%bz.string`
// the receiver lowers to (`LlvmBackend_Exprs.cpp`'s `isStringFallbackTarget`
// arm), so it guessed the layout right by accident, WITHOUT the type. And
// `var e: string := raw;` gave `e` a type by ASCRIPTION, so the member
// dispatch had a `string` receiver to work with. Both are the CONTROL rows
// below.
//
// THE RUNNER. `runAot` opens fd 0 on /dev/null, so a program that READS
// standard input sees EOF and cannot be observed. This runner compiles
// through the REAL Driver (the shipping `bezelc` AOT path — the same
// `Driver::compileFile` `runAot` drives) and runs the executable with fd 0
// opened READ-ONLY on a file holding the row's standard input, capturing
// stdout and stderr. The same shape as the fd-0 trap of
// `tests/codegen/textreader_readtoend_8383_test.cpp` and of
// `tests/codegen/textwriter_value_7792_test.cpp`, with fd 0 carrying INPUT
// instead of catching OUTPUT.
//
// THE LEAK — RE-ENABLED by fix #9199 (stdlib §7.4 / §4.6 / §28.3 / §28.9,
// v0.359). This runner used to set `detect_leaks=0`, because EVERY row's
// program leaked the CONSOLE-READ STRING itself (45 bytes for a 12-byte input:
// a 32-byte header and a 13-byte buffer). That was NOT this lane's property:
// it was the gap the #8383 lane measured and reported ("`Emitter::
// isPlusOneStringProducer` has no `ReadToEnd` arm, so a `ReadToEnd()` result
// is never discharged — NOT the stored form and NOT the unstored one"), and it
// was measured on the CONTROL rows too.
//
// fix #9199 added that arm: the §7.4 `TextReader.ReadToEnd(): string` result
// is a +1 string PRODUCER, so the caller owns exactly ONE reference and
// discharges it — the bound local is released at scope end, a call argument
// at the end of the full expression. The read text is therefore no longer
// leaked, and this runner now runs with `detect_leaks=1`: the ASan leg
// catches a use-after-free or an overflow AND LeakSanitizer catches a
// residual leak (the runner's `exitcode=97` keeps a leak distinguishable
// from the program's own exit status).
//
// THE ROWS THAT CHANGED. The rows that READ standard input — the four
// `local_to_lower_on_a_readtoend_local_9_6`,
// `local_trim_on_a_readtoend_local_9_6`,
// `chained_to_lower_on_a_readtoend_call_9_6` and
// `chained_trim_on_a_readtoend_call_9_6` rows, and the two CONTROL rows
// `the_length_twin_of_the_readtoend_local_still_works_9_6` and
// `the_ascribed_local_twin_of_the_readtoend_local_still_works_9_6` — are the
// ones the leak oracle now watches. The rows that only COMPILE
// (`no_internal_degrade_note_for_the_string_members_21_1`) are unaffected.

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
#include "support/force_sanitize.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

namespace {

// ---------------------------------------------------------------------------
// The runner — the REAL AOT path, with fd 0 carrying the row's input.
// ---------------------------------------------------------------------------

struct RunOutcome {
    bool compiled = false;
    int exitCode = -1;
    std::string stdoutText;
    std::string stderrText;
};

const std::filesystem::path& rteScratchDir() {
    return wtScratchDir("wt-tr9057");
}

std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

RunOutcome runWithStdin(const std::string& source,
                        const std::string& stdinText) {
    auto dir = rteScratchDir();
    std::string stem = unique_stem("bezel_tr9057");
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
                         "detect_leaks=1:abort_on_error=0:exitcode=97", 1);
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

// Every diagnostic message of an in-process front-end + codegen run. The
// AOT driver drops the diagnostics of a SUCCESSFUL compile, so a Warning's
// absence can only be observed here (the pattern of
// `tests/codegen/textreader_readtoend_8383_test.cpp` and of
// `tests/codegen/textwriter_value_7792_test.cpp`).
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

// The issue's repro, VERBATIM (§ "Minimal repro" of issue #9057). The rows
// below use it in the pieces each row needs; this is the whole program.
const char* kRepro =
    "module P;\n"
    "public class Program\n"
    "  public static method Main()\n"
    "    var a := \"AbC\";\n"
    "    var b := a.ToLower();\n"
    "    Console.WriteLine(b);\n"
    "    var raw := Console.In.ReadToEnd();\n"
    "    var c := raw.ToUpper();\n"
    "    Console.WriteLine(c);\n"
    "    var d := raw.Trim();\n"
    "    Console.WriteLine(d);\n"
    "    var e: string := raw;\n"
    "    Console.WriteLine(e.ToLower());\n"
    "  end Main;\n"
    "end Program;\n";

// The standard input every row reads. 12 bytes: "Hello World\n" — the
// newline INCLUDED, because §7.4's `ReadToEnd` returns the text AS READ (the
// same fact the #8383 rows pin).
const char* kStdin = "Hello World\n";

}  // namespace

// ===========================================================================
// THE FAIL-WITHOUT BLOCK — every row RED on the base, GREEN on the tip.
// ===========================================================================

// A §9.6 `ToLower()` on the INFERRED local. Base: the initializer lowers to
// nothing behind #2434's note, the local is zero-initialised, and the
// program prints `0` — the issue's own symptom.
BEZEL_TEST(textreader_readtoend_string_member_9057,
           local_to_lower_on_a_readtoend_local_9_6) {
    const char* src =
        "module P;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    var raw := Console.In.ReadToEnd();\n"
        "    var c := raw.ToLower();\n"
        "    Console.WriteLine(c);\n"
        "  end Main;\n"
        "end Program;\n";
    auto o = runWithStdin(src, kStdin);
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_EQ(o.stdoutText, std::string("hello world\n\n"));
}

// A §9.6 `Trim()` on the INFERRED local. Base: `0`, as above.
BEZEL_TEST(textreader_readtoend_string_member_9057,
           local_trim_on_a_readtoend_local_9_6) {
    const char* src =
        "module P;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    var raw := Console.In.ReadToEnd();\n"
        "    var d := raw.Trim();\n"
        "    Console.WriteLine(d);\n"
        "  end Main;\n"
        "end Program;\n";
    auto o = runWithStdin(src, kStdin);
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    // §7.4 returns the text AS READ ("Hello World\n"); §9.6's `Trim` strips
    // the surrounding whitespace, the trailing newline included.
    BEZEL_EXPECT_EQ(o.stdoutText, std::string("Hello World\n"));
}

// The CHAINED spelling: the RECEIVER of the §9.6 member is the
// `Console.In.ReadToEnd()` call itself. Base: the whole `WriteLine` statement
// emits nothing behind #3079's note — not even the line terminator — so the
// program prints NOTHING at all.
BEZEL_TEST(textreader_readtoend_string_member_9057,
           chained_to_lower_on_a_readtoend_call_9_6) {
    const char* src =
        "module P;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    Console.WriteLine(Console.In.ReadToEnd().ToLower());\n"
        "  end Main;\n"
        "end Program;\n";
    auto o = runWithStdin(src, kStdin);
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_EQ(o.stdoutText, std::string("hello world\n\n"));
}

// The chained `Trim()` spelling. Base: nothing printed, as above.
BEZEL_TEST(textreader_readtoend_string_member_9057,
           chained_trim_on_a_readtoend_call_9_6) {
    const char* src =
        "module P;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    Console.WriteLine(Console.In.ReadToEnd().Trim());\n"
        "  end Main;\n"
        "end Program;\n";
    auto o = runWithStdin(src, kStdin);
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_EQ(o.stdoutText, std::string("Hello World\n"));
}

// The issue's "Expected" names the NOTE as part of the expected behaviour:
// "No warning; `c` and `d` hold the upper-cased and trimmed input text". The
// #2434 note ("lowerLocalDecl's initializer expression failed to lower to a
// value for this local's slot") is the degrade marker the wrong output came
// with, so it must be gone with it. Base: the note is there, twice.
BEZEL_TEST(textreader_readtoend_string_member_9057,
           no_internal_degrade_note_for_the_string_members_21_1) {
    bool rejected = false;
    auto msgs = compileMessages(kRepro, rejected);
    BEZEL_EXPECT_FALSE(rejected);
    for (const auto& m : msgs) {
        BEZEL_EXPECT_TRUE(m.find("#2434") == std::string::npos);
        BEZEL_EXPECT_TRUE(m.find("#3079") == std::string::npos);
    }
}

// ===========================================================================
// THE CONTROL BLOCK — GREEN on the base AND on the tip. These pin the two
// survivors the issue names as already correct, so this lane's fix cannot
// have moved them. Each is RED under the plausible WRONG fix this commit
// names: typing the `ReadToEnd()` call as `string?` (or as `object`) instead
// of `string` would make the ascribed local's assignment ill-typed and the
// length read a nullable member access.
// ===========================================================================

// The `t.Length` twin the issue names: the degenerate-receiver fallback got
// this one right by accident, WITHOUT the type. It must stay right.
BEZEL_TEST(textreader_readtoend_string_member_9057,
           the_length_twin_of_the_readtoend_local_still_works_9_6) {
    const char* src =
        "module P;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    var raw := Console.In.ReadToEnd();\n"
        "    Console.WriteLine(raw.Length);\n"
        "  end Main;\n"
        "end Program;\n";
    auto o = runWithStdin(src, kStdin);
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    // §7.4 returns the text AS READ — the newline INCLUDED.
    BEZEL_EXPECT_EQ(o.stdoutText, std::string("12\n"));
}

// The ascribed twin the issue names: `var e: string := raw;` gave `e` its
// type by ASCRIPTION, so `e.ToLower()` always worked. It must keep working,
// and it must keep agreeing with the INFERRED spelling this lane fixed.
BEZEL_TEST(textreader_readtoend_string_member_9057,
           the_ascribed_local_twin_of_the_readtoend_local_still_works_9_6) {
    const char* src =
        "module P;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    var raw := Console.In.ReadToEnd();\n"
        "    var e: string := raw;\n"
        "    Console.WriteLine(e.ToLower());\n"
        "  end Main;\n"
        "end Program;\n";
    auto o = runWithStdin(src, kStdin);
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_EQ(o.stdoutText, std::string("hello world\n\n"));
}
