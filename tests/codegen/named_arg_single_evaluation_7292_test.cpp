// Spec-derived regression test for issue #7292 -- THE ARGUMENTS OF A
// NAMED-ARGUMENT CALL WERE EVALUATED TWICE.
//
// THE DEFECT.
//
// The built-in member-call pre-pass in `lowerCallDispatch`
// (src/codegen/LlvmBackend_Calls2.cpp) lowered EVERY argument of a reordered
// named call and DISCARDED the value; the built-in arm's own `lowerExpr` then
// evaluated the SAME argument expression a SECOND time.  A named argument
// with a side effect therefore ran twice -- a silently-wrong-answer defect,
// not merely a wasted-effect one.
//
// MEASURED on the pre-fix tree (633faf4a, the parent of 9570dcd7) by the
// trace of tags each argument appends (the trace is the ONLY way to see "how
// many times did this argument run?"):
//
//   Convert.ToString(toBase := A, value := B)   trace "ABAB"   (want "AB")
//   Math.Max(b := A, a := B)                   trace "ABAB"   (want "AB")
//   Math.Min(b := A, a := B)                   trace "ABAB"   (want "AB")
//
// and the repository's own conformance row
// `tests/conformance/rules/cases/l2_3_0006.bzl` (the same `Convert.ToString`
// call, with the arguments' own prints) printed
//
//   "tb\nv\ntb\nv\nff"      (want "tb\nv\nff")
//
// on that tree, while it prints exactly "tb\nv\nff" today.
//
// THE DOCUMENTS.
//
// spec §22.5 "Evaluation order" (v0.350): "A call's supplied argument
// expressions are evaluated in call-site textual order -- left to right as
// written, for positional and named arguments alike; naming an argument
// changes which parameter it binds to, never when it runs", and each supplied
// argument runs EXACTLY ONCE.
//
// stdlib §2.3 (v0.350) declares
//   public static method ToString(value: int, toBase: int): string;
// and §9.1 (v0.350) gives that member's text: the radix digits are
// lower-case, so `Convert.ToString(255, 16)` is "ff" and
// `Convert.ToString(10, 16)` is "a".  stdlib §2.2 declares
//   public static method Max<T>(a: T, b: T): T where T: IComparable<T>;
//   public static method Min<T>(a: T, b: T): T where T: IComparable<T>;
//
// THE ORACLE.
//
// Every row prints the COMPUTED VALUE and the TRACE on ONE line, separated by
// a '|'.  The value pins the binding (a mis-bound radix or operand cannot
// produce the expected text) and the trace pins the COUNT (a double
// evaluation doubles the trace).  A row therefore fails if EITHER the binding
// or the number of evaluations is wrong.  There is no negative assertion on
// a diagnostic anywhere in this file: the rows assert the CORRECT observable
// (the value and the trace), which is a strictly stronger oracle than "the
// compiler did not print the old broken thing".
//
// THE PIN ROWS AND THE CONTROL ROWS.
//
// The defect is the REORDERED named call: a call whose named arguments are
// written in an order other than the parameters' declaration order.  Such a
// call publishes a non-identity binding, which is what put it through the
// pre-pass.  A call whose named arguments are already in declaration order
// publishes the IDENTITY permutation and never reached the pre-pass, so it
// is a CONTROL here (green on both arms), exactly like the positional
// spelling.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotOut;

