// Regression tests for #6800 (language spec §8.5 auto-format ToString, §16.3,
// stdlib §9.8, v0.340).
//
// `Range<T>.ToString` renders `Start`/`End` as the `T?` endpoint's own text
// (§8.5 component rule; stdlib §9.8). A byte, ushort or uint endpoint above the
// signed maximum of its width was rendered from its sign-extended stored value,
// so byte 200 printed `-56`. Every element type is asserted at its sign
// boundary with the exact full line.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotOut;

namespace {

std::string rendered(const std::string& ty, const std::string& lo,
                     const std::string& hi) {
    return runAotOut(
        "module M; class C public static method Main(): int "
        "  var lo: " + ty + " := " + lo + "; "
        "  var hi: " + ty + " := " + hi + "; "
        "  var r := lo .. hi; "
        "  Console.WriteLine(r.ToString()); "
        "  return 0; "
        "end Main; end C;");
}

std::string line(const std::string& s, const std::string& e) {
    return "Range(Start: " + s + ", End: " + e +
           ", Step: 1, IsEndInclusive: True, IsOpenEnded: False, IsEmpty: False)";
}

}  // namespace

BEZEL_TEST(range_tostring_endpoint_width_6800, stdlib_9_8_byte_above_127) {
    BEZEL_EXPECT_EQ(rendered("byte", "100", "200"), line("100", "200"));
}
BEZEL_TEST(range_tostring_endpoint_width_6800, stdlib_9_8_byte_both_above_127) {
    BEZEL_EXPECT_EQ(rendered("byte", "128", "255"), line("128", "255"));
}
BEZEL_TEST(range_tostring_endpoint_width_6800, stdlib_9_8_ushort_above_32767) {
    BEZEL_EXPECT_EQ(rendered("ushort", "100", "60000"), line("100", "60000"));
}
BEZEL_TEST(range_tostring_endpoint_width_6800, stdlib_9_8_ushort_max) {
    BEZEL_EXPECT_EQ(rendered("ushort", "32768", "65535"), line("32768", "65535"));
}
BEZEL_TEST(range_tostring_endpoint_width_6800, stdlib_9_8_uint_above_2_31) {
    BEZEL_EXPECT_EQ(rendered("uint", "1000U", "4000000000U"),
                    line("1000", "4000000000"));
}
BEZEL_TEST(range_tostring_endpoint_width_6800, stdlib_9_8_uint_max) {
    BEZEL_EXPECT_EQ(rendered("uint", "2147483648U", "4294967295U"),
                    line("2147483648", "4294967295"));
}
// Controls: the other element types already rendered correctly.
BEZEL_TEST(range_tostring_endpoint_width_6800, stdlib_9_8_sbyte_negative_control) {
    BEZEL_EXPECT_EQ(rendered("sbyte", "-100", "100"), line("-100", "100"));
}
BEZEL_TEST(range_tostring_endpoint_width_6800, stdlib_9_8_short_negative_control) {
    BEZEL_EXPECT_EQ(rendered("short", "-30000", "30000"), line("-30000", "30000"));
}
BEZEL_TEST(range_tostring_endpoint_width_6800, stdlib_9_8_int_negative_control) {
    BEZEL_EXPECT_EQ(rendered("int", "-2000000000", "2000000000"),
                    line("-2000000000", "2000000000"));
}
BEZEL_TEST(range_tostring_endpoint_width_6800, stdlib_9_8_long_control) {
    BEZEL_EXPECT_EQ(rendered("long", "-5000000000L", "5000000000L"),
                    line("-5000000000", "5000000000"));
}
BEZEL_TEST(range_tostring_endpoint_width_6800, stdlib_9_8_ulong_control) {
    BEZEL_EXPECT_EQ(rendered("ulong", "9000000000000000000UL", "11000000000000000000UL"),
                    line("9000000000000000000", "11000000000000000000"));
}
BEZEL_TEST(range_tostring_endpoint_width_6800, stdlib_9_8_char_control) {
    BEZEL_EXPECT_EQ(rendered("char", "'a'", "'z'"), line("a", "z"));
}
