// Spec-derived regression test for issue #7600 -- reordered named arguments of
// several built-in member arms were evaluated once each, but in PARAMETER
// order instead of written order.
//
// spec §22.5 "Evaluation order" (v0.350): "A call's supplied argument
// expressions are evaluated in call-site textual order ... naming an argument
// changes which parameter it binds to, never when it runs."
// stdlib §9.6 (`string.Remove`, static `string.Repeat`) and §3.1
// (`StringBuilder.Insert` / `Remove` / `Replace`), v0.350.
//
// MEASURED on f21ee9a6 (real AOT compile + native run), each argument a call
// `T(sb, "<tag>", value)` appending its tag to a trace; written order A then B:
//
//   s.Remove(length := A, startIndex := B)            trace "BA" (want "AB")
//   string.Repeat(count := A, unit := B)              trace "BA"
//   sb2.Insert(value := A, index := B)                trace "BA"
//   sb2.Remove(length := A, startIndex := B)          trace "BA"
//   sb2.Replace(newValue := A, oldValue := B)         trace "BA"
//
// Every row prints the computed value AND the trace.

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
    "  public static method R(sb: StringBuilder, s: string): string "
    "    sb.Append(s); "
    "    return \"hello world\"; "
    "  end R; "
    "  public static method Mk(sb: StringBuilder, s: string, t: StringBuilder): StringBuilder "
    "    sb.Append(s); "
    "    return t; "
    "  end Mk; ";

std::string prog(const std::string& body) {
    return std::string("module M; import System; import System.Text; "
                       "class C ") +
           kHelpers +
           "  public static method Main(): int "
           "    using var sb := new StringBuilder(); "
           "    using var sb2 := new StringBuilder(); "
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

BEZEL_TEST(builtin_member_named_arg_order_7600, string_remove_written_order) {
    BEZEL_EXPECT_EQ(
        runTraced("    var r := s.Remove(length := T(sb, \"A\", 6), "
                  "                      startIndex := T(sb, \"B\", 5)); "),
        std::string("hello|AB"));
}

BEZEL_TEST(builtin_member_named_arg_order_7600, string_repeat_static_written_order) {
    BEZEL_EXPECT_EQ(
        runTraced("    var r := string.Repeat(count := T(sb, \"A\", 3), "
                  "                           unit := TS(sb, \"B\", \"ab\")); "),
        std::string("ababab|AB"));
}

BEZEL_TEST(builtin_member_named_arg_order_7600, sb_insert_written_order) {
    BEZEL_EXPECT_EQ(
        runTraced("    sb2.Append(\"abc\"); "
                  "    var r := sb2.Insert(value := TS(sb, \"A\", \"XX\"), "
                  "                        index := T(sb, \"B\", 1)).ToString(); "),
        std::string("aXXbc|AB"));
}

BEZEL_TEST(builtin_member_named_arg_order_7600, sb_remove_written_order) {
    BEZEL_EXPECT_EQ(
        runTraced("    sb2.Append(\"abcdef\"); "
                  "    var r := sb2.Remove(length := T(sb, \"A\", 2), "
                  "                        startIndex := T(sb, \"B\", 1)).ToString(); "),
        std::string("adef|AB"));
}

BEZEL_TEST(builtin_member_named_arg_order_7600, sb_replace_written_order) {
    BEZEL_EXPECT_EQ(
        runTraced("    sb2.Append(\"abcdef\"); "
                  "    var r := sb2.Replace(newValue := TS(sb, \"A\", \"Z\"), "
                  "                         oldValue := TS(sb, \"B\", \"c\")).ToString(); "),
        std::string("abZdef|AB"));
}

// The receiver is evaluated before any argument (§22.5: call-site textual
// order), then the arguments in written order. (The string arm lowers its
// receiver before the pre-pass; the StringBuilder rows below cover the arms
// that lower it inside.)
BEZEL_TEST(builtin_member_named_arg_order_7600, receiver_then_written_order) {
    BEZEL_EXPECT_EQ(
        runTraced("    var r := R(sb, \"R\").Remove(length := T(sb, \"A\", 6), "
                  "                                 startIndex := T(sb, \"B\", 5)); "),
        std::string("hello|RAB"));
}

// The StringBuilder arms lower their receiver INSIDE the arm, after the
// written-order pre-pass; the receiver must still run FIRST (it precedes the
// argument list in the text). MEASURED on the rejected first fix: trace
// "ABRCDSEFU" (receiver after the arguments); on the base: "BARDCSFEU".
BEZEL_TEST(builtin_member_named_arg_order_7600, sb_call_receiver_then_written_order) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    sb2.Append(\"abcdef\"); "
            "    var r1 := Mk(sb, \"R\", sb2).Insert(value := TS(sb, \"A\", \"Z\"), "
            "                                     index := T(sb, \"B\", 0)).ToString(); "
            "    var r2 := Mk(sb, \"S\", sb2).Remove(length := T(sb, \"C\", 1), "
            "                                     startIndex := T(sb, \"D\", 0)).ToString(); "
            "    var r3 := Mk(sb, \"U\", sb2).Replace(newValue := TS(sb, \"E\", \"q\"), "
            "                                      oldValue := TS(sb, \"F\", \"b\")).ToString(); "
            "    Console.WriteLine(r1 + \",\" + r2 + \",\" + r3 + \"|\" + sb.ToString()); ")),
        std::string("Zabcdef,abcdef,aqcdef|RABSCDUEF"));
}

