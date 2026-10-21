// Spec-derived regression test for issue #6753.
//
// stdlib §2.1 (`Console`), v0.340: "The `Object` overloads invoke `ToString()`."
// stdlib §3.1 (`StringBuilder`), v0.340: `Append(value: Object)` keeps
// `value.ToString()`'s text, and `StringBuilder` declares its own
// `public override method ToString(): string`, so its `ToString()` is its
// content. Spec §3.1 (Type kinds), v0.340: classes "are reference types whose
// values are already heap-allocated and reach `Object` directly through the
// inheritance chain" (they are not boxed). Spec §8.5 (`ToString`), v0.340: for
// classes, the default is "the type's qualified name", and "a `null` value of a
// nullable-typed hole renders as the text `null` with no dispatch".
//
// THE DEFECT. The console and `Append` arms of the StringBuilder lowering had no
// arm for a DECLARATION-LESS built-in class, so a `StringBuilder` argument fell
// to the raw-write fallback, and an `Object`-typed argument went through
// `lowerObjectToString`. A vtable-less built-in class has no Bezel vtable, so
// `bz_obj_is_string` answers "string" and the plain runtime struct was read as a
// `%bz.string`. MEASURED on f2f81408 (the base): the issue's repro 1 printed "q"
// and then four bytes of the struct; repro 2 aborted with exit 134
// ("free(): invalid pointer").
//
// THE FIX asks the one renderer that already exists (`emitValueToString`, shared
// with the §18.5 hole) for these receivers, and narrows an `Object`-typed local
// through its initializer the same way the hole does.
//
// WHY THE ORACLES ARE WHAT THEY ARE. Rows that read the program's text compare
// stdout exactly, so a wrong rendering fails them. The stderr row compares the
// captured stderr, since `JitResult` carries stdout only. The loop rows run
// under the checked oracle (`runAotChecked`, sanitizers armed) and also require
// exit 0, so a leak or a use-after-free fails them rather than passing silently.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include "driver/Driver.hpp"
#include "support/aot_support.hpp"   // runAotOut, runAotChecked
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;
using bezel::test::runAot;

namespace {

// Raw stdout, so an expectation that spells its newline is exact. runAotOut
// strips one trailing newline, which would make `Console.Write("q")` and
// `Console.WriteLine("q")` indistinguishable in these rows. Other codegen tests
// compare `stdoutText` the same way (for example `"600\n1200\n"`).
std::string out(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}

// Checked oracle: the program must run with sanitizers armed and exit 0. The
// value is its stdout, so a wrong rendering or a leak/UAF fails the row.
std::string checkedOut(const std::string& src) {
    auto r = runAotChecked(src);
    if (!r.ran) return "<did-not-run>";
    if (r.exitCode != 0) return "<exit " + std::to_string(r.exitCode) + "> " + r.stdoutText;
    return r.stdoutText;
}

// Module + class skeleton: `prelude` declares helper classes before `class C`,
// `imports` goes at the top, and `mainBody` is the body of `Main`.
std::string prog(const std::string& mainBody, const std::string& prelude = "",
                 const std::string& imports = "") {
    return "module M; " + imports + prelude +
           "class C public static method Main(): int " + mainBody +
           " return 0; end Main; end C;";
}

// The stderr channel is not in `JitResult`, so this compiles through the Driver
// and redirects the child's stderr to a file. exception_uncaught_report_spec_33_3
// uses the same pattern for the same reason.
std::string stderrOf(const std::string& src) {
    auto dir = wtScratchDir("wt-console6753");
    std::string stem = unique_stem("bezel_console6753");
    std::string srcPath = (dir / (stem + ".bzl")).string();
    std::string exe = (dir / stem).string();
    std::string errf = (dir / (stem + ".stderr")).string();
    {
        std::ofstream o(srcPath);
        o << src;
    }
    std::string text;
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileFile(srcPath, exe);
    if (r.success) {
        std::string cmd = exe + " > /dev/null 2> " + errf;
        std::system(cmd.c_str());
        std::ifstream in(errf);
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    std::remove(srcPath.c_str());
    std::remove(exe.c_str());
    std::remove(errf.c_str());
    return text;
}

// A user class whose `ToString` is declared, for the control rows.
const char* kDClass =
    "public class D "
    "  public override method ToString(): string => \"dd\"; "
    "end D; ";

}  // namespace

// ---------------------------------------------------------------------------
// stdlib §2.1 / §3.1. The issue's repro 1, verbatim: the explicit `ToString()`
// call prints the content, and the `Object`-overload `WriteLine(sb)` must print
// the same text.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_console_append_builtin_6753,
           writeline_static_stringbuilder_is_its_content_2_1) {
    BEZEL_EXPECT_EQ(out(prog(
                        "var sb := new StringBuilder(); _ := sb.Append(\"q\"); "
                        "Console.WriteLine(sb.ToString()); Console.WriteLine(sb);")),
                    std::string("q\nq\n"));
}