namespace {

// The helper each argument calls: it appends its tag to the trace and
// returns the value it is given.
const char* kHelpers =
    "  public static method T(sb: StringBuilder, tag: string, v: int): int "
    "    sb.Append(tag); "
    "    return v; "
    "  end T; ";

std::string prog(const std::string& body) {
    return std::string("module M; import System; import System.Text; "
                       "class C ") +
           kHelpers +
           "  public static method Main(): int "
           "    using var sb := new StringBuilder(); " +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

// Runs the program and returns its ONE line of stdout: "<value>|<trace>".
std::string runTraced(const std::string& body) {
    return runAotOut(prog(body));
}

}  // namespace

// ===========================================================================
// THE PIN ROWS -- the named-argument call of the issue.
//
// `value` and `toBase` are named in an order other than the parameters'
// declaration order, so each of them binds the parameter it NAMES (§22.5)
// and each argument's expression runs exactly once, in the order it is
// written: "R" (the radix) is written first, so it runs first.
// ===========================================================================
BEZEL_TEST(named_arg_single_evaluation_7292,
           convert_to_string_reordered_named_args_run_once_in_written_order) {
    BEZEL_EXPECT_EQ(
        runTraced("    var r := Convert.ToString(toBase := T(sb, \"R\", 16), "
                  "                              value := T(sb, \"V\", 255)); "
                  "    Console.WriteLine(r + \"|\" + sb.ToString()); "),
        std::string("ff|RV"));
}

// ===========================================================================
// THE SAME CALL, A DIFFERENT VALUE -- the trace's half and the value's half
// are independent pins.
//
// `value` is 10 and `toBase` is 16 here, so the member must render "a" (ten
// in base sixteen, lower-case digits per §9.1); a POSITIONAL reading of the
// same two arguments would render "16" (the radix taken as the value).
// ===========================================================================
BEZEL_TEST(named_arg_single_evaluation_7292,
           convert_to_string_reordered_named_args_bind_by_name_not_position) {
    BEZEL_EXPECT_EQ(
        runTraced("    var r := Convert.ToString(toBase := T(sb, \"R\", 16), "
                  "                              value := T(sb, \"V\", 10)); "
                  "    Console.WriteLine(r + \"|\" + sb.ToString()); "),
        std::string("a|RV"));
}

// ===========================================================================
// A SECOND MEMBER -- `Math.Max`.
//
// stdlib §2.2 declares `Max<T>(a: T, b: T): T where T: IComparable<T>`, so
// `b` and `a` are both nameable parameters.  The row is the same defect in
// a different member: a reordered named call whose arguments ran twice.
// ===========================================================================
BEZEL_TEST(named_arg_single_evaluation_7292,
           math_max_reordered_named_args_run_once_in_written_order) {
    BEZEL_EXPECT_EQ(
        runTraced("    var r := Math.Max(b := T(sb, \"B\", 7), "
                  "                        a := T(sb, \"A\", 3)); "
                  "    Console.WriteLine(r.ToString() + \"|\" + "
                  "sb.ToString()); "),
        std::string("7|BA"));
}

// ===========================================================================
// A THIRD MEMBER -- `Math.Min`.
//
// The sibling of the row above, so that the two cannot both be satisfied by
// one arm's spelling.
// ===========================================================================
BEZEL_TEST(named_arg_single_evaluation_7292,
           math_min_reordered_named_args_run_once_in_written_order) {
    BEZEL_EXPECT_EQ(
        runTraced("    var r := Math.Min(b := T(sb, \"M\", 7), "
                  "                        a := T(sb, \"N\", 3)); "
                  "    Console.WriteLine(r.ToString() + \"|\" + "
                  "sb.ToString()); "),
        std::string("3|MN"));
}

// ===========================================================================
// THE CONTROL -- the IN-ORDER NAMED spelling of the issue's call.
//
// The named arguments are already in DECLARATION order, so the call
// publishes the IDENTITY permutation and never reached the pre-pass.  It
// must be byte-identical before and after the fix; the row is here so that
// the pin rows above cannot be satisfied by "the compiler now refuses the
// call" or by "the trace is empty".
// ===========================================================================
BEZEL_TEST(named_arg_single_evaluation_7292,
           convert_to_string_in_order_named_args_are_unchanged) {
    BEZEL_EXPECT_EQ(
        runTraced("    var r := Convert.ToString(value := T(sb, \"V\", 255), "
                  "                              toBase := T(sb, \"R\", 16)); "
                  "    Console.WriteLine(r + \"|\" + sb.ToString()); "),
        std::string("ff|VR"));
}

// ===========================================================================
// THE CONTROL -- the POSITIONAL spelling of the same call.
//
// A positional call publishes the IDENTITY permutation too, so it was always
// correct; it must be byte-identical before and after the fix.
// ===========================================================================
BEZEL_TEST(named_arg_single_evaluation_7292,
           convert_to_string_positional_args_run_once_in_written_order) {
    BEZEL_EXPECT_EQ(
        runTraced("    var r := Convert.ToString(T(sb, \"V\", 255), "
                  "                        T(sb, \"R\", 16)); "
                  "    Console.WriteLine(r + \"|\" + sb.ToString()); "),
        std::string("ff|VR"));
}
