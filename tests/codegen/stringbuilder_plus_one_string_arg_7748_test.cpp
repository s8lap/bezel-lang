// Regression tests for #7748 -- a fresh +1 string temporary (`"X" + "Y"`, an
// interpolation, a string-returning call) passed as a `string` argument of a
// StringBuilder member leaked: 2 allocations per call, 200 over 100 iterations
// (`sb.Insert(1, "X" + "Y")`; the literal `"XY"` was clean).
//
// Language spec §28.3 (v0.350): a non-`owned` argument position is a borrowing
// use, and §28.9 / K8.1: an unbound owned (here: fresh +1 string) temporary is
// disposed at its full expression's end, after its uses. stdlib §3.1 (v0.350)
// declares `Insert(index: int, value: string)`, `Replace(oldValue: string,
// newValue: string)` and `AppendFormat(format: string, args: Array<Object>)`
// with plain (non-`owned`) `string` parameters, and each COPIES the text into the
// builder (`Append` is the transferring one, #1185, and was already clean).
//
// Every leak row runs on runAotChecked (ASan + LSan) and loops 100 times; the
// stdout carries the builder's content so a fix that released too much (a
// use-after-free or double free) fails too. The throw rows pin the exception
// edge: a raise branches past the post-call release.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

JitResult loop100(const std::string& body, const std::string& members = "") {
    return runAotChecked(
        "module M; import System; import System.Text;\n"
        "class C\n"
        "  public static method Mk(s: string): string  return s + \"!\"; end Mk;\n" +
        members +
        "  public static method Main(): int\n"
        "    using var sb := new StringBuilder();\n"
        "    var total := 0;\n"
        "    for var i in 0..<100 do\n"
        "      sb.Clear();\n"
        "      sb.Append(\"abc\");\n" +
        body +
        "      total := total + sb.Length;\n"
        "    end;\n"
        "    Console.WriteLine(total);\n"
        "    Console.WriteLine(sb.ToString());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
}

void expectClean(const JitResult& r, const char* out) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

}  // namespace

// The issue's shape: a concatenation as Insert's `value`.
BEZEL_TEST(stringbuilder_plus_one_string_arg_7748,
           insert_concat_value_loop_100_spec_28_3) {
    // "abc" -> "aXYbc": Length 5, 100 times.
    expectClean(loop100("      sb.Insert(1, \"X\" + \"Y\");\n"), "500\naXYbc\n");
}

// The same through a binding that holds the returned builder.
BEZEL_TEST(stringbuilder_plus_one_string_arg_7748,
           insert_concat_value_bound_result_loop_100_spec_28_3) {
    expectClean(loop100("      var q := sb.Insert(1, \"X\" + \"Y\");\n"
                        "      total := total + q.Length;\n"),
                "1000\naXYbc\n");
}

// Interpolation and a string-returning call are +1 producers too; named
// arguments in reversed order reach the same arm through the binding table.
BEZEL_TEST(stringbuilder_plus_one_string_arg_7748,
           insert_interpolation_and_call_values_loop_100_spec_28_3) {
    expectClean(loop100("      sb.Insert(1, $\"{i}\");\n"
                        "      sb.Insert(value := Mk(\"m\"), index := 0);\n"),
                "690\nm!a99bc\n");
}

// Both Replace operands are borrowed strings; either may be a fresh producer.
BEZEL_TEST(stringbuilder_plus_one_string_arg_7748,
           replace_both_operands_loop_100_spec_28_3) {
    // "abc": Replace("a"+"b" -> "Z!") = "Z!c"; Replace("c" -> "N"+"N") = "Z!NN".
    expectClean(loop100("      sb.Replace(\"a\" + \"b\", Mk(\"Z\"));\n"
                        "      sb.Replace(newValue := \"N\" + \"N\", oldValue := \"c\" + \"\");\n"),
                "400\nZ!NN\n");
}

// AppendFormat's `format` is a borrowed string too.
BEZEL_TEST(stringbuilder_plus_one_string_arg_7748,
           appendformat_concat_format_loop_100_spec_28_3) {
    // Hole-free formats: the content check is the literal text, and the
    // arguments array stays empty (the formats are the producers under test).
    expectClean(loop100("      var a := new Array<Object>();\n"
                        "      sb.AppendFormat(\"lit\" + \"eral\", a);\n"
                        "      sb.AppendFormat($\"[{i}]\" + \"{{}}\", a);\n"),
                "1590\nabcliteral[99]{}\n");
}

// The exception edge: a raise skips the post-call release, so the guard
// releases the producer itself (stdlib §3.1: out-of-range Insert raises
// IndexOutOfRangeException, an empty Replace `oldValue` ArgumentException).
BEZEL_TEST(stringbuilder_plus_one_string_arg_7748,
           throwing_guards_release_the_producer_loop_100_spec_28_9) {
    expectClean(
        loop100("      try\n"
                "        sb.Insert(99, \"X\" + \"Y\");\n"
                "      catch IndexOutOfRangeException =>\n"
                "        total := total + 1;\n"
                "      end try;\n"
                "      try\n"
                "        sb.Replace(\"\" + \"\", Mk(\"t\"));\n"
                "      catch ArgumentException =>\n"
                "        total := total + 1;\n"
                "      end try;\n"
                "      try\n"
                "        sb.Insert(-1, Mk(\"t\"));\n"
                "      catch IndexOutOfRangeException =>\n"
                "        total := total + 1;\n"
                "      end try;\n"),
        "600\nabc\n");
}
