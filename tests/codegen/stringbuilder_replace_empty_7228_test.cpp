// Regression tests for #7228 (StringBuilder.Replace with an EMPTY oldValue
// silently no-op'd).
//
// #7228 -- stdlib §3.1 (v0.350), StringBuilder Contracts, verbatim:
//   "`Replace` raises `ArgumentException` when `oldValue` is empty."
//   Pre-fix: sb.Replace("", "x") printed the unchanged text and exited 0.
//   The rows pin the exception TYPE: a catch of ArgumentException returns 42,
//   a catch of IndexOutOfRangeException returns 43, so a wrong type is seen.
//
// Loop rows run on runAotChecked (ASan + LSan), 100 iterations.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

std::string replaceProg(const std::string& stmt) {
    return "module M;\n"
           "class C\n"
           "  public static method Main(): int\n"
           "    var e := \"\";\n"
           "    try\n"
           "      using var sb := new StringBuilder();\n"
           "      sb.Append(\"abc\");\n"
           "      " + stmt + ";\n"
           "      Console.WriteLine(sb.ToString());\n"
           "    catch ArgumentException => return 42;\n"
           "    catch IndexOutOfRangeException => return 43;\n"
           "    end;\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

}  // namespace

BEZEL_TEST(stringbuilder_replace_empty_7228,
           empty_old_value_literal_raises_argument_exception_stdlib_3_1) {
    JitResult r = runAot(replaceProg("sb.Replace(\"\", \"x\")"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

BEZEL_TEST(stringbuilder_replace_empty_7228,
           empty_old_value_variable_raises_argument_exception_stdlib_3_1) {
    JitResult r = runAot(replaceProg("sb.Replace(e, \"x\")"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

BEZEL_TEST(stringbuilder_replace_empty_7228,
           empty_old_value_named_args_raises_argument_exception_stdlib_3_1) {
    JitResult r = runAot(
        replaceProg("sb.Replace(newValue := \"x\", oldValue := \"\")"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

BEZEL_TEST(stringbuilder_replace_empty_7228,
           empty_new_value_is_admitted_and_deletes_stdlib_3_1) {
    JitResult r = runAot(replaceProg("sb.Replace(\"b\", \"\")"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "ac\n");
}

BEZEL_TEST(stringbuilder_replace_empty_7228,
           ordinary_replace_unchanged_stdlib_3_1) {
    JitResult r = runAot(replaceProg("sb.Replace(\"b\", \"XY\")"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "aXYc\n");
}

BEZEL_TEST(stringbuilder_replace_empty_7228,
           empty_old_value_loop_100_no_leak_stdlib_3_1) {
    JitResult r = runAotChecked(
        "module M;\n"
        "class C\n"
        "  public static method Main(): int\n"
        "    var n := 0;\n"
        "    for var i in 0..<100 do\n"
        "      try\n"
        "        using var sb := new StringBuilder();\n"
        "        sb.Append(\"abc\");\n"
        "        sb.Replace(\"\", \"x\");\n"
        "      catch ArgumentException => n := n + 1;\n"
        "      end;\n"
        "    end;\n"
        "    Console.WriteLine(n);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "100\n");
}