// stdlib §2.1: `Write` writes its argument with no terminator. The Object
// overload of `Write` takes the same route, so the text is exact with no newline.
BEZEL_TEST(codegen_console_append_builtin_6753,
           write_static_stringbuilder_no_terminator_2_1) {
    BEZEL_EXPECT_EQ(out(prog(
                        "var sb := new StringBuilder(); _ := sb.Append(\"he\"); "
                        "_ := sb.Append(\"llo\"); Console.Write(sb);")),
                    std::string("hello"));
}

// stdlib §2.1: `Console.Out` is the same destination as `Console.WriteLine`.
BEZEL_TEST(codegen_console_append_builtin_6753,
           out_writeline_stringbuilder_is_its_content_2_1) {
    BEZEL_EXPECT_EQ(out(prog(
                        "var sb := new StringBuilder(); _ := sb.Append(\"he\"); "
                        "_ := sb.Append(\"llo\"); Console.Out.WriteLine(sb);")),
                    std::string("hello\n"));
}

// stdlib §2.1: `Console.Error.WriteLine` takes the same `Object` route, on stderr.
// The row reads stderr, because stdout would not show a wrong byte stream here.
BEZEL_TEST(codegen_console_append_builtin_6753,
           error_writeline_stringbuilder_is_its_content_2_1) {
    BEZEL_EXPECT_EQ(stderrOf(prog(
                        "var sb := new StringBuilder(); _ := sb.Append(\"he\"); "
                        "_ := sb.Append(\"llo\"); Console.Error.WriteLine(sb);")),
                    std::string("hello\n"));
}

// stdlib §2.1 / §3.1, under the checked oracle: one `WriteLine` of a named
// `StringBuilder` renders its content and leaves the program leak-free with no
// use-after-free. The temporary form, `Console.WriteLine(new StringBuilder("q"))`,
// prints "q" but leaks the temporary: a temporary passed as an argument is not
// disposed in these shapes (the same leak for a declared class,
// `Console.WriteLine(new D())`, and for `Append(new StringBuilder("z"))`). That
// is a separate ownership defect, tracked in #6775; the temporary form
// returns to this suite when it is fixed.
BEZEL_TEST(codegen_console_append_builtin_6753,
           writeline_named_stringbuilder_checked_no_leak_2_1) {
    BEZEL_EXPECT_EQ(checkedOut(prog(
                        "var sb := new StringBuilder(); _ := sb.Append(\"q\"); "
                        "Console.WriteLine(sb);")),
                    std::string("q\n"));
}

// stdlib §2.1: an `Object`-typed local whose initializer is a `StringBuilder`
// renders through its `ToString()`, as the §18.5 hole already does. Issue #6753's
// `Object`-local case (m07/m13 in the probe matrix).
BEZEL_TEST(codegen_console_append_builtin_6753,
           writeline_object_local_stringbuilder_is_its_content_2_1_3_1) {
    BEZEL_EXPECT_EQ(out(prog(
                        "var sb := new StringBuilder(); _ := sb.Append(\"he\"); "
                        "_ := sb.Append(\"llo\"); var o: Object := sb; "
                        "Console.WriteLine(o);")),
                    std::string("hello\n"));
}

// stdlib §2.1 / §3.1 with a nullable receiver that is present: the content.
BEZEL_TEST(codegen_console_append_builtin_6753,
           writeline_nullable_stringbuilder_present_is_its_content_2_1) {
    BEZEL_EXPECT_EQ(out(prog(
                        "var sb := new StringBuilder(); _ := sb.Append(\"q\"); "
                        "var n: StringBuilder? := sb; Console.WriteLine(n);")),
                    std::string("q\n"));
}

