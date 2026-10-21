// Regression tests for #6916 (§19.3, §5.1, v0.340): a `case` constant /
// range-bound over a 64-bit integer scrutinee was never range-checked, so
// `case u of -5..10` over a `ulong` compiled and compared against the wrapped
// 2^64-5 (answer `out` for u = 3), where the same pattern over a `uint` is
// the §5.1 narrowing error. The fix gates `ulong` to [0, 2^64-1] and `long`
// to [-2^63, 2^63-1]. These rows pin the ACCEPT side the gate must keep: a
// `ulong`-typed constant above 2^63-1 folds to a wrapped negative long long
// and must NOT be mistaken for a negative bound. The reject side is the lit
// rows sema_case_pattern_range__*_6916.bzl.
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
std::string out(const std::string& ty, const std::string& init,
                const std::string& arm) {
    auto r = runAot("module M; public class Program public static method "
                    "Main() var u: " + ty + " := " + init +
                    "; Console.WriteLine((case u of " + arm +
                    " => \"in\", _ => \"out\")); end Main; end Program;");
    return r.ran && r.exitCode == 0 ? r.stdoutText : "FAIL:" + r.stdoutText;
}
}  // namespace

BEZEL_TEST(case_pattern_ulong_bounds_6916, ulong_max_constant_matches) {
    BEZEL_EXPECT_EQ(out("ulong", "18446744073709551615UL",
                        "18446744073709551615UL"), "in\n");
}

BEZEL_TEST(case_pattern_ulong_bounds_6916, ulong_full_range_includes_small) {
    BEZEL_EXPECT_EQ(out("ulong", "3", "0..18446744073709551615UL"), "in\n");
}

BEZEL_TEST(case_pattern_ulong_bounds_6916, ulong_relop_high_bound) {
    BEZEL_EXPECT_EQ(out("ulong", "3", "< 18446744073709551615UL"), "in\n");
}

BEZEL_TEST(case_pattern_ulong_bounds_6916, ulong_in_range_still_matches) {
    BEZEL_EXPECT_EQ(out("ulong", "3", "0..10"), "in\n");
}

BEZEL_TEST(case_pattern_ulong_bounds_6916, long_negative_constant_matches) {
    BEZEL_EXPECT_EQ(out("long", "-3", "-9223372036854775807L..-1"), "in\n");
}
