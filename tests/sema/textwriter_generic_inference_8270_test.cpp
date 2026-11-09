// Regression test for issue #8270 (§14.1 / §14.5, stdlib §2.1, v0.355).
//
// THE DEFECT. §14.5's phase 1 binds a generic method's type argument from an
// argument's STATIC TYPE:
//
//   §14.5 *Type-argument inference for generic methods*
//   (docs/Bezel_Language_Spec.md:2752-2759):
//     "1. **Argument-driven unification.** Each argument whose type is
//      self-determined ... is unified structurally against its parameter
//      type ...
//        - a parameter type that is a bare type parameter `X` binds `X` to the
//          argument's static type;"
//
// and stdlib §2.1 declares `Console`'s stream properties with a CONCRETE type
// (docs/Bezel_Standard_Library.md:115-117):
//
//     public static readonly property In: TextReader;
//     public static readonly property Out: TextWriter;
//     public static readonly property Error: TextWriter;
//
// so `Id(Console.Error)` at `static method Id<T>(x: T): T` must bind
// `T = TextWriter` — exactly as an `int`, a `string` or a user-class argument
// does.
//
// MEASURED on the pre-fix base (90087bc4): the call is REJECTED with
//
//   "cannot infer the type argument `T` of `Id`'s call — it is mentioned only
//    inside a parameter whose declared type did not unify with that
//    argument's actual type (...)"
//
// The cause is NOT the unifier's treatment of the declaration-less Named
// `TextWriter` TypeRef (the issue's own hypothesis). MEASURED with a
// temporary probe in the inference loop: the argument's TypeRef is NULL —
// `Console.Error` carries NO type at all — so §14.5's unification is never
// reached. The reason is #7924's: the property read is left untyped BY
// DESIGN (the chained `Console.Out.Write` paths are syntactic), and the
// CALL-ARGUMENT position was the one VALUE position the minted `TextWriter`
// type never reached.
//
// THE FIX (src/sema/ExprTypecheck_Exprs.cpp). At a call-argument position the
// `Console` stream-property read carries the property's DECLARED type, read
// from the stdlib declaration block the compiler already carries
// (`common::builtinDeclaredProperty`). The read stays untyped everywhere
// else, so #7924's invariant is untouched and the chained
// `Console.Error.WriteLine(x)` path is byte-identical.
//
// The rows below pin the whole family: the issue's own repro, the two
// `TextWriter` streams, the `TextReader` stream, the EXPLICIT spelling (which
// worked before and must keep working), the direct use, the inferred local
// (and its TYPE), the `T?` parameter, the four negative controls, and the
// §5.1 target-type row with the controls that show its verdict is the
// EXPLICIT spelling's verdict.

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "driver/Driver.hpp"
#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// A `TextWriter` value is a standard-stream DESCRIPTOR (stdlib §7.4), so a
// write through `Console.Error` reaches standard ERROR and a write through
// `Console.Out` reaches standard OUTPUT. The harness captures stdout only,
// so the rows below that write to `Console.Error` need the stderr channel too:
// this helper compiles through the real Driver (the AOT path) and runs the
// executable with stdout and stderr redirected to files.
struct StreamOutcome {
    bool compiled = false;
    int exitCode = -1;
    std::string stdoutText;
    std::string stderrText;
};

static const std::filesystem::path& wtDirTw8270() {
    return wtScratchDir("wt-tw8270");
}