// A chained receiver: the inner call (and its arguments) runs before the
// outer call's arguments. MEASURED: base "BADC", rejected first fix "CDAB".
BEZEL_TEST(builtin_member_named_arg_order_7600, sb_chained_receiver_then_written_order) {
    BEZEL_EXPECT_EQ(
        runTraced("    sb2.Append(\"abcdef\"); "
                  "    var r := sb2.Remove(length := T(sb, \"A\", 1), "
                  "                        startIndex := T(sb, \"B\", 0))"
                  "               .Insert(value := TS(sb, \"C\", \"Z\"), "
                  "                       index := T(sb, \"D\", 0)).ToString(); "),
        std::string("Zbcdef|ABCD"));
}

// A borrowed call receiver of a reordered named call, 100x under ASan + LSan:
// the pinned receiver is lowered once and neither leaked nor released.
BEZEL_TEST(builtin_member_named_arg_order_7600, sb_call_receiver_loop_clean_under_asan) {
    JitResult r = runAotChecked(prog(
        "    var total := 0; "
        "    for var i in 0..<100 do "
        "      sb2.Clear(); sb2.Append(\"abcabc\"); "
        "      total += Mk(sb, \"R\", sb2).Insert(value := TS(sb, \"A\", \"XY\"), "
        "                                    index := T(sb, \"B\", 1)).Length; "
        "      total += Mk(sb, \"S\", sb2).Remove(length := T(sb, \"C\", 2), "
        "                                    startIndex := T(sb, \"D\", 1)).Length; "
        "    end for; "
        "    Console.WriteLine(total.ToString() + \"|\" + sb.Length.ToString() + \"|\" + "
        "                      sb.ToString().Substring(0, 6)); "));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1400|600|RABSCD\n"));
}

// A +1 `new StringBuilder(...)` receiver of a reordered named call, 100x
// under ASan: pinned first and lowered ONCE, so no use-after-free and no
// double release. LeakSanitizer is OFF for this row only: a `new
// StringBuilder(...)` temporary used only as a receiver leaks in POSITIONAL
// order too (issue #7565, open; MEASURED: the positional and the reordered
// loop leak the same 4 allocations per call).
BEZEL_TEST(builtin_member_named_arg_order_7600, new_sb_receiver_loop_no_memory_error_under_asan) {
    JitResult r = bezel::test::runAotCheckedLenient(prog(
        "    var total := 0; "
        "    for var i in 0..<100 do "
        "      total += new StringBuilder(\"abc\").Insert(value := TS(sb, \"A\", \"XY\"), "
        "                                               index := T(sb, \"B\", 1)).Length; "
        "      total += new StringBuilder(\"abcdef\").Remove(length := T(sb, \"C\", 2), "
        "                                                  startIndex := T(sb, \"D\", 1)).Length; "
        "    end for; "
        "    Console.WriteLine(total.ToString() + \"|\" + sb.Length.ToString() + \"|\" + "
        "                      sb.ToString().Substring(0, 4)); "));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("900|400|ABCD\n"));
}

// +1 temporaries (string concatenation) as reordered named arguments, 100x
// under ASan + LSan: the pinned values must neither leak nor be released twice.
// (StringBuilder.Replace / Insert with a +1 temporary string argument leaks in
// declaration order too -- a separate defect reported in the handback, not
// pinned here.)
BEZEL_TEST(builtin_member_named_arg_order_7600, plus_one_temporaries_loop_clean_under_asan) {
    JitResult r = runAotChecked(prog(
        "    var total := 0; "
        "    for var i in 0..<100 do "
        "      total += string.Repeat(count := T(sb, \"A\", 2), "
        "                             unit := TS(sb, \"B\", \"u\" + \"v\")).Length; "
        "      total += s.Remove(length := T(sb, \"C\", 1), "
        "                        startIndex := T(sb, \"D\", 0 + 2)).Length; "
        "      sb2.Clear(); sb2.Append(\"abcabc\"); "
        "      total += sb2.Replace(newValue := TS(sb, \"E\", \"Z\"), "
        "                           oldValue := TS(sb, \"F\", \"b\")).Length; "
        "    end for; "
        "    Console.WriteLine(total.ToString() + \"|\" + sb.Length.ToString()); "));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2000|600\n"));
}
