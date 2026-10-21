// #6720 — `TimeSpan.ToString()` (method call) renders the `c` form.
//
// Spec: stdlib §2.18 "The `TimeSpan` constant form" (v0.340): `c` is
// `[-][d.]HH:mm:ss[.fffffff]`; `-` iff negative, `d.` iff the magnitude is at
// least one day, `.fffffff` (seven digits, never trimmed) iff the sub-second
// remainder is non-zero. "The no-format `ToString()`, `ToString("c")` and an
// interpolation hole with or without `:c` produce this one rendering."
// Stdlib §2.5 declares `ToString(format: string)`; an unrecognized specifier
// raises FormatException (§2.18).
//
// Pre-fix the no-arg method call printed "0.01:30:30" (day part always, no
// fraction, each component signed) and the one-argument form was not lowered.

#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
std::string main_(const std::string& body) {
    return "module M; class C public static method Main(): int " + body +
           " return 0; end Main; end C;";
}
}  // namespace

// MinValue/MaxValue are spelled FromTicks(long.MinValue/MaxValue) here because
// the `TimeSpan.MinValue`/`MaxValue` constants themselves do not lower (filed separately).
// The five worked values of §2.18 plus the negative and zero examples, through
// the method call, the "c" overload and an interpolation hole.
BEZEL_TEST(timespan_tostring_c_form_6720, worked_values_all_three_spellings) {
    auto r = runAot(main_(
        "var a := TimeSpan.FromMinutes(90.5); "
        "var b := TimeSpan.FromDays(1.5); "
        "var c := TimeSpan.FromMilliseconds(1.5); "
        "var d := -TimeSpan.FromSeconds(90.0); "
        "var e := TimeSpan.Zero; "
        "var f := TimeSpan.FromTicks(-9223372036854775807L - 1L); "
        "var g := TimeSpan.FromTicks(9223372036854775807L); "
        "Console.WriteLine(a.ToString()); Console.WriteLine(a.ToString(\"c\")); Console.WriteLine($\"{a}\"); "
        "Console.WriteLine(b.ToString()); Console.WriteLine(b.ToString(\"c\")); Console.WriteLine($\"{b}\"); "
        "Console.WriteLine(c.ToString()); Console.WriteLine(c.ToString(\"c\")); Console.WriteLine($\"{c:c}\"); "
        "Console.WriteLine(d.ToString()); Console.WriteLine(d.ToString(\"c\")); Console.WriteLine($\"{d}\"); "
        "Console.WriteLine(e.ToString()); Console.WriteLine(e.ToString(\"c\")); Console.WriteLine($\"{e}\"); "
        "Console.WriteLine(f.ToString()); Console.WriteLine(f.ToString(\"c\")); Console.WriteLine($\"{f}\"); "
        "Console.WriteLine(g.ToString()); Console.WriteLine(g.ToString(\"c\")); Console.WriteLine($\"{g}\"); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText,
        "01:30:30\n01:30:30\n01:30:30\n"
        "1.12:00:00\n1.12:00:00\n1.12:00:00\n"
        "00:00:00.0015000\n00:00:00.0015000\n00:00:00.0015000\n"
        "-00:01:30\n-00:01:30\n-00:01:30\n"
        "00:00:00\n00:00:00\n00:00:00\n"
        "-10675199.02:48:05.4775808\n-10675199.02:48:05.4775808\n-10675199.02:48:05.4775808\n"
        "10675199.02:48:05.4775807\n10675199.02:48:05.4775807\n10675199.02:48:05.4775807\n");
}

// An unrecognized specifier raises FormatException (§2.18 strict-fault).
BEZEL_TEST(timespan_tostring_c_form_6720, unknown_specifier_raises_format_exception) {
    auto r = runAot(main_(
        "var a := TimeSpan.FromSeconds(5.0); "
        "try "
        "  Console.WriteLine(a.ToString(\"g\")); "
        "  Console.WriteLine(\"no raise\"); "
        "catch FormatException => Console.WriteLine(\"FormatException\"); "
        "end try; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "FormatException\n");
}

// Allocation: every render is a fresh +1 string (no-arg and format overloads);
// loop 200x under ASan/LSan so a leak or double free flips r.ran.
BEZEL_TEST(timespan_tostring_c_form_6720, loop_200_lsan_clean) {
    auto r = runAotChecked(
        "module M; class C public static method Main(): int "
        "  var v := TimeSpan.FromDays(1.5); "
        "  var count := 0; var i := 0; "
        "  while i < 200 do "
        "    var s := v.ToString(); "
        "    var t := v.ToString(\"c\"); "
        "    if s = \"1.12:00:00\" and t = \"1.12:00:00\" then count := count + 1; end; "
        "    i := i + 1; "
        "  end; "
        "  Console.WriteLine(count); "
        "  return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "200\n");
}
