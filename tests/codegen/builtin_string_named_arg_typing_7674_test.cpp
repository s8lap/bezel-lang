// Spec-derived regression test for issue #7674 -- reordered named arguments to
// the built-in `string` members IndexOf, Insert, CompareTo and PadLeft were
// type-checked by SOURCE POSITION instead of by the parameter each name binds,
// so a correct call was rejected with "not implicitly convertible".
//
// spec §22.5 "Optional parameters and named arguments" (v0.350): "naming an
// argument changes which parameter it binds to, never when it runs"; each
// supplied argument binds exactly one parameter, so §5.1's implicit-conversion
// check applies between an argument and the parameter it NAMES.
// stdlib §9.6 (v0.350): IndexOf(value: char|string, startIndex: int := 0),
// Insert(startIndex: int, value: string), CompareTo(other: string,
// ignoreCase: bool), PadLeft(totalWidth: int, paddingChar: char := ' ').
//
// Each argument is a call `T*(sb, "<tag>", value)` appending its tag to a
// trace, so the row also proves written-order evaluation, each exactly once.

#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotOut;

namespace {

const char* kHelpers =
    "  public static method T(sb: StringBuilder, s: string, v: int): int "
    "    sb.Append(s); "
    "    return v; "
    "  end T; "
    "  public static method TS(sb: StringBuilder, s: string, v: string): string "
    "    sb.Append(s); "
    "    return v; "
    "  end TS; "
    "  public static method TB(sb: StringBuilder, s: string, v: bool): bool "
    "    sb.Append(s); "
    "    return v; "
    "  end TB; "
    "  public static method TC(sb: StringBuilder, s: string, v: char): char "
    "    sb.Append(s); "
    "    return v; "
    "  end TC; ";

std::string prog(const std::string& body) {
    return std::string("module M; import System; import System.Text; "
                       "class C ") +
           kHelpers +
           "  public static method Main(): int "
           "    using var sb := new StringBuilder(); "
           "    var s := \"hello world\"; " +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

std::string runTraced(const std::string& stmtDeclaringR) {
    return runAotOut(prog(
        stmtDeclaringR +
        "    Console.WriteLine(r.ToString() + \"|\" + sb.ToString()); "));
}

}  // namespace

// The positional control: `IndexOf(value, startIndex)`.
BEZEL_TEST(builtin_string_named_arg_typing_7674, indexof_positional_control) {
    BEZEL_EXPECT_EQ(
        runTraced("    var r := s.IndexOf(TS(sb, \"A\", \"l\"), T(sb, \"B\", 4)); "),
        std::string("9|AB"));
}

// The issue's repro (m03): the int start index is written FIRST.
BEZEL_TEST(builtin_string_named_arg_typing_7674, indexof_named_reordered) {
    BEZEL_EXPECT_EQ(
        runTraced("    var r := s.IndexOf(startIndex := T(sb, \"A\", 4), "
                  "                       value := TS(sb, \"B\", \"l\")); "),
        std::string("9|AB"));
}

// The char overload of the same member, reordered.
BEZEL_TEST(builtin_string_named_arg_typing_7674, indexof_named_reordered_char) {
    BEZEL_EXPECT_EQ(
        runTraced("    var r := s.IndexOf(startIndex := T(sb, \"A\", 4), "
                  "                       value := TC(sb, \"B\", 'o')); "),
        std::string("4|AB"));
}

// The issue's repro (m04): `Insert(startIndex: int, value: string)`, value first.
BEZEL_TEST(builtin_string_named_arg_typing_7674, insert_named_reordered) {
    BEZEL_EXPECT_EQ(
        runTraced("    var r := s.Insert(value := TS(sb, \"A\", \"XY\"), "
                  "                      startIndex := T(sb, \"B\", 5)); "),
        std::string("helloXY world|AB"));
}

// The issue's repro (m07): `CompareTo(other, ignoreCase)`, ignoreCase first.
BEZEL_TEST(builtin_string_named_arg_typing_7674, compareto_named_reordered) {
    BEZEL_EXPECT_EQ(
        runTraced("    var r := s.CompareTo(ignoreCase := TB(sb, \"A\", true), "
                  "                         other := TS(sb, \"B\", \"HELLO WORLD\")); "),
        std::string("0|AB"));
}

// Same call with ignoreCase false: the case difference now shows.
BEZEL_TEST(builtin_string_named_arg_typing_7674, compareto_named_reordered_case_sensitive) {
    BEZEL_EXPECT_EQ(
        runTraced("    var c := s.CompareTo(ignoreCase := TB(sb, \"A\", false), "
                  "                         other := TS(sb, \"B\", \"HELLO WORLD\")); "
                  "    var r := c <> 0; "),
        std::string("True|AB"));
}

// The issue's repro (m08): `PadLeft(totalWidth: int, paddingChar: char)`,
// paddingChar first.
BEZEL_TEST(builtin_string_named_arg_typing_7674, padleft_named_reordered) {
    BEZEL_EXPECT_EQ(
        runTraced("    var r := s.PadLeft(paddingChar := TC(sb, \"A\", '*'), "
                  "                       totalWidth := T(sb, \"B\", 15)); "),
        std::string("****hello world|AB"));
}

// Typing is NOT dropped: the argument is still checked against the parameter
// its name binds. `totalWidth := "x"` is a string where `int` is declared.
BEZEL_TEST(builtin_string_named_arg_typing_7674, padleft_reordered_wrong_type_still_rejected) {
    JitResult r = runAot(prog(
        "    var r := s.PadLeft(paddingChar := '*', totalWidth := \"x\"); "));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "argument to `string.PadLeft` has a type that is not implicitly "
           "convertible"));
}

