// Issue #7901 (stdlib §2.2 *Math*, v0.350) -- `Math.Clamp` returns `low`
// when `value` compares less than `low`, `high` when it compares greater
// than `high`, and `value` otherwise.
//
// THE GOVERNING TEXT. stdlib §2.2 (v0.350), "What `Min`, `Max`, and `Clamp`
// compute":
//
//   "`Min(a, b)` returns `a` when `a.CompareTo(b) <= 0` and `b` otherwise;
//   `Max(a, b)` returns `a` when `a.CompareTo(b) >= 0` and `b` otherwise ...
//   `Clamp(value, low, high)` returns `low` when `value` compares less than
//   `low`, `high` when it compares greater than `high`, and `value`
//   otherwise."
//
// THE DEFECT. The INTEGER and the FLOATING arms of `Math.Clamp` did not
// implement that order: they computed the CLAMP first (`value < low ? low :
// value`) and then compared the CLAMP with `high` (`clamped > high ? high :
// clamped`). For `low <= high` the two shapes agree exactly -- the clamp can
// only differ once `low > high` -- but then the arm returned `high`, the one
// value stdlib §2.2's order NEVER returns, where the order returns `low`.
//
// MEASURED (the issue's own repros): `Math.Clamp(5, 10, 1)` printed 1 and
// `Math.Clamp(5.0, 10.0, 1.0)` printed 1, where stdlib §2.2's order gives
// 10 in both cases.
//
// The OTHER arms of the same call already implemented the order -- the
// `string`, `decimal`, `IComparable` and `DateTime` arms -- and the
// `DateTime` arm quotes the sentence verbatim in
// tests/codegen/datetime_kind_7716_test.cpp. This commit brings the integer
// and the floating arms into line with their own siblings.
//
// ORACLE. `runAot` (the AOT path). Every row asserts the EXACT STDOUT -- a
// VALUE oracle, never "no diagnostic was produced" -- so the order is
// pinned by what the call RETURNS.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

namespace {

using bezel::test::JitResult;
using bezel::test::runAot;

std::string prog(const std::string& body) {
    return "module M;\n"
           "import System;\n"
           "public class C\n"
           "  public static method Main(): int\n" + body +
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

void expectStdout(const std::string& body, const std::string& expected) {
    JitResult r = runAot(prog(body));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    std::string out = r.stdoutText;
    if (!out.empty() && out.back() == '\n') out.pop_back();
    BEZEL_EXPECT_EQ(out, expected);
}

}  // namespace

// =====================================================================
// ROW 1 -- the issue's own first repro: the INTEGER arm with
// `low > high`. stdlib §2.2's order returns `low`.
//
// Pre-fix: printed 1 (`high`).
// Post-fix: prints 10 (`low`).
// =====================================================================
BEZEL_TEST(math_clamp_low_high_order_7901,
           the_integer_arm_returns_low_when_low_is_greater_than_high) {
    expectStdout("    Console.WriteLine(Math.Clamp(5, 10, 1));\n", "10");
}

// =====================================================================
// ROW 2 -- the issue's own second repro: the FLOATING arm with
// `low > high`.
//
// Pre-fix: printed 1 (`high`).
// Post-fix: prints 10 (`low`).
// =====================================================================
BEZEL_TEST(math_clamp_low_high_order_7901,
           the_floating_arm_returns_low_when_low_is_greater_than_high) {
    expectStdout("    Console.WriteLine(Math.Clamp(5.0, 10.0, 1.0));\n", "10");
}

// =====================================================================
// ROW 3 -- the `long` arm (the same INTEGER arm at i64 width).
//
// Pre-fix: printed 1.
// Post-fix: prints 10.
// =====================================================================
BEZEL_TEST(math_clamp_low_high_order_7901,
           the_long_arm_returns_low_when_low_is_greater_than_high) {
    expectStdout("    Console.WriteLine(Math.Clamp(5L, 10L, 1L));\n", "10");
}

// =====================================================================
// ROW 4 -- a `TimeSpan` operand (one i64 tick count, stdlib §2.5) rides the
// same INTEGER arm, so the same order applies to it.
//
// Pre-fix: printed 5 (`high`).
// Post-fix: prints 10 (`low`).
// =====================================================================
BEZEL_TEST(math_clamp_low_high_order_7901,
           a_timespan_operand_returns_low_when_low_is_greater_than_high) {
    expectStdout("    Console.WriteLine(Math.Clamp(TimeSpan.FromTicks(1L), "
                 "TimeSpan.FromTicks(10L), TimeSpan.FromTicks(5L)).Ticks);\n",
                 "10");
}

// =====================================================================
// CONTROL -- the NORMAL range (`low <= high`) is untouched: the value below
// the range, inside it, and above it. A VALUE oracle, green on both arms:
// it pins that the order only differs once `low > high`.
//
// Pre-fix and post-fix: "10", "50", "100".
// =====================================================================
BEZEL_TEST(math_clamp_low_high_order_7901,
           the_normal_range_is_unchanged) {
    expectStdout("    Console.WriteLine(Math.Clamp(5, 10, 100));\n"
                 "    Console.WriteLine(Math.Clamp(50, 10, 100));\n"
                 "    Console.WriteLine(Math.Clamp(500, 10, 100));\n",
                 "10\n50\n100");
}

// =====================================================================
// CONTROL -- the `decimal` arm of the same call already implemented
// stdlib §2.2's order (it compares the VALUE with `low` first), so it is
// GREEN on both arms -- and it must STAY green: this commit changed the
// integer and the floating arms only.
//
// Pre-fix and post-fix: "10".
// =====================================================================
BEZEL_TEST(math_clamp_low_high_order_7901,
           the_decimal_arm_is_unchanged) {
    expectStdout("    Console.WriteLine(Math.Clamp(5m, 10m, 1m));\n", "10");
}

// =====================================================================
// CONTROL -- the `string` arm of the same call already implemented
// stdlib §2.2's order too (`"m"` compares less than `"z"`, so the order
// returns `low`, i.e. `"z"`). GREEN on both arms.
//
// Pre-fix and post-fix: "z".
// =====================================================================
BEZEL_TEST(math_clamp_low_high_order_7901,
           the_string_arm_is_unchanged) {
    expectStdout("    Console.WriteLine(Math.Clamp(\"m\", \"z\", \"a\"));\n", "z");
}