// Spec §8.5, v0.340: "a `null` value of a nullable-typed hole renders as the text
// `null` with no dispatch". The stdlib states no null rendering for these
// overloads; the `Object?` null path already prints "null" (interpretation,
// recorded in the commit body), and this row keeps the nullable `StringBuilder?`
// path on the same text. Before the fix it printed an empty line.
BEZEL_TEST(codegen_console_append_builtin_6753,
           writeline_nullable_stringbuilder_null_renders_null_8_5) {
    BEZEL_EXPECT_EQ(out(prog(
                        "var n: StringBuilder? := null; Console.WriteLine(n);")),
                    std::string("null\n"));
}

// Spec §8.5, v0.340: "For classes: the type's qualified name". `Array<T>` declares
// no `ToString` of its own, so the default applies. The module path is the
// stdlib §2.13 one (#3119), which the §18.5 hole already renders.
BEZEL_TEST(codegen_console_append_builtin_6753,
           writeline_static_array_renders_qualified_name_8_5) {
    BEZEL_EXPECT_EQ(out(prog(
                        "var a := new Array<int>(2); Console.WriteLine(a);")),
                    std::string("System.Collections.Generic.Array<int>\n"));
}

// Spec §8.5, v0.340: the same class default for a vtable-less built-in in the
// `System.Threading` module. Pre-fix it printed "" (the struct read as a string).
BEZEL_TEST(codegen_console_append_builtin_6753,
           writeline_static_mutex_renders_qualified_name_8_5) {
    BEZEL_EXPECT_EQ(out(prog(
                        "using var m := new Mutex(); Console.WriteLine(m);",
                        "", "import System.Threading; ")),
                    std::string("System.Threading.Mutex\n"));
}

// ---------------------------------------------------------------------------
// stdlib §3.1: `Append(value: Object)` keeps `value.ToString()`'s text. The
// issue's repro 2, verbatim: the appended part is the argument's content.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_console_append_builtin_6753,
           append_stringbuilder_argument_is_its_content_3_1) {
    BEZEL_EXPECT_EQ(out(prog(
                        "var sb := new StringBuilder(); var sb2 := new StringBuilder(); "
                        "_ := sb2.Append(\"z\"); _ := sb.Append(sb2); "
                        "Console.WriteLine(sb.ToString());")),
                    std::string("z\n"));
}

// stdlib §3.1, read through a multi-part argument: a builder holding "ab" appends
// a builder whose content is two parts, "c" then "d", and the result is "abcd".
// The row reads real data, so a constant-body fix cannot pass it.
BEZEL_TEST(codegen_console_append_builtin_6753,
           append_multipart_stringbuilder_reads_content_3_1) {
    BEZEL_EXPECT_EQ(out(prog(
                        "var sb := new StringBuilder(); _ := sb.Append(\"ab\"); "
                        "var sb2 := new StringBuilder(); _ := sb2.Append(\"c\"); "
                        "_ := sb2.Append(\"d\"); _ := sb.Append(sb2); "
                        "Console.WriteLine(sb.ToString());")),
                    std::string("abcd\n"));
}

// stdlib §3.1: a builder appended to itself doubles its content. The argument is
// read before the append, so the render sees "ab", not the growing builder.
BEZEL_TEST(codegen_console_append_builtin_6753,
           append_self_doubles_content_3_1) {
    BEZEL_EXPECT_EQ(out(prog(
                        "var sb := new StringBuilder(); _ := sb.Append(\"ab\"); "
                        "_ := sb.Append(sb); Console.WriteLine(sb.ToString());")),
                    std::string("abab\n"));
}

// stdlib §3.1: an `Object`-typed local as the `Append` argument renders its
// content, the same narrowing the console takes.
BEZEL_TEST(codegen_console_append_builtin_6753,
           append_object_local_stringbuilder_is_its_content_3_1) {
    BEZEL_EXPECT_EQ(out(prog(
                        "var sb := new StringBuilder(); var sb2 := new StringBuilder(); "
                        "_ := sb2.Append(\"cd\"); var o: Object := sb2; "
                        "_ := sb.Append(o); Console.WriteLine(sb.ToString());")),
                    std::string("cd\n"));
}

