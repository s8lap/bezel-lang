// Spec-derived regression tests for fix #2024 — `int.Parse(s, radix)` (the
// two-argument overload) silently returned wrong values on invalid/overflow
// input instead of raising FormatException / OverflowException as the §9
// strict-fault model requires.
//
// Authority: stdlib §9.1 `Parse(s: string, radix: int)` (v0.197) and the
// §9.3 strict-fault model (specified for §9.1 by v0.137 — "silent tolerance
// hides data problems; a caller that means to accept padded input trims
// explicitly"). The single-arg overload already rejects malformed/range
// input correctly (pinned by parse_overloads_747_test.cpp); the 2-arg
// overload was uncovered on those edges.
//
// Root cause: the 2-arg codegen branch (LlvmBackend_Calls2.cpp) called
// `strtol(data, NULL, radix)` with a NULL endptr and no range-check, then
// silently truncated the result. The 1-arg branch uses strtoll/strtoull with
// an endptr slot and per-width range-check; the fix mirrors that path.
//
// Each Parse test below: returns 1 from a `catch FormatException`/
// `catch OverflowException` arm (caught = correct) and 0 from the no-throw
// path (no throw = bug). The `try`-form tests assert the matching TryParse
// shape-(c) contract: false + default out on every input Parse would reject.

#include "support/aot_support.hpp"   // runAot — AOT test harness
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ── §9.1: VALID Parse(s, radix) still returns the correct value ──────────────
// Pre-existing behavior; the fix must not regress it.

// int.Parse("ff", 16) = 255.
BEZEL_TEST(int_parse_radix_2024, hex_valid) {
    // stdlib §9.1 Parse(s, radix): valid base-16 input parses to its decimal
    // value, no throw.
    BEZEL_EXPECT_EQ(run(
        "module M; static class F public static method Main(): int "
        "  return int.Parse(\"ff\", 16); "  // 255
        "end Main; end F;"), 255);
}

// int.Parse("101", 2) = 5.
BEZEL_TEST(int_parse_radix_2024, binary_valid) {
    BEZEL_EXPECT_EQ(run(
        "module M; static class F public static method Main(): int "
        "  return int.Parse(\"101\", 2); "  // 5
        "end Main; end F;"), 5);
}

// int.Parse("17", 8) = 15.
BEZEL_TEST(int_parse_radix_2024, octal_valid) {
    BEZEL_EXPECT_EQ(run(
        "module M; static class F public static method Main(): int "
        "  return int.Parse(\"17\", 8); "  // 15
        "end Main; end F;"), 15);
}

// ── §9.1 / §9.3: INVALID digits → FormatException ─────────────────────────────
// #2024: was silently returning 0.

// int.Parse("2", 2) — '2' is not a valid base-2 digit → FormatException.
BEZEL_TEST(int_parse_radix_2024, invalid_digit_binary_throws) {
    // stdlib §9.1: '2' is out of the base-2 vocabulary; strict-fault model
    // (§9.3) makes this a FormatException, not a silent 0.
    BEZEL_EXPECT_EQ(run(
        "module M; static class F public static method Main(): int "
        "  try "
        "    var x := int.Parse(\"2\", 2); return 0; "  // no throw = bug
        "  catch var e: FormatException => return 1; "  // caught = correct
        "  end; "
        "end Main; end F;"), 1);
}

// int.Parse("g", 16) — 'g' is not a valid base-16 digit → FormatException.
BEZEL_TEST(int_parse_radix_2024, invalid_digit_hex_throws) {
    BEZEL_EXPECT_EQ(run(
        "module M; static class F public static method Main(): int "
        "  try "
        "    var x := int.Parse(\"g\", 16); return 0; "
        "  catch var e: FormatException => return 1; "
        "  end; "
        "end Main; end F;"), 1);
}

// int.Parse("", 10) — empty string → FormatException.
BEZEL_TEST(int_parse_radix_2024, empty_string_throws) {
    // stdlib §9.1: empty input is not a valid integer representation in any
    // base; FormatException per the strict-fault model.
    BEZEL_EXPECT_EQ(run(
        "module M; static class F public static method Main(): int "
        "  try "
        "    var x := int.Parse(\"\", 10); return 0; "
        "  catch var e: FormatException => return 1; "
        "  end; "
        "end Main; end F;"), 1);
}

// int.Parse("10x", 16) — trailing garbage → FormatException.
BEZEL_TEST(int_parse_radix_2024, trailing_garbage_throws) {
    // stdlib §9.1: every character in the string must be part of the numeric
    // representation; trailing non-numeric chars raise FormatException.
    // The old code returned 16 (parsed "10", ignored "x").
    BEZEL_EXPECT_EQ(run(
        "module M; static class F public static method Main(): int "
        "  try "
        "    var x := int.Parse(\"10x\", 16); return 0; "
        "  catch var e: FormatException => return 1; "
        "  end; "
        "end Main; end F;"), 1);
}

// ── §9.1: OUT-OF-RANGE → OverflowException ───────────────────────────────────
// #2024: was silently truncating to the low bits.

// byte.Parse("100", 16) = 0x100 = 256 > byte.MaxValue(255) → OverflowException.
BEZEL_TEST(int_parse_radix_2024, byte_overflow_throws) {
    // stdlib §9.1: a value outside the target type's representable range
    // raises OverflowException, not silent truncation to 0.
    BEZEL_EXPECT_EQ(run(
        "module M; static class F public static method Main(): int "
        "  try "
        "    var b := byte.Parse(\"100\", 16); return 0; "
        "  catch var e: OverflowException => return 1; "
        "  end; "
        "end Main; end F;"), 1);
}

// byte.Parse("-1", 10) — negative into an unsigned type → OverflowException.
BEZEL_TEST(int_parse_radix_2024, byte_negative_throws) {
    // stdlib §9.1: a negative value is out of range for `byte` (lo = 0); the
    // old code silently wrapped to 255.
    BEZEL_EXPECT_EQ(run(
        "module M; static class F public static method Main(): int "
        "  try "
        "    var b := byte.Parse(\"-1\", 10); return 0; "
        "  catch var e: OverflowException => return 1; "
        "  end; "
        "end Main; end F;"), 1);
}

// int.Parse("80000000", 16) — 0x80000000 = 2147483648 > int.MaxValue → OverflowException.
BEZEL_TEST(int_parse_radix_2024, int_hex_overflow_throws) {
    // stdlib §9.1: 0x80000000 exceeds int.MaxValue; OverflowException, not
    // silent wrap to the negative representation.
    BEZEL_EXPECT_EQ(run(
        "module M; static class F public static method Main(): int "
        "  try "
        "    var x := int.Parse(\"80000000\", 16); return 0; "
        "  catch var e: OverflowException => return 1; "
        "  end; "
        "end Main; end F;"), 1);
}

// ── §9.1: ulong.Parse("-1", 10) — leading '-' into unsigned 64-bit ───────────
// strtoull silently negates; the fix rejects leading '-' for the ulong width
// as a FormatException (mirrors the TryParse branch above).

// ulong.Parse("-1", 10) → FormatException.
BEZEL_TEST(int_parse_radix_2024, ulong_negative_throws_format) {
    BEZEL_EXPECT_EQ(run(
        "module M; static class F public static method Main(): int "
        "  try "
        "    var x := ulong.Parse(\"-1\", 10); return 0; "
        "  catch var e: FormatException => return 1; "
        "  end; "
        "end Main; end F;"), 1);
}
