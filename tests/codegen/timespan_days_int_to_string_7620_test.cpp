// Pin test for issue #7620 -- `var u: string := sun.Subtract(sun).Days;` is
// REJECTED: `TimeSpan.Days` is an `int`, and spec §5.1 (v0.355) "Implicit
// conversions" has no int -> string conversion, so the initializer "has a type
// that is not implicitly convertible to the target type".
//
// ALREADY FIXED on staging b52409a7 (verified by lane r39-verify-vB); this
// file only PINS it (no fail-without is available). The control row shows the
// same chain is accepted with an `int` target.
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
std::string prog(const std::string& target) {
    return "module M;\n"
           "class C\n"
           "  public static method Main(): int\n"
           "    var sun := DateTime.Now;\n"
           "    var u: " + target + " := sun.Subtract(sun).Days;\n"
           "    Console.WriteLine(u);\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}
}  // namespace

BEZEL_TEST(timespan_days_int_to_string_7620,
           string_target_rejected_spec_5_1_v0_355) {
    auto r = runAot(prog("string"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the initializer for `u` has a type that is not implicitly "
           "convertible to the target type (§5.1)"));
}

BEZEL_TEST(timespan_days_int_to_string_7620,
           int_target_accepted_spec_5_1_v0_355) {
    auto r = runAotChecked(prog("int"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0\n"));
}
