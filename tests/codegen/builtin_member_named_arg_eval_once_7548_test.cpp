// Spec-derived regression test for issue #7548 -- the built-in MEMBER-call
// argument pre-pass lowered every argument of a reordered named call and
// DISCARDED the value, so the arm's own lowering evaluated it a second time.
//
// spec §22.5 "Evaluation order" (v0.350): arguments are evaluated exactly once,
// in written (textual) order, and each binds the parameter it names; "the order
// is observable through side-effecting arguments and is normative."
// stdlib §9.7 `Math`, §9.8 `Convert`, §9.6 `string.Join` (v0.350).
//
// MEASURED on 0900fb1f (build/bezelc, real AOT compile + native run), each
// argument a call `T(sb, "<tag>", value)` that appends its tag to a trace:
//
//   Math.Pow(y := A, x := B)                  trace "ABAB"   (want "AB")
//   Math.Fmod(y := A, x := B)                 trace "ABABAB" (want "AB")
//   Math.Clamp(high := A, low := B, value := C)  "ABCABC"    (want "ABC")
//   Math.Round(digits := A, x := B)           trace "ABAB"
//   Convert.ToString(toBase := A, value := B) trace "ABAB"
//   string.Join(items := arr, separator := A) trace "AA"
//
// Every row prints the computed value AND the trace, so a positional mis-bind
// cannot pass (the value is checked) and a double evaluation cannot pass (the
// trace is checked).  The loop row runs under the ASan/LSan oracle
// (`runAotChecked`) and also checks the trace length, so a pinned value that a
// later arm releases a second time would fail it as a use-after-free.

#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
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
    "  public static method TD(sb: StringBuilder, s: string, v: double): double "
    "    sb.Append(s); "
    "    return v; "
    "  end TD; ";

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

// Prints "<value>|<trace>".
std::string runTraced(const std::string& stmtDeclaringR) {
    return runAotOut(prog(
        stmtDeclaringR +
        "    Console.WriteLine(r.ToString() + \"|\" + sb.ToString()); "));
}

}  // namespace

BEZEL_TEST(builtin_member_named_arg_eval_once_7548,
           math_pow_reordered_args_evaluated_once_in_written_order) {
    BEZEL_EXPECT_EQ(
        runTraced("    var r := Math.Pow(y := TD(sb, \"A\", 2.0), "
                  "                      x := TD(sb, \"B\", 3.0)); "),
        std::string("9|AB"));
}

BEZEL_TEST(builtin_member_named_arg_eval_once_7548,
           math_fmod_reordered_args_evaluated_once_in_written_order) {
    BEZEL_EXPECT_EQ(
        runTraced("    var r := Math.Fmod(y := TD(sb, \"A\", 2.0), "
                  "                       x := TD(sb, \"B\", 7.0)); "),
        std::string("1|AB"));
}

BEZEL_TEST(builtin_member_named_arg_eval_once_7548,
           math_clamp_three_reordered_args_evaluated_once_in_written_order) {
    BEZEL_EXPECT_EQ(
        runTraced("    var r := Math.Clamp(high := T(sb, \"A\", 9), "
                  "                        low := T(sb, \"B\", 1), "
                  "                        value := T(sb, \"C\", 5)); "),
        std::string("5|ABC"));
}

BEZEL_TEST(builtin_member_named_arg_eval_once_7548,
           math_round_reordered_args_evaluated_once_in_written_order) {
    BEZEL_EXPECT_EQ(
        runTraced("    var r := Math.Round(digits := T(sb, \"A\", 1), "
                  "                        x := TD(sb, \"B\", 2.55)); "),
        std::string("2.6|AB"));
}

BEZEL_TEST(builtin_member_named_arg_eval_once_7548,
           convert_tostring_reordered_args_evaluated_once_in_written_order) {
    BEZEL_EXPECT_EQ(
        runTraced("    var r := Convert.ToString(toBase := T(sb, \"A\", 2), "
                  "                              value := T(sb, \"B\", 5)); "),
        std::string("101|AB"));
}

BEZEL_TEST(builtin_member_named_arg_eval_once_7548,
           string_join_reordered_args_evaluated_once) {
    BEZEL_EXPECT_EQ(
        runTraced("    var parr := new Array<string>(2, \"x\"); "
                  "    var r := string.Join(items := parr, "
                  "                         separator := TS(sb, \"A\", \",\")); "),
        std::string("x,x|A"));
}

// Controls: the in-order named spelling and the positional one are untouched
// (no non-identity binding is published, so no pre-pass runs).
BEZEL_TEST(builtin_member_named_arg_eval_once_7548,
           in_order_named_and_positional_math_pow_unchanged) {
    BEZEL_EXPECT_EQ(
        runTraced("    var r := Math.Pow(x := TD(sb, \"A\", 2.0), "
                  "                      y := TD(sb, \"B\", 3.0)); "),
        std::string("8|AB"));
    BEZEL_EXPECT_EQ(
        runTraced("    var r := Math.Pow(TD(sb, \"A\", 2.0), "
                  "                      TD(sb, \"B\", 3.0)); "),
        std::string("8|AB"));
}

// A trailing optional parameter left at its default (`digits := 0`): the
// binding is published with an unbound trailing slot and the arm defaults it.
BEZEL_TEST(builtin_member_named_arg_eval_once_7548,
           trailing_default_left_unbound_is_evaluated_once) {
    BEZEL_EXPECT_EQ(
        runTraced("    var r := Math.Round(x := TD(sb, \"A\", 2.4)); "),
        std::string("2|A"));
}

// The pre-pass pins are dropped when the call is done: 100 iterations of the
// same call site, each evaluating its three argument calls exactly once, under
// ASan + LSan.  A leaked or doubly-released pinned value fails the checked
// oracle; the trace length (3 tags x 100) fails a double evaluation.
BEZEL_TEST(builtin_member_named_arg_eval_once_7548,
           reordered_member_args_in_a_loop_are_clean_under_asan) {
    JitResult r = runAotChecked(prog(
        "    var parr := new Array<string>(3, \"x\"); "
        "    var total := 0; "
        "    for var i in 0..<100 do "
        "      total += int(Math.Pow(y := TD(sb, \"a\", 2.0), "
        "                            x := TD(sb, \"b\", 3.0))); "
        "      total += string.Join(items := parr, "
        "                           separator := TS(sb, \"c\", \"-\")).Length; "
        "    end for; "
        "    Console.WriteLine(total.ToString() + \"|\" + "
        "                      sb.Length.ToString()); "));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1400|300\n"));
}
