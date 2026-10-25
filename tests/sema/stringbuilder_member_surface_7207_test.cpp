// Spec-derived regression test for issue #7207.
//
// stdlib §3.1 (v0.343) declares the whole member surface of the built-in
// `StringBuilder` — and `StringBuilder` has NO user declaration, so that
// listing IS its surface (the compiler's own #2457 diagnostic says so where
// it rejects a member that is not on it). Before the fix nothing checked a
// call against that surface: the built-in members have no MethodSymbol, so
// §22.5's argument-count check never ran for them and the member was typed
// by NAME alone.
//
//   * a call at an arity §3.1 declares no form for was a SILENT NO-OP: the
//     call did nothing and the program printed a plausible wrong value
//     (`sb.Replace()` printed "hello" — the declared form replaced nothing);
//   * an argument of a type the declared parameter does not take reached
//     codegen as MALFORMED IR (`sb.Replace('l','L')` and `sb.Replace(3,4)`
//     both died with "Call parameter type does not match function
//     signature!"), because the runtime helper takes the arguments as their
//     Bezel type.
//
// §22.5/§22.6: a call that matches no declared form is a compile error.
// §5.1: the conversion at a declared parameter is the ordinary implicit
// conversion, so an argument that does not convert to it is a compile error.
//
// The rows below are on the strict AOT oracle (`runAot`): a rejection row
// asserts the SPECIFIC diagnostic, and a control row asserts the RENDERED
// VALUE of every §3.1 member that still works.
#include <string>
#include <vector>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// The program every rejection row runs: a builder holding "hello", so a
// rejected call is visibly a no-op when it is (wrongly) accepted.
std::string prog(const std::string& stmt) {
    return "module M;\n"
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
void expectStdout(const std::string& body, const std::string& out) {
    auto r = runAot("module M;\n"
                    "class C\n"
                    "  public static method Main(): int\n"
                    "    " + body + "\n"
                    "    return 0;\n"
                    "  end Main;\n"
                    "end C;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

const char kReplaceArity[] =
    "the call to `StringBuilder.Replace` supplies 0 argument(s) but stdlib "
    "§3.1 declares `Replace(oldValue: string, newValue: string): "
    "StringBuilder` — exactly 2 argument(s) (§22.5/§22.6; stdlib §3.1)";
const char kReplaceOldValueType[] =
    "the argument `oldValue` to `StringBuilder.Replace` (stdlib §3.1 declares "
    "`Replace(oldValue: string, newValue: string): StringBuilder`) has a type "
    "that is not implicitly convertible to the target type (§5.1)";

}  // namespace

// ── §22.5/§22.6: a call at an arity §3.1 declares no form for ──────────────
// (the issue's own repro: `sb.Replace()` compiled, ran, and printed "hello").

BEZEL_TEST(stringbuilder_member_surface_7207, replace_zero_args_rejected) {
    expectRejected("sb.Replace();", kReplaceArity);
}
BEZEL_TEST(stringbuilder_member_surface_7207, replace_one_arg_rejected) {
    expectRejected("sb.Replace(\"l\");",
                   "the call to `StringBuilder.Replace` supplies 1 argument(s) "
                   "but stdlib §3.1 declares `Replace(oldValue: string, "
                   "newValue: string): StringBuilder` — exactly 2 argument(s) "
                   "(§22.5/§22.6; stdlib §3.1)");
}
BEZEL_TEST(stringbuilder_member_surface_7207, replace_three_args_rejected) {
    expectRejected("sb.Replace(\"l\", \"L\", \"x\");",
                   "the call to `StringBuilder.Replace` supplies 3 argument(s) "
                   "but stdlib §3.1 declares `Replace(oldValue: string, "
                   "newValue: string): StringBuilder` — exactly 2 argument(s) "
                   "(§22.5/§22.6; stdlib §3.1)");
}
// The null-conditional spelling (`sb?.Replace()`) is the same hole on a
// separate path (a NullCondMemberExpr callee, #2581/#2594).
BEZEL_TEST(stringbuilder_member_surface_7207,
           null_conditional_replace_zero_args_rejected) {
    expectRejected("sb?.Replace();", kReplaceArity);
}
BEZEL_TEST(stringbuilder_member_surface_7207, append_zero_args_rejected) {
    expectRejected("sb.Append();",
                   "the call to `StringBuilder.Append` supplies 0 argument(s) "
                   "but stdlib §3.1 declares `Append(value: string): "
                   "StringBuilder` and its per-type overloads — exactly 1 "
                   "argument(s) (§22.5/§22.6; stdlib §3.1)");
}
BEZEL_TEST(stringbuilder_member_surface_7207, append_two_args_rejected) {
    expectRejected("sb.Append(\"a\", \"b\");",
                   "the call to `StringBuilder.Append` supplies 2 argument(s) "
                   "but stdlib §3.1 declares `Append(value: string): "
                   "StringBuilder` and its per-type overloads — exactly 1 "
                   "argument(s) (§22.5/§22.6; stdlib §3.1)");
}
BEZEL_TEST(stringbuilder_member_surface_7207, appendline_two_args_rejected) {
    expectRejected("sb.AppendLine(\"a\", \"b\");",
                   "the call to `StringBuilder.AppendLine` supplies 2 "
                   "argument(s) but stdlib §3.1 declares `AppendLine(): "
                   "StringBuilder` / `AppendLine(value: string): StringBuilder` "
                   "— 0 or 1 argument(s) (§22.5/§22.6; stdlib §3.1)");
}
BEZEL_TEST(stringbuilder_member_surface_7207, appendformat_one_arg_rejected) {
    expectRejected("sb.AppendFormat(\"x\");",
                   "the call to `StringBuilder.AppendFormat` supplies 1 "
                   "argument(s) but stdlib §3.1 declares `AppendFormat(format: "
                   "string, args: Array<Object>): StringBuilder` — exactly 2 "
                   "argument(s) (§22.5/§22.6; stdlib §3.1)");
}
BEZEL_TEST(stringbuilder_member_surface_7207, appendformat_three_args_rejected) {
    expectRejected("sb.AppendFormat(\"x\", [1], 2);",
                   "the call to `StringBuilder.AppendFormat` supplies 3 "
                   "argument(s) but stdlib §3.1 declares `AppendFormat(format: "
                   "string, args: Array<Object>): StringBuilder` — exactly 2 "
                   "argument(s) (§22.5/§22.6; stdlib §3.1)");
}
BEZEL_TEST(stringbuilder_member_surface_7207, insert_one_arg_rejected) {
    expectRejected("sb.Insert(0);",
                   "the call to `StringBuilder.Insert` supplies 1 argument(s) "
                   "but stdlib §3.1 declares `Insert(index: int, value: "
                   "string): StringBuilder` — exactly 2 argument(s) "
                   "(§22.5/§22.6; stdlib §3.1)");
}
BEZEL_TEST(stringbuilder_member_surface_7207, insert_three_args_rejected) {
    expectRejected("sb.Insert(0, \"a\", 3);",
                   "the call to `StringBuilder.Insert` supplies 3 argument(s) "
                   "but stdlib §3.1 declares `Insert(index: int, value: "
                   "string): StringBuilder` — exactly 2 argument(s) "
                   "(§22.5/§22.6; stdlib §3.1)");
}
BEZEL_TEST(stringbuilder_member_surface_7207, remove_one_arg_rejected) {
    expectRejected("sb.Remove(0);",
                   "the call to `StringBuilder.Remove` supplies 1 argument(s) "
                   "but stdlib §3.1 declares `Remove(startIndex: int, length: "
                   "int): StringBuilder` — exactly 2 argument(s) "
                   "(§22.5/§22.6; stdlib §3.1)");
}
BEZEL_TEST(stringbuilder_member_surface_7207, remove_three_args_rejected) {
    expectRejected("sb.Remove(0, 1, 2);",
                   "the call to `StringBuilder.Remove` supplies 3 argument(s) "
                   "but stdlib §3.1 declares `Remove(startIndex: int, length: "
                   "int): StringBuilder` — exactly 2 argument(s) "
                   "(§22.5/§22.6; stdlib §3.1)");
}
BEZEL_TEST(stringbuilder_member_surface_7207, clear_one_arg_rejected) {
    expectRejected("sb.Clear(1);",
                   "the call to `StringBuilder.Clear` supplies 1 argument(s) "
                   "but stdlib §3.1 declares `Clear(): StringBuilder` — exactly "
                   "0 argument(s) (§22.5/§22.6; stdlib §3.1)");
}

// ── §5.1: the conversion at a declared parameter ────────────────────────────
// (the issue's own repro: `sb.Replace('l','L')` and `sb.Replace(3,4)` both
// died in codegen with malformed IR).

BEZEL_TEST(stringbuilder_member_surface_7207, replace_char_args_rejected) {
    expectRejected("sb.Replace('l', 'L');", kReplaceOldValueType);
}
BEZEL_TEST(stringbuilder_member_surface_7207, replace_int_args_rejected) {
    expectRejected("sb.Replace(3, 4);", kReplaceOldValueType);
}
BEZEL_TEST(stringbuilder_member_surface_7207, replace_object_args_rejected) {
    expectRejected("var o: Object := \"l\"; sb.Replace(o, o);",
                   kReplaceOldValueType);
}
// §22.5: a named argument binds to the parameter it NAMES, so the type check
// reads the PARAMETER, not the position the argument occupies.
BEZEL_TEST(stringbuilder_member_surface_7207, replace_named_char_args_rejected) {
    expectRejected("sb.Replace(newValue := 'L', oldValue := 'l');",
                   kReplaceOldValueType);
}
BEZEL_TEST(stringbuilder_member_surface_7207, insert_string_index_rejected) {
    expectRejected("sb.Insert(\"x\", \"y\");",
                   "the argument `index` to `StringBuilder.Insert` (stdlib §3.1 "
                   "declares `Insert(index: int, value: string): StringBuilder`) "
                   "has a type that is not implicitly convertible to the target "
                   "type (§5.1)");
}
BEZEL_TEST(stringbuilder_member_surface_7207, remove_string_index_rejected) {
    expectRejected("sb.Remove(\"x\", \"y\");",
                   "the argument `startIndex` to `StringBuilder.Remove` (stdlib "
                   "§3.1 declares `Remove(startIndex: int, length: int): "
                   "StringBuilder`) has a type that is not implicitly "
                   "convertible to the target type (§5.1)");
}
// §5.1's table has no `long` -> `int` row (its row is `int` -> `long`), so a
// run-time `long` index is not an implicit conversion. §5.1's
// constant-narrowing clause still admits a COMPILE-TIME CONSTANT of any
// integer type — the control row below pins that half.
BEZEL_TEST(stringbuilder_member_surface_7207, remove_long_index_rejected) {
    expectRejected("var n: long := 1; sb.Remove(n, 2);",
                   "the argument `startIndex` to `StringBuilder.Remove` (stdlib "
                   "§3.1 declares `Remove(startIndex: int, length: int): "
                   "StringBuilder`) has a type that is not implicitly "
                   "convertible to the target type (§5.1)");
}
BEZEL_TEST(stringbuilder_member_surface_7207, appendformat_int_format_rejected) {
    expectRejected("var args: Array<Object> := [1]; sb.AppendFormat(3, args);",
                   "the argument `format` to `StringBuilder.AppendFormat` "
                   "(stdlib §3.1 declares `AppendFormat(format: string, args: "
                   "Array<Object>): StringBuilder`) has a type that is not "
                   "implicitly convertible to the target type (§5.1)");
}
// §3.1 declares `args: Array<Object>`, and a class type is INVARIANT
// (§12.9), so `Array<int>` is not an `Array<Object>`.
BEZEL_TEST(stringbuilder_member_surface_7207, appendformat_non_array_args_rejected) {
    expectRejected("var args: Array<int> := [1]; sb.AppendFormat(\"{0}\", args);",
                   "no matching overload of `StringBuilder.AppendFormat` — "
                   "stdlib §3.1 declares `AppendFormat(format: string, args: "
                   "Array<Object>): StringBuilder`, so the `args` argument "
                   "must be an `Array<Object>` (stdlib §3.1; §5.1)");
}

// ── CONTROLS: every §3.1 form that works must keep working, with its
// rendered value pinned ────────────────────────────────────────────────────

BEZEL_TEST(stringbuilder_member_surface_7207, append_string_control) {
    expectStdout("var s := new StringBuilder(); s.Append(\"a\"); "
                 "Console.WriteLine(s.ToString());",
                 "a\n");
}
BEZEL_TEST(stringbuilder_member_surface_7207, append_char_control) {
    expectStdout("var s := new StringBuilder(); s.Append('b'); "
                 "Console.WriteLine(s.ToString());",
                 "b\n");
}
BEZEL_TEST(stringbuilder_member_surface_7207, append_int_control) {
    expectStdout("var s := new StringBuilder(); s.Append(3); "
                 "Console.WriteLine(s.ToString());",
                 "3\n");
}
BEZEL_TEST(stringbuilder_member_surface_7207, append_bool_control) {
    expectStdout("var s := new StringBuilder(); s.Append(true); "
                 "Console.WriteLine(s.ToString());",
                 "True\n");
}
BEZEL_TEST(stringbuilder_member_surface_7207, append_object_control) {
    expectStdout("var s := new StringBuilder(); var o: Object := \"c\"; "
                 "s.Append(o); Console.WriteLine(s.ToString());",
                 "c\n");
}
BEZEL_TEST(stringbuilder_member_surface_7207, append_chain_returns_this_control) {
    expectStdout("var s := new StringBuilder(); "
                 "s.Append(\"a\").Append('b').Append(3); "
                 "Console.WriteLine(s.ToString());",
                 "ab3\n");
}
BEZEL_TEST(stringbuilder_member_surface_7207, appendline_value_control) {
    expectStdout("var s := new StringBuilder(); s.AppendLine(\"x\"); "
                 "Console.WriteLine(s.ToString());",
                 "x\n\n");  // the terminator, then WriteLine's own
}
BEZEL_TEST(stringbuilder_member_surface_7207, appendline_no_args_control) {
    expectStdout("var s := new StringBuilder(); s.AppendLine(); "
                 "Console.WriteLine(s.ToString());",
                 "\n\n");  // the terminator, then WriteLine's own
}
BEZEL_TEST(stringbuilder_member_surface_7207, appendformat_control) {
    expectStdout("var s := new StringBuilder(); "
                 "var args: Array<Object> := [1]; s.AppendFormat(\"{0}\", args); "
                 "Console.WriteLine(s.ToString());",
                 "1\n");
}
BEZEL_TEST(stringbuilder_member_surface_7207, insert_control) {
    expectStdout("var s := new StringBuilder(\"hello\"); s.Insert(0, \"X\"); "
                 "Console.WriteLine(s.ToString());",
                 "Xhello\n");
}
BEZEL_TEST(stringbuilder_member_surface_7207, remove_control) {
    expectStdout("var s := new StringBuilder(\"hello\"); s.Remove(0, 2); "
                 "Console.WriteLine(s.ToString());",
                 "llo\n");
}
BEZEL_TEST(stringbuilder_member_surface_7207, replace_control) {
    expectStdout("var s := new StringBuilder(\"hello\"); "
                 "s.Replace(\"l\", \"L\"); Console.WriteLine(s.ToString());",
                 "heLLo\n");
}
BEZEL_TEST(stringbuilder_member_surface_7207, clear_control) {
    expectStdout("var s := new StringBuilder(\"hello\"); s.Clear(); "
                 "Console.WriteLine(\"[\" + s.ToString() + \"]\");",
                 "[]\n");
}
BEZEL_TEST(stringbuilder_member_surface_7207, tostring_control) {
    expectStdout("var s := new StringBuilder(\"hello\"); "
                 "Console.WriteLine(s.ToString());",
                 "hello\n");
}
BEZEL_TEST(stringbuilder_member_surface_7207, length_property_control) {
    expectStdout("var s := new StringBuilder(\"hello\"); "
                 "Console.WriteLine(s.Length);",
                 "5\n");
}
// §22.5: a named argument binds to the parameter it NAMES, in any order.
BEZEL_TEST(stringbuilder_member_surface_7207, named_replace_control) {
    expectStdout("var s := new StringBuilder(\"hello\"); "
                 "s.Replace(oldValue := \"l\", newValue := \"L\"); "
                 "Console.WriteLine(s.ToString());",
                 "heLLo\n");
}
BEZEL_TEST(stringbuilder_member_surface_7207, named_replace_reordered_control) {
    expectStdout("var s := new StringBuilder(\"hello\"); "
                 "s.Replace(newValue := \"L\", oldValue := \"l\"); "
                 "Console.WriteLine(s.ToString());",
                 "heLLo\n");
}
// §5.1's constant-narrowing clause: a COMPILE-TIME CONSTANT of any integer
// type narrows to an `int` parameter whose range contains its value.
BEZEL_TEST(stringbuilder_member_surface_7207, remove_long_constant_control) {
    expectStdout("var s := new StringBuilder(\"hello\"); s.Remove(0L, 2L); "
                 "Console.WriteLine(s.ToString());",
                 "llo\n");
}

// A USER-declared class named `StringBuilder` is NOT the built-in surface:
// it has a declaration, and its own members are checked by the ordinary
// user-method machinery. The call is placed in a branch that never runs, so
// the row pins the COMPILE (the built-in `StringBuilder` lowering claims the
// receiver by head name at codegen, which is a separate defect).
BEZEL_TEST(stringbuilder_member_surface_7207, user_defined_stringbuilder_control) {
    const std::string src =
        "module M;\n"
        "public class StringBuilder\n"
        "  public method Replace(a: string, b: string): StringBuilder\n"
        "    return this;\n"
        "  end Replace;\n"
        "end StringBuilder;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var sb := new StringBuilder();\n"
        "    var never := false;\n"
        "    if never then\n"
        "      _ := sb.Replace(\"l\", \"L\");\n"
        "    end;\n"
        "    Console.WriteLine(\"ok\");\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("ok\n"));
}
