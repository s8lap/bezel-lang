// Pin test for issue #7534 -- the named arguments of the static
// `string.Repeat` are evaluated in CALL-SITE (written) order, for BOTH
// declared overloads, even when the written order differs from the parameter
// order. spec §22.5 "Optional parameters and named arguments" (v0.355):
// arguments are evaluated left to right as written; stdlib §9.6 declares
// `Repeat(unit: string, count: int)` and `Repeat(c: char, count: int)`.
//
// ALREADY FIXED on staging b52409a7 (verified by lane r39-verify-vA); this
// file only PINS it, so it is not a fail-without test. The fixing commit is
// 1295324e (fix #7600: evaluate a reordered built-in member call's receiver,
// then its named arguments, in written order). That commit's own test
// (builtin_member_named_arg_order_7600_test.cpp) pins the `unit` overload with
// a trace; the `c :=` overload and the issue's literal repro are pinned here.
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

std::string prog(const std::string& unitType, const std::string& unitBody,
                 const std::string& call) {
    return "module M;\n"
           "class C\n"
           "  public static method N(): int\n"
           "    Console.WriteLine(\"N\");\n"
           "    return 3;\n"
           "  end N;\n"
           "  public static method U(): " + unitType + "\n"
           "    Console.WriteLine(\"U\");\n"
           "    return " + unitBody + ";\n"
           "  end U;\n"
           "  public static method Main(): int\n"
           "    var s := " + call + ";\n"
           "    Console.WriteLine(s);\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

}  // namespace

// The issue's repro: count := N() is written FIRST, so N prints before U even
// though `unit` is the first declared parameter.
BEZEL_TEST(string_repeat_named_arg_order_7534,
           unit_overload_count_first_spec_22_5_v0_355) {
    auto r = runAotChecked(prog("string", "\"ab\"",
                                "string.Repeat(count := N(), unit := U())"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("N\nU\nababab\n"));
}

// The `c :=` overload, same shape.
BEZEL_TEST(string_repeat_named_arg_order_7534,
           char_overload_count_first_spec_22_5_v0_355) {
    auto r = runAotChecked(prog("char", "'a'",
                                "string.Repeat(count := N(), c := U())"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("N\nU\naaa\n"));
}

// Control: written in declared order, U is evaluated before N.
BEZEL_TEST(string_repeat_named_arg_order_7534,
           unit_overload_unit_first_spec_22_5_v0_355) {
    auto r = runAotChecked(prog("string", "\"ab\"",
                                "string.Repeat(unit := U(), count := N())"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("U\nN\nababab\n"));
}