// Spec §8.5, v0.340: an `Append` of a class with no `ToString` of its own appends
// the class's qualified name, as the console and the hole do.
BEZEL_TEST(codegen_console_append_builtin_6753,
           append_static_array_renders_qualified_name_8_5) {
    BEZEL_EXPECT_EQ(out(prog(
                        "var sb := new StringBuilder(); _ := sb.Append(\"x\"); "
                        "var a := new Array<int>(2); _ := sb.Append(a); "
                        "Console.WriteLine(sb.ToString());")),
                    std::string("xSystem.Collections.Generic.Array<int>\n"));
}

// ---------------------------------------------------------------------------
// Controls. These routes were already correct and must not move.
// ---------------------------------------------------------------------------

// stdlib §2.1: a `string` in an `Object`-typed local still prints as itself.
BEZEL_TEST(codegen_console_append_builtin_6753,
           control_object_local_string_unchanged_2_1) {
    BEZEL_EXPECT_EQ(out(prog("var o: Object := \"x\"; Console.WriteLine(o);")),
                    std::string("x\n"));
}

// Spec §8.5: a user class's own `ToString` override still dispatches.
BEZEL_TEST(codegen_console_append_builtin_6753,
           control_object_local_user_class_tostring_unchanged_8_5) {
    BEZEL_EXPECT_EQ(out(prog("var o: Object := new D(); Console.WriteLine(o);",
                             kDClass)),
                    std::string("dd\n"));
}

// Spec §8.6: a value-typed `int` boxed into an `Object` local prints its value.
BEZEL_TEST(codegen_console_append_builtin_6753,
           control_object_local_boxed_int_unchanged_8_6) {
    BEZEL_EXPECT_EQ(out(prog("var o: Object := 5; Console.WriteLine(o);")),
                    std::string("5\n"));
}

// Spec §18.5 agrees with stdlib §2.1: the console and the hole print the same text
// for the same `Object`-typed local.
BEZEL_TEST(codegen_console_append_builtin_6753,
           console_and_hole_agree_on_object_local_18_5_2_1) {
    BEZEL_EXPECT_EQ(out(prog(
                        "var sb := new StringBuilder(); _ := sb.Append(\"q\"); "
                        "var o: Object := sb; Console.WriteLine(o); "
                        "Console.WriteLine($\"{o}\");")),
                    std::string("q\nq\n"));
}

// ---------------------------------------------------------------------------
// Ownership. The checked oracle runs these with the sanitizers armed. A leaked
// render or a freed-then-read receiver fails the row, and the 200-iteration loop
// makes a one-shot leak visible.
// ---------------------------------------------------------------------------

// stdlib §2.1: 200 rounds of the three rendering routes on one builder. The
// receiver is read after each render, so an over-release shows as a use-after-
// free, and an un-released render shows as a leak.
BEZEL_TEST(codegen_console_append_builtin_6753,
           checked_loop_console_200x_no_leak_or_use_after_free_2_1) {
    std::string expected;
    for (int i = 0; i < 200; ++i) expected += "hello\nhello\nhello\n";
    BEZEL_EXPECT_EQ(checkedOut(prog(
                        "var sb := new StringBuilder(); _ := sb.Append(\"he\"); "
                        "_ := sb.Append(\"llo\"); var o: Object := sb; "
                        "var n: StringBuilder? := sb; "
                        "for var i in 0..<200 do Console.WriteLine(sb); "
                        "Console.WriteLine(o); Console.WriteLine(n); end; ")),
                    expected);
}

// stdlib §3.1: 200 rounds of `Append` of a builder into a second one that is
// cleared each round. The final content proves the last append read real data.
BEZEL_TEST(codegen_console_append_builtin_6753,
           checked_loop_append_200x_no_leak_3_1) {
    BEZEL_EXPECT_EQ(checkedOut(prog(
                        "var sb := new StringBuilder(); _ := sb.Append(\"he\"); "
                        "_ := sb.Append(\"llo\"); var acc := new StringBuilder(); "
                        "for var i in 0..<200 do _ := acc.Clear(); "
                        "_ := acc.Append(sb); _ := acc.Append(sb); end; "
                        "Console.WriteLine(acc);")),
                    std::string("hellohello\n"));
}
