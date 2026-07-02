// Codegen tests for TimeSpan new members — issue #1357 / stdlib §2.5 v0.165.
//
// New members:
//   - TimeSpan.CompareTo(other: TimeSpan): int  (IComparable<TimeSpan>)
//   - TimeSpan.FromTicks(ticks: long): TimeSpan
//   - TimeSpan.ToString(): string
//
// Note: Bezel uses `=` for equality (not `==`), `and`/`or`/`not` for boolean ops.
// Each test cites the stdlib section and specific behaviour it verifies.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}

std::string prog(const std::string& body) {
    return "module M; class C public static method Main(): int " + body +
           " end Main; end C;";
}

}  // namespace

// stdlib §2.5 / #1357: TimeSpan.CompareTo — equal spans → 0.
BEZEL_TEST(codegen_timespan_compar_fromticks_1357, compareto_equal_zero) {
    BEZEL_EXPECT_EQ(run(prog(
        "var a := TimeSpan.FromSeconds(60.0); "
        "var b := TimeSpan.FromSeconds(60.0); "
        "var r := a.CompareTo(b); "
        "if r = 0 then return 1; end; "
        "return 0;"
    )), 1);
}

// stdlib §2.5 / #1357: TimeSpan.CompareTo — shorter < longer → negative.
BEZEL_TEST(codegen_timespan_compar_fromticks_1357, compareto_shorter_negative) {
    BEZEL_EXPECT_EQ(run(prog(
        "var a := TimeSpan.FromSeconds(30.0); "
        "var b := TimeSpan.FromSeconds(60.0); "
        "var r := a.CompareTo(b); "
        "if r < 0 then return 1; end; "
        "return 0;"
    )), 1);
}

// stdlib §2.5 / #1357: TimeSpan.CompareTo — longer > shorter → positive.
BEZEL_TEST(codegen_timespan_compar_fromticks_1357, compareto_longer_positive) {
    BEZEL_EXPECT_EQ(run(prog(
        "var a := TimeSpan.FromSeconds(60.0); "
        "var b := TimeSpan.FromSeconds(30.0); "
        "var r := a.CompareTo(b); "
        "if r > 0 then return 1; end; "
        "return 0;"
    )), 1);
}

// stdlib §2.5 / #1357: TimeSpan.FromTicks — identity (same as raw ticks).
// One second = 10,000,000 ticks.
BEZEL_TEST(codegen_timespan_compar_fromticks_1357, from_ticks_one_second) {
    BEZEL_EXPECT_EQ(run(prog(
        "var ts := TimeSpan.FromTicks(10000000L); "
        "if ts.Ticks = 10000000L then return 1; end; "
        "return 0;"
    )), 1);
}

// stdlib §2.5 / #1357: TimeSpan.ToString — returns a non-empty string.
// The format is "d.hh:mm:ss"; we just verify it is non-empty.
BEZEL_TEST(codegen_timespan_compar_fromticks_1357, tostring_nonempty) {
    BEZEL_EXPECT_EQ(run(prog(
        "var ts := TimeSpan.FromSeconds(90.0); "
        "var s := ts.ToString(); "
        "if s.Length > 0 then return 1; end; "
        "return 0;"
    )), 1);
}

// stdlib §2.5 / #1357: TimeSpan.Ticks already works (existing); round-trip check.
BEZEL_TEST(codegen_timespan_compar_fromticks_1357, from_ticks_roundtrip) {
    BEZEL_EXPECT_EQ(run(prog(
        "var ticks := 864000000000L; "  // 1 day
        "var ts := TimeSpan.FromTicks(ticks); "
        "if ts.Ticks = ticks then return 1; end; "
        "return 0;"
    )), 1);
}
