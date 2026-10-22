// Regression tests for #6988 (§16.6, §5.1, v0.340): a `case` STATEMENT over a
// `ulong` scrutinee rejected a constant or range bound at or above 2^63 with a
// false §5.1 error that named the folded value `-1`, while the expression form
// accepted the same bound. The statement-form gate now runs after the arm walk,
// so each bound's recorded type is read and the bound is taken as unsigned. These
// rows pin the ACCEPT side and the exact arm selection of the statement form; the
// reject side is the lit rows sema_case_stmt_ulong_range_6988__*.bzl.
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
// A statement-form `case` over a ulong local; prints "in" or "out".
std::string stmtCase(const std::string& init, const std::string& arm) {
    auto r = runAot("module M; public class Program public static method "
                    "Main() var u: ulong := " + init +
                    "; case u of " + arm +
                    " => Console.WriteLine(\"in\"), _ => Console.WriteLine(\"out\") "
                    "end case; end Main; end Program;");
    return r.ran && r.exitCode == 0 ? r.stdoutText : "FAIL:" + r.stdoutText;
}

// The same, with a class-level ulong constant K holding the ulong maximum.
std::string stmtCaseNamedMax(const std::string& init) {
    auto r = runAot("module M; public class Program public const K: ulong := "
                    "18446744073709551615UL; public static method Main() "
                    "var u: ulong := " + init +
                    "; case u of 0..K => Console.WriteLine(\"in\"), "
                    "_ => Console.WriteLine(\"out\") end case; end Main; end Program;");
    return r.ran && r.exitCode == 0 ? r.stdoutText : "FAIL:" + r.stdoutText;
}
}  // namespace

BEZEL_TEST(case_stmt_ulong_bounds_6988, ulong_max_constant_matches) {
    BEZEL_EXPECT_EQ(stmtCase("18446744073709551615UL",
                             "18446744073709551615UL"), "in\n");
}

BEZEL_TEST(case_stmt_ulong_bounds_6988, ulong_full_range_includes_small) {
    BEZEL_EXPECT_EQ(stmtCase("3", "0..18446744073709551615UL"), "in\n");
}

BEZEL_TEST(case_stmt_ulong_bounds_6988, ulong_2p63_range_includes_its_low_bound) {
    BEZEL_EXPECT_EQ(stmtCase("9223372036854775808UL",
                             "9223372036854775808UL..18446744073709551615UL"),
                    "in\n");
}

BEZEL_TEST(case_stmt_ulong_bounds_6988, ulong_2p63_range_excludes_below) {
    BEZEL_EXPECT_EQ(stmtCase("3",
                             "9223372036854775808UL..18446744073709551615UL"),
                    "out\n");
}

BEZEL_TEST(case_stmt_ulong_bounds_6988, ulong_halfopen_max_excludes_max) {
    BEZEL_EXPECT_EQ(stmtCase("18446744073709551615UL",
                             "0..<18446744073709551615UL"), "out\n");
}

BEZEL_TEST(case_stmt_ulong_bounds_6988, ulong_relop_max_matches_below) {
    BEZEL_EXPECT_EQ(stmtCase("3", "< 18446744073709551615UL"), "in\n");
}

BEZEL_TEST(case_stmt_ulong_bounds_6988, ulong_relop_max_excludes_max) {
    BEZEL_EXPECT_EQ(stmtCase("18446744073709551615UL",
                             "< 18446744073709551615UL"), "out\n");
}

BEZEL_TEST(case_stmt_ulong_bounds_6988, ulong_alt_member_high_range_matches) {
    BEZEL_EXPECT_EQ(stmtCase("18446744073709551615UL",
                             "7UL | 9223372036854775808UL..18446744073709551615UL"),
                    "in\n");
}

BEZEL_TEST(case_stmt_ulong_bounds_6988, ulong_named_constant_max_matches) {
    BEZEL_EXPECT_EQ(stmtCaseNamedMax("3"), "in\n");
}
