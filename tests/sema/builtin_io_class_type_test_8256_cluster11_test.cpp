// Regression test for issue #8256 (spec §12.7, stdlib §2.1 / §7.1 / §7.4, v0.355).
//
// THE DEFECT. §12.7: "`is T` tests whether a value is an instance of `T` and
// produces `bool`." stdlib §1 *Extensibility* names the built-in classes that
// admit a user-declared subclass:
//
//   "A class of this library admits a user-declared subclass only where its
//    section says so: `Encoding` (§3.2), `Stream` (§7.1), `TextReader` and
//    `TextWriter` (§7.4), and the exception types."
//
// so a `class W: TextWriter` IS an instance of `TextWriter` (§12.7) and a type
// test over one of those heads must ANSWER. Before the fix the three heads had
// NO runtime type identity at all: a type test whose target was one of them
// reached the #8000 bail, was DROPPED, and the statement that held it emitted
// NOTHING (not even the line terminator) — the compiler printed only the
// #3079 "the argument of this `Console.WriteLine` failed to lower" warning.
// The three heads' own VALUES were in the same state: `Console.Out` is the
// standard stream's i32 DESCRIPTOR (not a reference), so the pointer-based
// `is Object` arm bailed on it and the test was dropped as well.
//
// THE ROWS below are the issue's own population (its table's rows 1, 2, 4, 5,
// 6, 7 and 8), each asserting the EXACT stdout. The issue's row 3
// (`Console.Out as Object` must be non-null) is NOT asserted here: it is the
// CARRIER half of the family — a `TextWriter` value has no `Object`
// representation at all — and it is issue #8243's subject, not this one. A
// row that pins an unfixed defect is not a regression test.
//
// THE FIX (three files, declared in the handback):
//   * `src/sema/ExprTypecheck.cpp` — the `is` / `is not` forms over a
//     compiler-known I/O class head are ADMITTED (the `as` forms and the
//     typed `case` binding keep the #7792 rejection: they must produce a
//     VALUE of the target type).
//   * `src/codegen/LlvmBackend_Setup.cpp` — the three heads get a runtime
//     type descriptor of their own (`btd$TextWriter` / `btd$TextReader` /
//     `btd$Stream`), appended to the runtime type chain of every user class
//     that derives from them, so `emitIsA` answers `o is TextWriter`.
//   * `src/codegen/LlvmBackend_Layout.cpp` — a type test whose OPERAND is an
//     I/O class VALUE (`Console.Out`, `Console.Error`, `Console.In`, or a
//     `TextWriter`-typed local) is answered STATICALLY: the stream is an
//     instance of its own head and of `Object` (§8.1) and of no other class
//     (§12.7).
//
// THE CONTROLS at the end pin that the fix did not widen: a user class's own
// `is` / `as` is unchanged, and the `as TextWriter` rejection (#7792) is still
// a rejection.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// The issue's own prelude, verbatim: a user `W : TextWriter` (with an
// interface beside it), a user `R : TextReader`, and an unrelated class `V`.
// The generic carrier `G.F<T>(o: Object): bool` is the issue's row 4.
static const char* kPrelude = R"bzl(
module M;
interface I end I;
class W: TextWriter, I
 public constructor() end constructor;
 public override method Write(value: string) Console.Write("[" + value + "]"); end Write;
 public override method Write(value: char) Console.Write("<c>"); end Write;
 public override method Flush() end Flush;
end W;
class R: TextReader
 public constructor() end constructor;
 public override method Read(): int return -1; end Read;
 public override method ReadLine(): string? return null; end ReadLine;
end R;
class V public constructor() end constructor; end V;
class G public static method F<T>(o: Object): bool return o is T; end F; end G;
)bzl";

// The issue's own prelude WITHOUT the generic carrier — for the rows that do
// not need it.
static const char* kPreludeNoGeneric = R"bzl(
module M;
interface I end I;
class W: TextWriter, I
 public constructor() end constructor;
 public override method Write(value: string) Console.Write("[" + value + "]"); end Write;
 public override method Write(value: char) Console.Write("<c>"); end Write;
 public override method Flush() end Flush;