// `Insert(startIndex := "x", value := 5)`: both arguments are mistyped against
// the parameters their names bind (the old positional reading accepted the
// swap of kinds only by accident).
BEZEL_TEST(builtin_string_named_arg_typing_7674, insert_reordered_wrong_type_still_rejected) {
    JitResult r = runAot(prog(
        "    var r := s.Insert(value := 5, startIndex := \"x\"); "));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "argument to `string.Insert` has a type that is not implicitly "
           "convertible"));
}

// IndexOf's search-value gate (#6562) follows the NAME, not the index: an int
// bound to `value` is still refused when written second.
BEZEL_TEST(builtin_string_named_arg_typing_7674, indexof_reordered_int_search_value_still_rejected) {
    JitResult r = runAot(prog(
        "    var r := s.IndexOf(startIndex := 1, value := 2); "));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(bezel::test::diagnosticMessageContains(
        r, "`string.IndexOf` takes its search value at position 0"));
}

// Loop 100x under ASan + LSan: the reordered calls leave no leak.
BEZEL_TEST(builtin_string_named_arg_typing_7674, named_loop_clean_under_asan) {
    JitResult r = runAotChecked(prog(
        "    var total := 0; "
        "    for var i in 0..<100 do "
        "      total += s.IndexOf(startIndex := T(sb, \"A\", 0), value := TS(sb, \"B\", \"ll\")); "
        "      total += s.Insert(value := TS(sb, \"C\", \"XY\"), startIndex := T(sb, \"D\", 1)).Length; "
        "      total += s.CompareTo(ignoreCase := TB(sb, \"E\", true), other := TS(sb, \"F\", \"HELLO WORLD\")); "
        "      total += s.PadLeft(paddingChar := TC(sb, \"G\", '*'), totalWidth := T(sb, \"H\", 12)).Length; "
        "    end for; "
        "    Console.WriteLine(total.ToString() + \"|\" + sb.Length.ToString() + \"|\" + "
        "                      sb.ToString().Substring(0, 8)); "));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2700|800|ABCDEFGH\n"));
}