static std::string slurp(const std::string& path) {
    std::ifstream in(path);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

StreamOutcome runStreams(const std::string& source) {
    auto dir = wtDirTw8270();
    std::string stem = unique_stem("bezel_tw8270");
    std::string src = (dir / (stem + ".bzl")).string();
    std::string exe = (dir / stem).string();
    std::string outf = (dir / (stem + ".stdout")).string();
    std::string errf = (dir / (stem + ".stderr")).string();
    {
        std::ofstream out(src);
        out << source;
    }
    StreamOutcome o;
    bezel::Driver driver;
    bezel::codegen::CodegenOptions opts;
    opts.sanitizeAddress = bezel::test::forceSanitizeAddress();
    bezel::CompileResult r = driver.compileFile(src, exe, /*deps=*/{}, opts);
    o.compiled = r.success;
    if (r.success) {
        pid_t pid = ::fork();
        if (pid == 0) {
            if (opts.sanitizeAddress)
                ::setenv("ASAN_OPTIONS",
                         "detect_leaks=0:abort_on_error=0:exitcode=97", 1);
            int fd1 = ::open(outf.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
            int fd2 = ::open(errf.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (fd1 < 0 || fd2 < 0) ::_exit(126);
            ::dup2(fd1, 1);
            ::dup2(fd2, 2);
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
    return o;
}

// The issue's own repro, verbatim (the `Id` method and the `Main` body).
//
//   module M; class P static method Id<T>(x: T): T return x; end Id;
//   public static method Main() Id(Console.Error).WriteLine("x");
//   var w := Id(Console.Error); w.WriteLine("y");
//   Console.WriteLine("end"); end Main; end P;
//
// PRE-FIX: rejected with "cannot infer the type argument `T` of `Id`'s call".
// POST-FIX: `T` binds to `TextWriter` (§14.5 phase 1), so both writes reach
// standard ERROR (`Console.Error`) and "end" reaches standard OUTPUT.
BEZEL_TEST(sema_textwriter_generic_inference_8270,
           issue_repro_binds_textwriter_from_a_console_error_argument_14_5) {
    auto o = runStreams(
        "module M; class P static method Id<T>(x: T): T return x; end Id; "
        "public static method Main() Id(Console.Error).WriteLine(\"x\"); "
        "var w := Id(Console.Error); w.WriteLine(\"y\"); "
        "Console.WriteLine(\"end\"); end Main; end P;");
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_EQ(o.stdoutText, std::string("end\n"));
    BEZEL_EXPECT_EQ(o.stderrText, std::string("x\ny\n"));
}

// The same family: `Console.Out` binds `T = TextWriter` too, and the write
// reaches standard OUTPUT.
BEZEL_TEST(sema_textwriter_generic_inference_8270,
           console_out_binds_textwriter_and_writes_to_stdout_14_5) {
    auto o = runStreams(
        "module M; class P static method Id<T>(x: T): T return x; end Id; "
        "public static method Main() var w := Id(Console.Out); "
        "w.WriteLine(\"y\"); Console.WriteLine(\"end\"); end Main; end P;");
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_EQ(o.stdoutText, std::string("y\nend\n"));
    BEZEL_EXPECT_EQ(o.stderrText, std::string(""));
}

// The `TextReader` stream: `Console.In` is declared `TextReader` (stdlib
// §2.1), so §14.5 binds `T = TextReader` and the "cannot infer" rejection is
// GONE.
//
// The program is still refused — but by CODEGEN, not by sema: this compiler
// has no runtime representation for a `TextReader` value, so the argument does
// not lower. That verdict is the PRE-EXISTING one for the same shape on the
// pre-fix base: `Emit(Console.In)` at a `TextReader` parameter fails there
// with the very same diagnostic (#1386). The row therefore pins the SEMANTIC
// change — the type argument IS inferred — and not the codegen gap, which is
// a separate defect (a `TextReader` value has no representation).
BEZEL_TEST(sema_textwriter_generic_inference_8270,
           console_in_binds_textreader_so_the_cannot_infer_rejection_is_gone_14_5) {
    // The control: the pre-existing verdict for the same shape WITHOUT the
    // inference (`Emit(Console.In)` at a `TextReader` parameter).
    {
        auto c = runStreams(
            "module M; class P static method Emit(w: TextReader) "
            "Console.WriteLine(\"e\"); end Emit; "
            "public static method Main() Emit(Console.In); end Main; end P;");
        BEZEL_EXPECT_FALSE(c.compiled);
    }
    auto r = runAot(
        "module M; class P static method Id<T>(x: T): T return x; end Id; "
        "public static method Main() var w := Id(Console.In); "
        "Console.WriteLine(\"end\"); end Main; end P;");
    BEZEL_EXPECT_FALSE(r.ran);
    // The §14.5 rejection is GONE.
    BEZEL_EXPECT_FALSE(bezel::test::diagnosticMessageContains(
        r, "cannot infer the type argument"));
    // What remains is the pre-existing codegen gap for a `TextReader` value.
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "lowerExpr returned null for call argument"));
}

// The EXPLICIT spelling worked before the fix and must keep working: the
// inference is now an ALTERNATIVE to it, not a replacement.
BEZEL_TEST(sema_textwriter_generic_inference_8270,
           explicit_textwriter_spelling_still_works_14_5) {
    auto o = runStreams(
        "module M; class P static method Id<T>(x: T): T return x; end Id; "
        "public static method Main() Id<TextWriter>(Console.Error)"
        ".WriteLine(\"x\"); Console.WriteLine(\"end\"); end Main; end P;");
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_EQ(o.stdoutText, std::string("end\n"));
    BEZEL_EXPECT_EQ(o.stderrText, std::string("x\n"));
}

// The result used DIRECTLY as a receiver: `Id(Console.Error).WriteLine("x")`.
BEZEL_TEST(sema_textwriter_generic_inference_8270,
           inferred_result_used_directly_as_a_receiver_14_5) {
    auto o = runStreams(
        "module M; class P static method Id<T>(x: T): T return x; end Id; "
        "public static method Main() Id(Console.Error).WriteLine(\"x\"); "
        "Console.WriteLine(\"end\"); end Main; end P;");
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_EQ(o.stdoutText, std::string("end\n"));
    BEZEL_EXPECT_EQ(o.stderrText, std::string("x\n"));
}

// The inferred LOCAL, and its TYPE.
//
// `w.WriteLine("y")` is the type oracle: `WriteLine` is a member of the
// built-in `TextWriter` (stdlib §7.4), so it resolves on `w` only when `w`'s
// type IS a `TextWriter`; and it needs no §7.4 narrowing, so `w` is
// NON-NULLABLE. The write reaching standard ERROR then proves the local
// holds the `Console.Error` stream and not a zero-initialised value.
//
// The `var t: TextWriter := w;` row pins the type from the other side: the
// local's declared type is the stdlib's `TextWriter`, and the assignment is
// admitted.
BEZEL_TEST(sema_textwriter_generic_inference_8270,
           inferred_local_has_the_textwriter_type_14_5) {
    auto o = runStreams(
        "module M; class P static method Id<T>(x: T): T return x; end Id; "
        "static method Take(w: TextWriter) w.WriteLine(\"t\"); end Take; "
        "public static method Main() var w := Id(Console.Error); "
        "w.WriteLine(\"y\"); var t: TextWriter := w; Take(t); "
        "Console.WriteLine(\"end\"); end Main; end P;");
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_EQ(o.stdoutText, std::string("end\n"));
    BEZEL_EXPECT_EQ(o.stderrText, std::string("y\nt\n"));
}

// The `T?` parameter spelling: §14.5's `X?` bullet ("a non-nullable argument
// of type `A` binds `X` to `A`") binds `T = TextWriter`, and the parameter
// admits it.
BEZEL_TEST(sema_textwriter_generic_inference_8270,
           a_nullable_type_parameter_binds_from_a_console_error_argument_14_5) {
    auto o = runStreams(
        "module M; class P static method Id<T>(x: T?): T? return x; end Id; "
        "public static method Main() var w := Id(Console.Error); "
        "w!.WriteLine(\"y\"); Console.WriteLine(\"end\"); end Main; end P;");
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_EQ(o.stdoutText, std::string("end\n"));
    BEZEL_EXPECT_EQ(o.stderrText, std::string("y\n"));
}

// The OTHER argument kinds keep inferring: an `int`, a `string` and a user
// class. These are the controls that show the fix did not narrow the
// inference to the stream family.
BEZEL_TEST(sema_textwriter_generic_inference_8270,
           int_string_and_user_class_arguments_still_infer_14_5) {
    {
        auto r = runAot(
            "module M; class P static method Id<T>(x: T): T return x; end Id; "
            "public static method Main() var w := Id(42); "
            "Console.WriteLine(w); end Main; end P;");
        BEZEL_EXPECT_TRUE(r.ran);
        BEZEL_EXPECT_EQ(r.stdoutText, std::string("42\n"));
    }
    {
        auto r = runAot(
            "module M; class P static method Id<T>(x: T): T return x; end Id; "
            "public static method Main() var w := Id(\"s\"); "
            "Console.WriteLine(w); end Main; end P;");
        BEZEL_EXPECT_TRUE(r.ran);
        BEZEL_EXPECT_EQ(r.stdoutText, std::string("s\n"));
    }
    {
        auto r = runAot(
            "module M; class Sw public constructor() end constructor; end Sw; "
            "class P static method Id<T>(x: T): T return x; end Id; "
            "public static method Main() using var s := new Sw(); "
            "var w := Id(s); Console.WriteLine(\"ok\"); end Main; end P;");
        BEZEL_EXPECT_TRUE(r.ran);
        BEZEL_EXPECT_EQ(r.stdoutText, std::string("ok\n"));
    }
}

// NEGATIVE CONTROL 1 (the critical one): a REAL type parameter — a generic
// method whose own `T` is bound by an argument — still infers. A fix that
// made a genuine type-parameter reference unresolvable would fail here.
BEZEL_TEST(sema_textwriter_generic_inference_8270,
           a_real_type_parameter_argument_still_infers_14_5) {
    auto r = runAot(
        "module M; class P static method Id<T>(x: T): T return x; end Id; "
        "static method Wrap<T>(x: T): T return Id(x); end Wrap; "
        "public static method Main() var w := Wrap(42); "
        "Console.WriteLine(w); end Main; end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("42\n"));
}

// NEGATIVE CONTROL 2: a GENUINELY UNRESOLVED name is still rejected — it must
// not silently bind to something.
BEZEL_TEST(sema_textwriter_generic_inference_8270,
           an_unresolved_name_is_still_rejected_14_5) {
    auto r = runAot(
        "module M; class P static method Id<T>(x: T): T return x; end Id; "
        "public static method Main() var w := Id(SomeNameThatDoesNotExist); "
        "Console.WriteLine(\"end\"); end Main; end P;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "unresolved identifier `SomeNameThatDoesNotExist`"));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "cannot infer the type argument"));
}

// NEGATIVE CONTROL 3: a TYPE-PARAMETER-ONLY argument — the enclosing `T` of a
// generic method — still works. This is the "mentioned only inside a
// parameter" case the §14.5 rejection names.
BEZEL_TEST(sema_textwriter_generic_inference_8270,
           a_type_parameter_only_argument_still_infers_14_5) {
    auto r = runAot(
        "module M; class P static method Id<T>(x: T): T return x; end Id; "
        "static method Inner<T>(t: T): T return Id(t); end Inner; "
        "public static method Main() var w := Inner(7); "
        "Console.WriteLine(w); end Main; end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("7\n"));
}

// The §5.1 TARGET-TYPE row: `Id(Console.Error)` at a target type that
// disagrees.
//
// The pre-fix base REJECTED this program — but with the §14.5 "cannot infer"
// diagnostic, i.e. as a SYMPTOM of the defect this issue fixes, not as a §5.1
// verdict. After the fix the call infers `T = TextWriter`, and the verdict is
// the EXPLICIT spelling's verdict at the same target — which the base also
// gives, and which the issue's own expectation ("the explicit spellings
// `Id<TextWriter>(Console.Error)` ... compile") requires.
//
// The controls below pin WHY: the built-in `TextWriter` is not a §5.1
// conversion-checked source on EITHER side — a `TextWriter` local assigned to
// an `int` local is admitted on the base too — while a USER CLASS at the same
// target IS rejected. So the target-type disagreement is undecided for the
// built-in `TextWriter` on both sides; closing that §5.1 hole is a separate
// defect in `src/sema/ExprTypecheck.cpp`'s `checkAssignable`, outside this
// lane's declared hot file, and closing it would change the EXPLICIT
// spelling's base verdict too.
BEZEL_TEST(sema_textwriter_generic_inference_8270,
           a_disagreeing_target_type_has_the_explicit_spelling_verdict_5_1) {
    // The control: the built-in `TextWriter` at an `int` target is admitted
    // (the §5.1 hole), so the disagreement is undecided.
    {
        auto r = runAot(
            "module M; class P public static method Main() "
            "var t: TextWriter := Console.Error; var w: int := t; "
            "Console.WriteLine(\"end\"); end Main; end P;");
        BEZEL_EXPECT_TRUE(r.ran);
    }
    // The control: a USER CLASS at the same target IS rejected (§5.1).
    {
        auto r = runAot(
            "module M; class Sw public constructor() end constructor; end Sw; "
            "class P public static method Main() using var s := new Sw(); "
            "var w: int := s; end Main; end P;");
        BEZEL_EXPECT_FALSE(r.ran);
        BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
            r, "not implicitly convertible to the target type"));
    }
    // The row itself: the inferred call at the disagreeing target has the
    // EXPLICIT spelling's verdict.
    {
        auto inferred = runAot(
            "module M; class P static method Id<T>(x: T): T return x; end Id; "
            "public static method Main() var w: int := Id(Console.Error); "
            "Console.WriteLine(\"end\"); end Main; end P;");
        auto explicitSpelling = runAot(
            "module M; class P static method Id<T>(x: T): T return x; end Id; "
            "public static method Main() var w: int := "
            "Id<TextWriter>(Console.Error); "
            "Console.WriteLine(\"end\"); end Main; end P;");
        BEZEL_EXPECT_EQ(inferred.ran, explicitSpelling.ran);
    }
}

}  // namespace
