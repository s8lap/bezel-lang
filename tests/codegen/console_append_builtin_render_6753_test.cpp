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

// #6778 (§7.1, §22.6, v0.340): true iff `src` is rejected with the §7 text at
// the argument of the first `call` (the position just past its `(`).
bool rejectsNullableConsoleArg(const std::string& src, const std::string& call,
                               const std::string& callee) {
    const std::size_t at = src.find(call);
    if (at == std::string::npos) return false;
    const std::size_t arg = at + call.find('(') + 1;
    std::size_t line = 1, lineStart = 0;
    for (std::size_t i = 0; i < arg; ++i)
        if (src[i] == '\n') { ++line; lineStart = i + 1; }
    const std::string want =
        ":" + std::to_string(line) + ":" + std::to_string(arg - lineStart + 1) +
        ": the argument to `" + callee +
        "` is nullable but the target type is non-nullable (\xC2\xA7" "7)";
    auto r = runAot(src);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(want) != std::string::npos)
            return true;
    return false;
}

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
//
// Owner ruling on #6778 (§7.1, §22.6, v0.340; stdlib §2.1): `WriteLine`'s
// `Object` parameter is non-nullable, so this un-narrowed `StringBuilder?` is
// now a compile error rather than a rendered "null"; the §18.5 hole is the
// remedy that renders `null`.
BEZEL_TEST(codegen_console_append_builtin_6753,
           writeline_nullable_stringbuilder_null_rejected_6778_7_1) {
    BEZEL_EXPECT_TRUE(rejectsNullableConsoleArg(
        prog("var n: StringBuilder? := null; Console.WriteLine(n);"),
        "Console.WriteLine(n)", "Console.WriteLine"));
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

// ---------------------------------------------------------------------------
// The narrowing declines when the slot is written outside a statement-level
// assignment (review follow-up to #6753). An `Object` local narrowed through
// its `StringBuilder` initializer is valid only while the slot still holds
// that builder. A write from a lambda body (spec §7.4, v0.340: "mutably
// captured — assigned, or passed as `ref`/`out`, anywhere inside any lambda")
// or through a `ref`/`out` argument (spec §22.3, v0.340) replaces it with a
// string. MEASURED at ef6e119d: each row below hands that string to the
// builder renderer and dies with SEGV in `bz_strbuilder_to_string`. stdlib
// §2.1 (v0.340): "The `Object` overloads invoke `ToString()`", and spec §8.5
// (v0.340) gives a `string` its own text, so the expected output is the
// string that was written.
// ---------------------------------------------------------------------------

namespace {
// `Set(ref x)` / `SetOut(out x)` store a string into the caller's `Object`.
const char* kSetters =
    "public class H "
    "public static method Set(ref x: Object): () x := \"viaref\"; end Set; "
    "public static method SetOut(out x: Object): () x := \"viaout\"; end SetOut; "
    "end H; ";
}  // namespace

// §7.4 / stdlib §2.1 / stdlib §3.1: the reviewer's b03 repro. A lambda assigns
// the captured local; WriteLine and Append both render the new string.
BEZEL_TEST(codegen_console_append_builtin_6753,
           closure_written_object_local_renders_its_string_7_4_2_1) {
    BEZEL_EXPECT_EQ(checkedOut(prog(
                        "var sb := new StringBuilder(); _ := sb.Append(\"sbtext\"); "
                        "var o: Object := sb; "
                        "var f := lambda() => do o := \"viaclosure\"; end; f(); "
                        "Console.WriteLine(o); var t := new StringBuilder(); "
                        "_ := t.Append(o); Console.WriteLine(t.ToString());")),
                    std::string("viaclosure\nviaclosure\n"));
}

// §22.3 / stdlib §2.1 / stdlib §3.1: the reviewer's b04 repro, with Append added.
BEZEL_TEST(codegen_console_append_builtin_6753,
           ref_written_object_local_renders_its_string_22_3_2_1) {
    BEZEL_EXPECT_EQ(checkedOut(prog(
                        "var sb := new StringBuilder(); _ := sb.Append(\"sbtext\"); "
                        "var o: Object := sb; H.Set(ref o); "
                        "Console.WriteLine(o); var t := new StringBuilder(); "
                        "_ := t.Append(o); Console.WriteLine(t.ToString());",
                        kSetters)),
                    std::string("viaref\nviaref\n"));
}

// §22.3 / stdlib §2.1 / stdlib §3.1: the `out` twin.
BEZEL_TEST(codegen_console_append_builtin_6753,
           out_written_object_local_renders_its_string_22_3_2_1) {
    BEZEL_EXPECT_EQ(checkedOut(prog(
                        "var sb := new StringBuilder(); _ := sb.Append(\"sbtext\"); "
                        "var o: Object := sb; H.SetOut(out o); "
                        "Console.WriteLine(o); var t := new StringBuilder(); "
                        "_ := t.Append(o); Console.WriteLine(t.ToString());",
                        kSetters)),
                    std::string("viaout\nviaout\n"));
}

// §18.5 / §7.4: the interpolation hole had the same hole before #6753 (it is the
// narrowing's original user); the same decline fixes it.
BEZEL_TEST(codegen_console_append_builtin_6753,
           closure_written_object_local_interpolates_its_string_18_5_7_4) {
    BEZEL_EXPECT_EQ(checkedOut(prog(
                        "var sb := new StringBuilder(); _ := sb.Append(\"sbtext\"); "
                        "var o: Object := sb; "
                        "var f := lambda() => do o := \"viaclosure\"; end; f(); "
                        "Console.WriteLine($\"[{o}]\");")),
                    std::string("[viaclosure]\n"));
}

// §18.5 / §22.3: the `ref` twin of the interpolation row.
BEZEL_TEST(codegen_console_append_builtin_6753,
           ref_written_object_local_interpolates_its_string_18_5_22_3) {
    BEZEL_EXPECT_EQ(checkedOut(prog(
                        "var sb := new StringBuilder(); _ := sb.Append(\"sbtext\"); "
                        "var o: Object := sb; H.Set(ref o); "
                        "Console.WriteLine($\"[{o}]\");",
                        kSetters)),
                    std::string("[viaref]\n"));
}

// §8.5 / stdlib §2.1: a local declared AND reassigned inside a lambda body. The
// statement-level set walks only the enclosing function's statements, so it
// never saw this assignment.
BEZEL_TEST(codegen_console_append_builtin_6753,
           lambda_body_local_reassigned_renders_its_string_8_5_2_1) {
    BEZEL_EXPECT_EQ(checkedOut(prog(
                        "var f := lambda() => do var sb := new StringBuilder(); "
                        "_ := sb.Append(\"sbtext\"); var o: Object := sb; "
                        "o := \"inlam\"; Console.WriteLine(o); "
                        "Console.WriteLine($\"[{o}]\"); end; f();")),
                    std::string("inlam\n[inlam]\n"));
}

// CONTROL (§7.4 / stdlib §2.1 / stdlib §3.1): a lambda that only READS the local
// is not a write, so the narrowing still applies and every route prints the
// builder's content.
BEZEL_TEST(codegen_console_append_builtin_6753,
           control_closure_reading_object_local_still_renders_builder_7_4_2_1) {
    BEZEL_EXPECT_EQ(checkedOut(prog(
                        "var sb := new StringBuilder(); _ := sb.Append(\"sbtext\"); "
                        "var o: Object := sb; "
                        "var f := lambda() => do Console.WriteLine(o); end; f(); "
                        "Console.WriteLine(o); var t := new StringBuilder(); "
                        "_ := t.Append(o); Console.WriteLine(t.ToString()); "
                        "Console.WriteLine($\"[{o}]\");")),
                    std::string("sbtext\nsbtext\nsbtext\n[sbtext]\n"));
}

// §17.15 / §7.4 / stdlib §2.1: the write sits in a lambda bound inside a binding
// atom, which the slot-write walk did not descend into before this fix. This row
// uses the plain oracle: a closure bound in an atom clause leaks its env with no
// `Object` involved (MEASURED: `(var h := lambda() => do s := "y"; end; h())` over
// a `string` local), which is #2123 item 3. The stdout comparison still pins the
// crash: at ef6e119d the program dies with SEGV and prints nothing.
BEZEL_TEST(codegen_console_append_builtin_6753,
           bind_atom_closure_written_object_local_renders_its_string_17_15_7_4) {
    BEZEL_EXPECT_EQ(out(prog(
                        "var sb := new StringBuilder(); _ := sb.Append(\"sbtext\"); "
                        "var o: Object := sb; "
                        "_ := (var h := lambda() => do o := \"viabind\"; end; h()); "
                        "Console.WriteLine(o); Console.WriteLine($\"[{o}]\");")),
                    std::string("viabind\n[viabind]\n"));
}

// ---------------------------------------------------------------------------
// Fix-forward #6753 (second review): the narrowing must FAIL CLOSED. Before
// this change the slot-write scan ran over the body `collectBoxReleaseEligible`
// last walked, and a body it never walked (an expression-bodied member, a
// constructor, a static constructor, an async method, an iterator) answered
// "not written", so a local the body DID reassign, or wrote through a lambda or
// a `ref` argument, was still narrowed to its initializer's `StringBuilder` and
// its string was handed to the builder renderer (MEASURED on 66cd27fc, ASan:
// SEGV in every row below that is not marked CONTROL). The narrowing now needs
// positive proof: the scan root is the body of the function being emitted, the
// scan must reach the local's own declaration, and it must find no write.
//
// Oracles. Rows whose program is leak-clean at the base (f2f81408, MEASURED
// under ASan with `detect_leaks=1`) use the checked oracle. Five rows use the
// plain oracle because their program leaks AT THE BASE for a reason that is not
// this path: a `StringBuilder` local declared inside a lambda body is never
// released (#6061; 64 + 512 B from `bz_array_alloc`), and a lambda-valued local
// in a constructor body leaks its closure (#6766; 64 + 8/16 B). The exact
// stdout still pins the crash: on 66cd27fc each of those programs dies with
// SEGV and prints nothing.
// ---------------------------------------------------------------------------
namespace {
// `o` is an `Object` local whose initializer is a `StringBuilder` holding
// "sbtext": the shape every row below narrows (or must refuse to narrow).
const char* kObjLocal =
    "var sb := new StringBuilder(); _ := sb.Append(\"sbtext\"); "
    "var o: Object := sb; ";
// A `ref` setter inside the class under test.
const char* kSet = "  static method Set(ref x: Object): () x := \"viaref\"; end Set; ";
}  // namespace

// §22.2 / §8.5 / stdlib §2.1: an EXPRESSION-BODIED method never reached the
// statement collector, so its lambda-body local answered "not written".
BEZEL_TEST(codegen_console_append_builtin_6753,
           arrow_method_lambda_local_reassigned_renders_its_string_22_2_8_5) {
    BEZEL_EXPECT_EQ(out(std::string(
                        "module M; class C "
                        "  static method Call(f: () -> ()): int f(); return 0; end Call; "
                        "  static method Run(): int => Call(lambda() => do ") + kObjLocal +
                        "o := \"inlam\"; Console.WriteLine(o); end); "
                        "  public static method Main(): int return Run(); end Main; end C;"),
                    std::string("inlam\n"));
}

// §22.8 / §8.5 / stdlib §2.1: a FIELD INITIALIZER's lambda is lowered with no
// body of its own ever scanned; its local is now unprovable and declines.
BEZEL_TEST(codegen_console_append_builtin_6753,
           field_initializer_lambda_local_reassigned_renders_its_string_22_8_8_5) {
    BEZEL_EXPECT_EQ(out(std::string(
                        "module M; class C "
                        "  private _f: () -> () := lambda() => do ") + kObjLocal +
                        "o := \"infield\"; Console.WriteLine(o); end; "
                        "  public method Run(): () _f(); end Run; "
                        "  public static method Main(): int var c := new C(); c.Run(); "
                        "return 0; end Main; end C;"),
                    std::string("infield\n"));
}

// §23.2 / §8.5 / stdlib §2.1: the expression-bodied PROPERTY twin of the
// arrow-method row.
BEZEL_TEST(codegen_console_append_builtin_6753,
           arrow_property_lambda_local_reassigned_renders_its_string_23_2_8_5) {
    BEZEL_EXPECT_EQ(out(std::string(
                        "module M; class C "
                        "  static method Call(f: () -> ()): int f(); return 0; end Call; "
                        "  public property P: int => Call(lambda() => do ") + kObjLocal +
                        "o := \"inprop\"; Console.WriteLine(o); end); "
                        "  public static method Main(): int var c := new C(); return c.P; "
                        "end Main; end C;"),
                    std::string("inprop\n"));
}

// §30.1 / §22.3 / stdlib §2.1: an ASYNC method's body is lowered by the state
// machine emitter, which never ran the collector; the `ref` write was unseen.
BEZEL_TEST(codegen_console_append_builtin_6753,
           async_method_ref_written_object_local_renders_its_string_30_1_22_3) {
    BEZEL_EXPECT_EQ(checkedOut(std::string("module M; class C ") + kSet +
                        "  static async method Run(): Task<int> " + kObjLocal +
                        "Set(ref o); Console.WriteLine(o); return 0; end Run; "
                        "  public static async method Main(): Task<int> return await Run(); "
                        "end Main; end C;"),
                    std::string("viaref\n"));
}

// §29.3 / §22.3 / stdlib §2.1: the ITERATOR twin (the builder is `using var`
// because an iterator body may not drop an owned local).
BEZEL_TEST(codegen_console_append_builtin_6753,
           iterator_ref_written_object_local_renders_its_string_29_3_22_3) {
    BEZEL_EXPECT_EQ(checkedOut(std::string("module M; class C ") + kSet +
                        "  static method Gen(): IEnumerable<int> "
                        "using var sb := new StringBuilder(); _ := sb.Append(\"sbtext\"); "
                        "var o: Object := sb; Set(ref o); Console.WriteLine(o); "
                        "yield return 1; end Gen; "
                        "  public static method Main(): int "
                        "for var x in Gen() do Console.WriteLine(x); end for; "
                        "return 0; end Main; end C;"),
                    std::string("viaref\n1\n"));
}

// §22.12 / §27.5 / stdlib §2.1: a STATIC CONSTRUCTOR body with a closure write.
BEZEL_TEST(codegen_console_append_builtin_6753,
           static_constructor_closure_written_object_local_renders_its_string_22_12_27_5) {
    BEZEL_EXPECT_EQ(out(std::string(
                        "module M; class C "
                        "  static constructor() ") + kObjLocal +
                        "var f := lambda() => do o := \"instatic\"; end; f(); "
                        "Console.WriteLine(o); end constructor; "
                        "  public static method Main(): int return 0; end Main; end C;"),
                    std::string("instatic\n"));
}

// §22.7 / §27.5 / stdlib §2.1: a CONSTRUCTOR body with a closure write.
BEZEL_TEST(codegen_console_append_builtin_6753,
           constructor_closure_written_object_local_renders_its_string_22_7_27_5) {
    BEZEL_EXPECT_EQ(out(std::string(
                        "module M; class C "
                        "  public constructor() ") + kObjLocal +
                        "var f := lambda() => do o := \"inctor\"; end; f(); "
                        "Console.WriteLine(o); end constructor; "
                        "  public static method Main(): int var c := new C(); return 0; "
                        "end Main; end C;"),
                    std::string("inctor\n"));
}

// §22.7 / §8.5 / stdlib §2.1: a PLAIN reassignment in a constructor body. The
// statement-level set is filled by the same collector, so it was empty too.
BEZEL_TEST(codegen_console_append_builtin_6753,
           constructor_reassigned_object_local_renders_its_string_22_7_8_5) {
    BEZEL_EXPECT_EQ(checkedOut(std::string(
                        "module M; class C "
                        "  public constructor() ") + kObjLocal +
                        "o := \"reassigned\"; Console.WriteLine(o); end constructor; "
                        "  public static method Main(): int var c := new C(); return 0; "
                        "end Main; end C;"),
                    std::string("reassigned\n"));
}

// §30.1 / §8.5 / stdlib §2.1: a plain reassignment in an async method body.
BEZEL_TEST(codegen_console_append_builtin_6753,
           async_method_reassigned_object_local_renders_its_string_30_1_8_5) {
    BEZEL_EXPECT_EQ(checkedOut(std::string(
                        "module M; class C "
                        "  static async method Run(): Task<int> ") + kObjLocal +
                        "o := \"reassigned\"; Console.WriteLine(o); return 0; end Run; "
                        "  public static async method Main(): Task<int> return await Run(); "
                        "end Main; end C;"),
                    std::string("reassigned\n"));
}

// §30.1 / §22.3 / stdlib §3.1: the `Append(o)` route in an async method.
BEZEL_TEST(codegen_console_append_builtin_6753,
           async_method_ref_written_object_local_appends_its_string_30_1_3_1) {
    BEZEL_EXPECT_EQ(checkedOut(std::string("module M; class C ") + kSet +
                        "  static async method Run(): Task<int> " + kObjLocal +
                        "Set(ref o); var t := new StringBuilder(); _ := t.Append(o); "
                        "Console.WriteLine(t.ToString()); return 0; end Run; "
                        "  public static async method Main(): Task<int> return await Run(); "
                        "end Main; end C;"),
                    std::string("viaref\n"));
}

// §30.1 / §22.3 / §18.5: the interpolation hole in an async method. MEASURED:
// this program also dies with SEGV at the base (f2f81408), where the hole was
// the narrowing's only user and no slot-write scan existed at all.
BEZEL_TEST(codegen_console_append_builtin_6753,
           async_method_ref_written_object_local_interpolates_its_string_30_1_18_5) {
    BEZEL_EXPECT_EQ(checkedOut(std::string("module M; class C ") + kSet +
                        "  static async method Run(): Task<int> " + kObjLocal +
                        "Set(ref o); Console.WriteLine($\"[{o}]\"); return 0; end Run; "
                        "  public static async method Main(): Task<int> return await Run(); "
                        "end Main; end C;"),
                    std::string("[viaref]\n"));
}

// CONTROL (§30.1 / stdlib §2.1): an async body whose local is NEVER written
// still narrows — the scan root is set before the async routing, so the state
// machine's body is scanned and the declaration is seen.
BEZEL_TEST(codegen_console_append_builtin_6753,
           control_async_method_unwritten_object_local_still_renders_builder_30_1_2_1) {
    BEZEL_EXPECT_EQ(checkedOut(std::string(
                        "module M; class C "
                        "  static async method Run(): Task<int> ") + kObjLocal +
                        "Console.WriteLine(o); return 0; end Run; "
                        "  public static async method Main(): Task<int> return await Run(); "
                        "end Main; end C;"),
                    std::string("sbtext\n"));
}

// §14.1 / stdlib §2.1: a generic specialization is built mid-emission of its
// first caller. Its own body is scanned while it is emitted (written local in
// `G` declines, unwritten local in `U` narrows, at both instantiations), and the
// caller's scan root is restored afterwards, so `Main`'s own unwritten local
// still narrows when `Main` resumes. MEASURED on 66cd27fc: the specialized `G`
// still narrowed its reassigned local (SEGV, the fault address spelling "ingen"),
// so this row fails without the fix too; the `U` and `Main` renders are the
// controls for what fail-closed must keep.
BEZEL_TEST(codegen_console_append_builtin_6753,
           control_generic_specializations_keep_callers_object_local_rendering_14_1_2_1) {
    BEZEL_EXPECT_EQ(checkedOut(std::string(
                        "module M; class C "
                        "  static method G<T>(x: T): int ") + kObjLocal +
                        "o := \"ingen\"; Console.WriteLine(o); return 0; end G; "
                        "  static method U<T>(x: T): int " + kObjLocal +
                        "Console.WriteLine(o); return 0; end U; "
                        "  public static method Main(): int "
                        "var sb0 := new StringBuilder(); _ := sb0.Append(\"main\"); "
                        "var m: Object := sb0; "
                        "_ := G<int>(1); _ := G<string>(\"s\"); _ := U<int>(1); _ := U<string>(\"s\"); "
                        "Console.WriteLine(m); return 0; end Main; end C;"),
                    std::string("ingen\ningen\nsbtext\nsbtext\nmain\n"));
}

// CONTROL (§22.2 / §17.15 / stdlib §2.1): an expression body that is a binding
// atom declares the local in a clause; the scan now covers expression bodies, so
// the declaration is seen and the (single-assignment) local still narrows.
BEZEL_TEST(codegen_console_append_builtin_6753,
           control_arrow_method_bind_atom_object_local_still_renders_builder_22_2_17_15) {
    BEZEL_EXPECT_EQ(checkedOut(
                        "module M; class C "
                        "  static method R(): () => (var sb := new StringBuilder(\"sbtext\"); "
                        "var o: Object := sb; Console.WriteLine(o)); "
                        "  public static method Main(): int R(); return 0; end Main; end C;"),
                    std::string("sbtext\n"));
}