end W;
class R: TextReader
 public constructor() end constructor;
 public override method Read(): int return -1; end Read;
 public override method ReadLine(): string? return null; end ReadLine;
end R;
class V public constructor() end constructor; end V;
)bzl";

// Run a `Main` body under the issue's prelude.
static bezel::test::JitResult runPrelude(const std::string& body) {
    return runAot(std::string(kPrelude) +
                  "class P public static method Main() " + body +
                  " end Main; end P;");
}

// Run a `Main` body under the prelude WITHOUT the generic carrier.
static bezel::test::JitResult runPreludeNoGeneric(const std::string& body) {
    return runAot(std::string(kPreludeNoGeneric) +
                  "class P public static method Main() " + body +
                  " end Main; end P;");
}

// ISSUE ROW 1. `Console.WriteLine(Console.Out is Object);` must print `True`.
//
// PRE-FIX: the line was MISSING — the `is Object` arm found a non-pointer
// operand (the stream's i32 descriptor) and bailed, so the whole statement
// emitted nothing and the compiler printed the #3079 warning. The `Console.In`
// and `Console.Out` streams are class VALUES (§7.4's `TextWriter` is a
// class), so §8.1's "every class value is an `Object`" answers TRUE.
BEZEL_TEST(sema_builtin_io_class_type_test_8256,
           console_out_is_object_is_true_12_7) {
    auto r = runPrelude(
        "Console.WriteLine(Console.Out is Object); Console.WriteLine(\"end\");");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\nend\n"));
}

// ISSUE ROW 2. `Console.WriteLine(Console.Out is W);` must print `False` — the
// standard stream is an instance of `TextWriter`, NOT of a user subclass of it
// (§12.7: the test names the value's own class).
BEZEL_TEST(sema_builtin_io_class_type_test_8256,
           console_out_is_a_user_subclass_is_false_12_7) {
    auto r = runPrelude(
        "Console.WriteLine(Console.Out is W); Console.WriteLine(\"end\");");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("False\nend\n"));
}

// The `is not` spelling (§12.7 answers it with the same answer, inverted) and
// the test that names the operand's OWN head. The `is not` form is the one
// #7991's own regression row uses, so it must keep working.
BEZEL_TEST(sema_builtin_io_class_type_test_8256,
           the_is_not_spelling_and_the_own_head_answer_12_7) {
    auto r = runPrelude(
        "Console.WriteLine(Console.Out is not W); "
        "Console.WriteLine(Console.Out is not Object); "
        "Console.WriteLine(Console.Out is TextWriter); "
        "Console.WriteLine(\"end\");");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\nFalse\nTrue\nend\n"));
}

// ISSUE ROW 4. `F<T>(o: Object): bool => o is T`, called at `T = TextWriter`
// with an `Object` carrier holding a `W : TextWriter`, must print `True`.
//
// PRE-FIX: `False` — the target `TextWriter` had no runtime descriptor, so
// the test was dropped and the return value degraded to a zero/null return
// (the #2491 warning). `F<W>` was already correct and must stay correct: the
// fix must not change a user class's own answer.
BEZEL_TEST(sema_builtin_io_class_type_test_8256,
           a_generic_type_test_at_textwriter_answers_true_12_7) {
    auto r = runPrelude(
        "var w := new W(); var o: Object := w; "
        "Console.WriteLine(G.F<TextWriter>(o)); "
        "Console.WriteLine(G.F<W>(o));");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\nTrue\n"));
}

