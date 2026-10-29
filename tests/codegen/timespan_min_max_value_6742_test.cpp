// Regression tests for #6742: `TimeSpan.MinValue` / `TimeSpan.MaxValue` did
// not lower (zero-initialised local, empty Console.WriteLine) and an inferred
// local was not typed TimeSpan.
//
// stdlib §2.5 (v0.350): `public static readonly property MinValue: TimeSpan;
// // FromTicks(long.MinValue)` and `MaxValue: TimeSpan; // FromTicks(long.MaxValue)`.
// stdlib §2.18 (v0.350): "`TimeSpan.MinValue` renders `-10675199.02:48:05.4775808`".

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

void expectValue(const std::string& body, const std::string& expected) {
    std::string src = "module M; import System; class C "
                      "  public static method Main(): int " + body +
                      "    return 0; end Main; end C;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    std::string out = r.stdoutText;
    if (!out.empty() && out.back() == '\n') {
        out.pop_back();
        if (!out.empty() && out.back() == '\r') out.pop_back();
    }
    BEZEL_EXPECT_EQ(out, expected);
}

}  // namespace

// The issue's repro: typed local, ToString("c") and the direct ToString().
BEZEL_TEST(timespan_min_max_6742, min_value_typed_local_renders_c_form_2_18) {
    expectValue("    var f: TimeSpan := TimeSpan.MinValue; "
                "    Console.WriteLine(f.ToString(\"c\")); "
                "    Console.WriteLine(TimeSpan.MinValue.ToString());",
                "-10675199.02:48:05.4775808\n-10675199.02:48:05.4775808");
}

BEZEL_TEST(timespan_min_max_6742, max_value_inferred_local_is_timespan_2_5) {
    expectValue("    var g := TimeSpan.MaxValue; "
                "    Console.WriteLine(g.ToString(\"c\")); "
                "    Console.WriteLine(g.Ticks);",
                "10675199.02:48:05.4775807\n9223372036854775807");
}

BEZEL_TEST(timespan_min_max_6742, min_value_equals_from_ticks_long_min_2_5) {
    expectValue("    Console.WriteLine(TimeSpan.MinValue = TimeSpan.FromTicks(long.MinValue)); "
                "    Console.WriteLine(TimeSpan.MaxValue = TimeSpan.FromTicks(long.MaxValue));",
                "True\nTrue");
}

// Mirror: the other TimeSpan constant is unchanged.
BEZEL_TEST(timespan_min_max_6742, zero_constant_mirror_2_5) {
    expectValue("    var z := TimeSpan.Zero; Console.WriteLine(z.ToString()); "
                "    Console.WriteLine(z.Ticks);",
                "00:00:00\n0");
}
