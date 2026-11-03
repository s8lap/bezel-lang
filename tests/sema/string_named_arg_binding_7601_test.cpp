// Regression test for issue #7601 -- the §5.1 argument-type gate read a
// REORDERED named argument BY POSITION, so a legal §22.5 reordered named
// call on a built-in `string` member was REFUSED.
//
// THE DEFECT (already fixed on this tree; the issue stayed OPEN).
//
//   spec §22.5 (v0.350): "a named argument binds the parameter it names, in
//   any order". The issue's own measurement: `"ab".PadLeft(paddingChar := '*',
//   totalWidth := 5)` was rejected with "argument to string.PadLeft has a type
//   that is not implicitly convertible" -- the gate compared the FIRST
//   argument ('*', a char) against the FIRST PARAMETER (`totalWidth`, an int).
//   The same happened for PadRight, Insert, CompareTo and
//   `IndexOf(startIndex :=, value :=)`.
//
// WHAT FIXED IT.
//
//   commit 01df7809 -- "fix #7674: type a built-in string call's arguments by
//   the parameter each name binds". The declared parameter an argument is
//   checked against is now the one its NAME binds, not the one at its source
//   index (the gate builds the binding locally over the registry's declared
//   names, exactly as the §9.6 declared-parameter gate does).
//
//   That commit closed #7674 -- the same defect filed under the other
//   number -- and did not name #7601, so #7601 stayed OPEN until this one.
//
// THE ORACLE.
//
//   Every row asserts the EXACT stdout of the AOT program: the reordered
//   named call must COMPILE and produce the conforming value. On the pre-fix
//   tree a call whose POSITIONAL reading differs from its §22.5 binding is
//   refused, so the program does not run and the stdout is empty: those rows
//   are RED there and GREEN here.
//
//   The rows whose positional reading already coincides with the binding
//   (`CompareTo`, which has one parameter; `Insert` in declaration order)
//   and the POSITIONAL spelling of the same member are CONTROLS: they pass
//   on BOTH arms, so a fix that refused every named call could not pass.
//
// Cite: spec §22.5 (v0.350), §5.1 (v0.350); stdlib §9.6 (v0.350).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotOut;

namespace {

std::string prog(const std::string& body) {
    return "module M; import System; "
           "public class C "
           "  public static method Main(): int " +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

}  // namespace

// ---------------------------------------------------------------------------
// THE DEFECT ROWS -- a reordered named argument binds the parameter it NAMES.
//
// `PadLeft` / `PadRight` / `IndexOf` / `Insert` have MORE THAN ONE parameter,
// so the positional reading the pre-fix gate used is the wrong one for a
// reordered call: these four rows are RED on the pre-fix tree and GREEN here.
//
// The two rows below them (`CompareTo`, which has ONE parameter, and `Insert`
// in DECLARATION order, whose positional reading coincides with the binding)
// were never broken -- they pass on BOTH arms, and they are what makes the
// four rows above attributable to the binding and not to the member.
// ---------------------------------------------------------------------------
BEZEL_TEST(string_named_arg_binding_7601,
           padleft_reordered_named_arguments_bind_by_name_22_5) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    Console.WriteLine(\"ab\".PadLeft(paddingChar := '*', "
        "totalWidth := 5)); ")),
        std::string("***ab"));
}

BEZEL_TEST(string_named_arg_binding_7601,
           padright_reordered_named_arguments_bind_by_name_22_5) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    Console.WriteLine(\"ab\".PadRight(paddingChar := '*', "
        "totalWidth := 5)); ")),
        std::string("ab***"));
}

BEZEL_TEST(string_named_arg_binding_7601,
           compareto_reordered_named_arguments_bind_by_name_22_5) {
    // ONE parameter: the positional reading and the §22.5 binding coincide, so
    // this row was never broken. It is a CONTROL row.
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    Console.WriteLine(\"ab\".CompareTo(other := \"ac\")); ")),
        std::string("-1"));
}

BEZEL_TEST(string_named_arg_binding_7601,
           indexof_reordered_named_arguments_bind_by_name_22_5) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    Console.WriteLine(\"ab\".IndexOf(startIndex := 0, value := \"a\")); ")),
        std::string("0"));
}

BEZEL_TEST(string_named_arg_binding_7601,
           insert_reordered_named_arguments_bind_by_name_22_5) {
    // `Insert(startIndex: int, value: string)` in DECLARATION order: the
    // positional reading coincides with the §22.5 binding, so this row was
    // never broken. It is a CONTROL row; the row below it is the reversed
    // order, which the pre-fix gate read by position and refused.
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    Console.WriteLine(\"ab\".Insert(startIndex := 0, value := \"x\")); ")),
        std::string("xab"));
}

BEZEL_TEST(string_named_arg_binding_7601,
           insert_reordered_in_the_other_direction_binds_by_name_22_5) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    Console.WriteLine(\"ab\".Insert(value := \"x\", startIndex := 0)); ")),
        std::string("xab"));
}

// ---------------------------------------------------------------------------
// THE CONTROL ROW -- the POSITIONAL spelling of the same member, which was
// never affected.  It passes on BOTH arms.
// ---------------------------------------------------------------------------
BEZEL_TEST(string_named_arg_binding_7601,
           control_positional_padleft_is_unchanged_9_6) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    Console.WriteLine(\"ab\".PadLeft(5)); ")),
        std::string("   ab"));
}
