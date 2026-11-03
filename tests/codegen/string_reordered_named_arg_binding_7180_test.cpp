// Spec-derived regression test for issue #7180 -- a REORDERED
// NAMED-ARGUMENT CALL to a built-in `string` member that has a `bool`
// parameter could make the compiler emit MALFORMED IR and abort.
//
// THE DEFECT.
//
// The `Equals` / `CompareTo` arms in src/codegen/LlvmBackend_String.cpp read
// their operands POSITIONALLY: `args[0]` was read as the STRING operand and
// `args[1]` as the BOOL.  With the reordered named call
//
//   s.Equals(ignoreCase := true, other := "ABC")
//
// `args[0]` is the BOOL and `args[1]` is the STRING, so the arm handed an
// `i1` to a `ptr` parameter.  That is not merely a wrong value: it is a
// COMPILER ABORT.  MEASURED on the pre-fix tree (the parent of 40d17acd):
//
//   Branch condition is not 'i1' type!
//     br ptr @.bz.str.53, label %eq.ic, label %eq.pl
//   ...
//   bezelc: codegen error: IR verification failed (malformed IR emitted)
//
// THE DOCUMENTS.
//
// spec §22.5 "Optional parameters and named arguments" (v0.350): a named
// argument BINDS THE PARAMETER IT NAMES, in any order after the positional
// ones; "naming an argument changes which parameter it binds to, never when
// it runs".
//
// stdlib §9.6 (v0.350) declares the two members that carry a `bool`
// parameter:
//
//   public pure method Equals(other: string?, ignoreCase: bool): bool;
//   public pure method CompareTo(other: string, ignoreCase: bool): int;
//
// Both spellings are LEGAL: `ignoreCase` and `other` are declared parameter
// names.  So the reordered call must be a CLEAN ACCEPT with the CORRECT
// value -- never a rejection, and never a compiler abort.
//
// THE ORACLE.
//
// Every row prints the member's VALUE on stdout and asserts the exact text.
// The operand pair is DISCRIMINATING ("ABC" against "abc"), so a wrong
// binding cannot produce the expected answer by accident: the
// case-SENSITIVE comparison of "ABC" and "abc" is False and their
// case-INSENSITIVE comparison is True (and the case-insensitive
// `CompareTo` is 0, where the case-sensitive one is not).  A row therefore
// fails on the pre-fix tree (the compiler aborts, so the program produces no
// value at all) and passes today.
//
// There is no negative assertion on a diagnostic anywhere in this file: the
// rows assert the CORRECT observable (the value), which is a strictly
// stronger oracle than "the compiler did not abort".

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotOut;

namespace {

// `stmt` is the call whose value is printed.
std::string prog(const std::string& stmt) {
    return std::string("module M; import System; "
                       "public class Program ") +
           "  public static method Main(): int "
           "    var s: string := \"ABC\"; " +
           stmt +
           "    return 0; "
           "  end Main; "
           "end Program;";
}

}  // namespace

// ===========================================================================
// THE PIN ROW -- the reordered named-argument call of the issue.
//
// "ABC" against "abc", case-insensitively: True.  The case-SENSITIVE answer
// is False, so a mis-bound `ignoreCase` cannot produce True by accident.
// ===========================================================================
BEZEL_TEST(string_reordered_named_arg_binding_7180,
           equals_reordered_named_args_are_accepted_and_correct) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    Console.WriteLine(s.Equals(ignoreCase := true, "
            "                                other := \"abc\")); ")),
        std::string("True"));
}

// ===========================================================================
// THE SECOND MEMBER WITH A `bool` PARAMETER.
//
// stdlib §9.6 declares `CompareTo(other: string, ignoreCase: bool): int`.
// The case-insensitive comparison of "ABC" and "abc" is 0 (equal); the
// case-sensitive one is not 0, so the value is discriminating too.
// ===========================================================================
BEZEL_TEST(string_reordered_named_arg_binding_7180,
           compare_to_reordered_named_args_are_accepted_and_correct) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    Console.WriteLine(s.CompareTo(ignoreCase := true, "
            "                                    other := \"abc\")); ")),
        std::string("0"));
}

// ===========================================================================
// THE CONTROL -- the POSITIONAL spelling of the same call.
//
// §22.5: a positional call binds the arguments to the parameters in
// declaration order, so this call was always correct.  It must be
// byte-identical before and after the fix; the row is here so that the pin
// rows above cannot be satisfied by "the compiler now refuses the call".
// ===========================================================================
BEZEL_TEST(string_reordered_named_arg_binding_7180,
           equals_positional_args_are_unchanged) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog("    Console.WriteLine(s.Equals(\"abc\", true)); ")),
        std::string("True"));
}

// ===========================================================================
// THE CONTROL -- the IN-ORDER NAMED spelling of the same call.
//
// The named arguments already in DECLARATION order publish the IDENTITY
// permutation, so this call was always correct too.  Together with the
// positional control it shows that the fix is exactly the REORDERED shape.
// ===========================================================================
BEZEL_TEST(string_reordered_named_arg_binding_7180,
           equals_in_order_named_args_are_unchanged) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    Console.WriteLine(s.Equals(other := \"abc\", "
            "                                ignoreCase := true)); ")),
        std::string("True"));
}

// ===========================================================================
// THE SECOND MEMBER'S CONTROL -- the positional `CompareTo`.
//
// The two members must AGREE with each other in the positional spelling,
// so a fix that disturbed one of them cannot pass this row.
// ===========================================================================
BEZEL_TEST(string_reordered_named_arg_binding_7180,
           compare_to_positional_args_are_unchanged) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog("    Console.WriteLine(s.CompareTo(\"abc\", true)); ")),
        std::string("0"));
}