// ISSUE ROW 5. `var o: Object := new R(); Console.WriteLine(o is TextReader);`
// must print `True` — a `class R : TextReader` IS an instance of
// `TextReader` (§12.7).
//
// PRE-FIX: the line was MISSING — the `TextReader` head had no runtime
// descriptor, so the test was dropped (#3079).
BEZEL_TEST(sema_builtin_io_class_type_test_8256,
           an_object_holding_a_textreader_is_a_textreader_12_7) {
    auto r = runPrelude(
        "var o: Object := new R(); "
        "Console.WriteLine(o is TextReader); "
        "Console.WriteLine(o is R);");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\nTrue\n"));
}

// ISSUE ROW 6. `var r := o as TextReader;` over the SAME carrier must yield a
// NON-NULL value — the cast succeeds, because the value IS a `TextReader`.
//
// PRE-FIX: the local was DEGRADED to a zero-initialised value (the #2434
// warning), so `r = null` printed `True` — a cast that silently produced
// null.
BEZEL_TEST(sema_builtin_io_class_type_test_8256,
           a_cast_to_textreader_yields_a_non_null_value_12_7) {
    auto r = runPrelude(
        "var o: Object := new R(); "
        "Console.WriteLine(o is TextReader); "
        "Console.WriteLine(o is R); "
        "var r := o as TextReader; "
        "Console.WriteLine(r = null);");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\nTrue\nFalse\n"));
}

// ISSUE ROW 7. `Console.WriteLine(Console.In is TextReader);` must print
// `True` — stdlib §2.1 declares `Console.In` as `static readonly property In:
// TextReader`, and §7.4's `TextReader` is a class, so the standard input
// stream IS an instance of `TextReader` (§12.7).
//
// PRE-FIX: the line was MISSING — the test was dropped (#3079).
BEZEL_TEST(sema_builtin_io_class_type_test_8256,
           console_in_is_a_textreader_12_7) {
    auto r = runPrelude(
        "Console.WriteLine(Console.In is TextReader); "
        "Console.WriteLine(\"end\");");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\nend\n"));
}

// ISSUE ROW 8. `var o: Object := new V(); Console.WriteLine(o is Stream);`
// must print `False` — §7.4's `TextReader` / `TextWriter` do NOT derive from
// §7.1's `Stream`, so an unrelated class is an instance of neither.
BEZEL_TEST(sema_builtin_io_class_type_test_8256,
           an_unrelated_class_is_not_a_stream_12_7) {
    auto r = runPrelude(
        "var o: Object := new V(); "
        "Console.WriteLine(o is Stream); "
        "Console.WriteLine(\"end\");");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("False\nend\n"));
}

// CONTROL 1. A USER CLASS's own `is` / `as` is untouched: the fix is scoped to
// the three compiler-known I/O class heads, and it must not change how an
// ordinary class answers.
BEZEL_TEST(sema_builtin_io_class_type_test_8256,
           a_user_class_is_and_as_are_unchanged_12_7) {
    auto r = runAot(
        "module M; "
        "class Widget public constructor() end constructor; end Widget; "
        "class P public static method Main() "
        "var o: Object := new Widget(); "
        "Console.WriteLine(o is Widget); "
        "var w := o as Widget; "
        "Console.WriteLine(w = null); "
        "Console.WriteLine(\"end\"); "
        "end Main; end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\nFalse\nend\n"));
}

// CONTROL 2. The `as TextWriter` REJECTION (#7792) is still a rejection: the
// `as` form must produce a VALUE of the target type, and an `Object` has no
// representation as the standard stream's i32 descriptor.
//
// The fix admits the `is` / `is not` FORMS only; a `TextWriter?` (the
// `{i8, i32}` nullable carrier) and the `as` form keep their rejection.
BEZEL_TEST(sema_builtin_io_class_type_test_8256,
           the_as_textwriter_rejection_is_kept_7_4) {
    auto r = runAot(
        "module M; "
        "class P public static method Main() "
        "var o: Object := Console.Error; "
        "var t := o as TextWriter; "
        "Console.WriteLine(\"end\"); "
        "end Main; end P;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "a type test or cast into the built-in `TextWriter` (`as`) is not "
           "implemented by this compiler"));
}

}  // namespace
