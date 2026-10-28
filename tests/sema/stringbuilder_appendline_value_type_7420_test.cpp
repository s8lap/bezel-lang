// Spec-derived regression test for issue #7420.
//
// stdlib §3.1 (v0.345) declares EXACTLY TWO `AppendLine` forms:
//
//   public method AppendLine(): StringBuilder;
//   public method AppendLine(value: string): StringBuilder;
//
// — ONE declared value type, and NO `AppendLine(value: Object)` and NO
// per-type overloads (unlike `Append`, which has SEVEN, including
// `Append(value: Object)`).
//
// The `AppendLine/1` row of the ONE declared-signature table
// (`common::kBuiltinParams`) carried NO kind at its value position, so the
// position was never checked against §3.1's declared surface: `sb.AppendLine(5)`,
// `sb.AppendLine('c')` and `sb.AppendLine(true)` all compiled, and all
// APPENDED. A call that matches no declared form is a compile error (§22.6),
// and an `int` (a `char`, a `bool`) reaches a `string` parameter only by an
// EXPLICIT conversion (§5.1) — so each of the three is a compile error.
//
// The rejection rows assert a LOCATED compile-time diagnostic (the house
// `failedAtCompileTimeWithRealLocation` helper) — never an exit code, and
// never the absence of a diagnostic on a program that compiles.
//
// The control rows pin the SAFETY property: the §3.1 surface that must stay
// admitted stays admitted — the SEVEN `Append` overloads (including
// `Append(value: Object)`), `AppendLine(value: string)`, `AppendLine()` and
// `AppendFormat(format: string, args: Array<Object>)`.
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// The diagnostic §3.1's declared value type draws, quoted from the
// `StringBuilder` member checker.
const char kAppendLineValueType[] =
    "the argument `value` to `StringBuilder.AppendLine` (stdlib §3.1 "
    "declares `AppendLine(value: string): StringBuilder`) has a type that is "
    "not implicitly convertible to the target type (§5.1)";

std::string prog(const std::string& stmt) {
    return "module M;\n"
           "import System.Text;\n"
           "class C\n"
           "  public static method Main(): int\n"
           "    var sb := new StringBuilder();\n"
           "    sb.Append(\"hello\");\n"
           "    " + stmt + "\n"
           "    Console.WriteLine(sb.ToString());\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

bool hasError(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

// A rejection row: the program must be REJECTED at compile time, with the
// named diagnostic, at a real source location.
void expectRejected(const std::string& stmt, const std::string& needle) {
    auto r = runAot(prog(stmt));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r, needle));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
}

// A control row: the program must compile, run, and print exactly `out`.
// The oracle is the AOT path (`runAot`), the same one the sibling §3.1
// surface test (`stringbuilder_member_surface_7207`) uses for its control
// rows: the `AppendFormat` control allocates an `Array<Object>`, and the
// compiler does not free that array at scope end — a pre-existing gap that
// is not this fix's business, and the reason the control rows are not on
// the ASan leg.
void expectStdout(const std::string& body, const std::string& out) {
    auto r = runAot("module M;\n"
                    "import System.Text;\n"
                    "class C\n"
                    "  public static method Main(): int\n"
                    "    " + body + "\n"
                    "    return 0;\n"
                    "  end Main;\n"
                    "end C;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

}  // namespace

// ── §3.1 / §22.6 / §5.1: the VALUE position is checked ─────────────────────

// stdlib §3.1 (v0.345): `AppendLine(value: string)` is the only declared
// value form. An `int` reaches a `string` parameter only by an explicit
// conversion (§5.1), so `sb.AppendLine(5)` matches no declared form.
BEZEL_TEST(stringbuilder_appendline_value_type_7420, int_value_rejected) {
    expectRejected("sb.AppendLine(5);", kAppendLineValueType);
}
// §3.1 declares NO `AppendLine(value: char)` — `Append` has a `char`
// overload, `AppendLine` does not.
BEZEL_TEST(stringbuilder_appendline_value_type_7420, char_value_rejected) {
    expectRejected("sb.AppendLine('c');", kAppendLineValueType);
}
// §3.1 declares NO `AppendLine(value: bool)` either.
BEZEL_TEST(stringbuilder_appendline_value_type_7420, bool_value_rejected) {
    expectRejected("sb.AppendLine(true);", kAppendLineValueType);
}
// §5.1: "any type to `Object`" is a REFERENCE conversion — it does not make
// an `Object` a `string`. §3.1 declares no `AppendLine(value: Object)`.
BEZEL_TEST(stringbuilder_appendline_value_type_7420, object_value_rejected) {
    expectRejected("sb.AppendLine(new Object());", kAppendLineValueType);
}

// ── CONTROLS: the §3.1 surface that must stay admitted ────────────────────

// §3.1's `AppendLine(value: string)` — the declared form.
BEZEL_TEST(stringbuilder_appendline_value_type_7420, string_value_admitted) {
    expectStdout("var sb := new StringBuilder(); sb.AppendLine(\"x\"); "
                 "Console.WriteLine(sb.ToString());",
                 "x\n\n");  // the terminator, then WriteLine's own
}
// §3.1's `AppendLine()` — the declared no-argument form.
BEZEL_TEST(stringbuilder_appendline_value_type_7420, no_argument_admitted) {
    expectStdout("var sb := new StringBuilder(); sb.AppendLine(); "
                 "Console.WriteLine(sb.Length);",
                 "1\n");  // the terminator alone
}
// §3.1 declares SEVEN `Append` overloads (`string` / `char` / `Object` /
// `bool` / the numeric primitives / `ReadOnlyStringSpan`). The value
// position of `Append` therefore carries NO type opinion, and every one of
// them must still be admitted.
BEZEL_TEST(stringbuilder_appendline_value_type_7420,
           append_overloads_still_admitted) {
    expectStdout("var sb := new StringBuilder(); sb.Append(5); sb.Append('c'); "
                 "sb.Append(true); sb.Append(\"s\"); "
                 "Console.WriteLine(sb.ToString());",
                 "5cTrues\n");
}
// §3.1 declares `AppendFormat(format: string, args: Array<Object>)` — it
// must still be admitted, and its `args` must still be an `Array<Object>`.
BEZEL_TEST(stringbuilder_appendline_value_type_7420,
           appendformat_still_admitted) {
    expectStdout("var args: Array<Object> := [1]; "
                 "var sb := new StringBuilder(); sb.AppendFormat(\"{0}\", args); "
                 "Console.WriteLine(sb.ToString());",
                 "1\n");
}
// §3.1 declares `Insert(index: int, value: string)` — the sibling member
// whose value position the fix does NOT touch (its own dedicated check,
// #6807, owns that position).
BEZEL_TEST(stringbuilder_appendline_value_type_7420,
           insert_string_value_still_admitted) {
    expectStdout("var sb := new StringBuilder(); sb.Append(\"ab\"); "
                 "sb.Insert(1, \"X\"); Console.WriteLine(sb.ToString());",
                 "aXb\n");
}
