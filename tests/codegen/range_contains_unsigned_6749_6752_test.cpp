// #6749 / #6752 (spec §16.3 Normative formula, §17.5, v0.339): `Contains(v)` is
// true iff the range is nonempty under iteration and
// `min(Start, End) <= v <= max(Start, End)` (End excluded when half-open); `v in r`
// IS `r.Contains(v)`. The range path compared signed: a `byte` argument was
// sign-extended (200 read as -56), and a `ulong`/`uint` above the signed midpoint
// read as negative. Each row prints through BOTH spellings (and a stored-range
// local, which takes the non-literal path), with hits and misses at the width's
// boundaries and a signed control.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
std::string prog(const std::string& body) {
    return "module Conf;\n"
           "public class Program\n"
           "  public static method Main()\n" + body +
           "  end Main;\n"
           "end Program;\n";
}
}  // namespace

BEZEL_TEST(range_contains_unsigned_6749_6752, r1_issue_6749_repro_byte_method_spec_16_3) {
    auto r = runAot(prog(
        "    var v: byte := 200;\n"
        "    Console.WriteLine((100 .. 255).Contains(v));\n"
        "    Console.WriteLine((-100 .. 0).Contains(v));\n"
        "    var w: int := v;\n"
        "    Console.WriteLine((100 .. 255).Contains(w));\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\nFalse\nTrue\n");
}

BEZEL_TEST(range_contains_unsigned_6749_6752, r2_byte_both_spellings_and_stored_range_spec_16_3) {
    auto r = runAot(prog(
        "    var b: byte := 200;\n"
        "    var top: byte := 255;\n"
        "    var rb := 100 .. 255;\n"
        "    var lo: byte := 100;\n"
        "    var rbb := lo .. top;\n"
        "    Console.WriteLine((100 .. 255).Contains(b));\n"
        "    Console.WriteLine(b in 100 .. 255);\n"
        "    Console.WriteLine(rb.Contains(b));\n"
        "    Console.WriteLine(b in rb);\n"
        "    Console.WriteLine(rbb.Contains(b));\n"
        "    Console.WriteLine(b in rbb);\n"
        "    Console.WriteLine(b in -100 .. 0);\n"
        "    Console.WriteLine((100 ..< 200).Contains(b));\n"
        "    Console.WriteLine(b in 100 ..< 200);\n"
        "    Console.WriteLine((100 .. 200).Contains(b));\n"
        "    Console.WriteLine(top in 0 .. 255);\n"
        "    Console.WriteLine(top in 0 ..< 255);\n"
        "    Console.WriteLine(rbb.Contains(top));\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText,
        "True\nTrue\nTrue\nTrue\nTrue\nTrue\nFalse\nFalse\nFalse\nTrue\n"
        "True\nFalse\nTrue\n");
}

BEZEL_TEST(range_contains_unsigned_6749_6752, r3_ushort_and_uint_above_signed_midpoint_spec_16_3) {
    auto r = runAot(prog(
        "    var s: ushort := 50000;\n"
        "    var u: uint := 3000000000;\n"
        "    var ru := 2000000000U .. 4000000000U;\n"
        "    Console.WriteLine((40000 .. 60000).Contains(s));\n"
        "    Console.WriteLine(s in 40000 .. 60000);\n"
        "    Console.WriteLine(s in -100 .. 0);\n"
        "    Console.WriteLine((2000000000U .. 4000000000U).Contains(u));\n"
        "    Console.WriteLine(u in 2000000000U .. 4000000000U);\n"
        "    Console.WriteLine((0U .. 2000000000U).Contains(u));\n"
        "    Console.WriteLine(ru.Contains(u));\n"
        "    Console.WriteLine(u in ru);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText,
        "True\nTrue\nFalse\nTrue\nTrue\nFalse\nTrue\nTrue\n");
}

BEZEL_TEST(range_contains_unsigned_6749_6752, r4_issue_6752_repro_ulong_in_spec_16_3) {
    auto r = runAot(prog(
        "    var v: ulong := 10000000000000000000UL;\n"
        "    Console.WriteLine(v > 9000000000000000000UL);\n"
        "    Console.WriteLine(v < 11000000000000000000UL);\n"
        "    Console.WriteLine(v in 9000000000000000000UL .. 11000000000000000000UL);\n"
        "    Console.WriteLine((9000000000000000000UL .. 11000000000000000000UL).Contains(v));\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\nTrue\nTrue\nTrue\n");
}

BEZEL_TEST(range_contains_unsigned_6749_6752, r5_ulong_matrix_max_and_misses_spec_16_3) {
    auto r = runAot(prog(
        "    var l: ulong := 10000000000000000000UL;\n"
        "    var mx: ulong := 18446744073709551615UL;\n"
        "    var rl := 9000000000000000000UL .. 11000000000000000000UL;\n"
        "    var rall := 0UL .. 18446744073709551615UL;\n"
        "    Console.WriteLine(l in 0UL .. 9000000000000000000UL);\n"
        "    Console.WriteLine((0UL .. 9000000000000000000UL).Contains(l));\n"
        "    Console.WriteLine((10000000000000000001UL .. 18446744073709551615UL).Contains(l));\n"
        "    Console.WriteLine((0UL .. 18446744073709551615UL).Contains(mx));\n"
        "    Console.WriteLine(mx in 0UL .. 18446744073709551615UL);\n"
        "    Console.WriteLine(mx in 0UL ..< 18446744073709551615UL);\n"
        "    Console.WriteLine((0UL ..< 18446744073709551615UL).Contains(mx));\n"
        "    Console.WriteLine(rl.Contains(l));\n"
        "    Console.WriteLine(l in rl);\n"
        "    Console.WriteLine(rall.Contains(mx));\n"
        "    Console.WriteLine(mx in rall);\n"
        "    Console.WriteLine(rall.Contains(0UL));\n"
        "    Console.WriteLine(rl.Contains(mx));\n"
        "    Console.WriteLine(mx in rl);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText,
        "False\nFalse\nFalse\nTrue\nTrue\nFalse\nFalse\nTrue\nTrue\nTrue\n"
        "True\nTrue\nFalse\nFalse\n");
}

BEZEL_TEST(range_contains_unsigned_6749_6752, r6_signed_controls_negative_values_spec_16_3) {
    auto r = runAot(prog(
        "    var n: int := -5;\n"
        "    var q: long := -9000000000000000000L;\n"
        "    var sb: sbyte := -3;\n"
        "    Console.WriteLine((-10 .. 0).Contains(n));\n"
        "    Console.WriteLine(n in -10 .. 0);\n"
        "    Console.WriteLine((0 .. 10).Contains(n));\n"
        "    Console.WriteLine((-9223372036854775807L .. 0L).Contains(q));\n"
        "    Console.WriteLine(q in -9223372036854775807L .. 0L);\n"
        "    Console.WriteLine(q in -8000000000000000000L .. 0L);\n"
        "    Console.WriteLine((-10 .. 0).Contains(sb));\n"
        "    Console.WriteLine(sb in -10 .. 0);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText,
        "True\nTrue\nFalse\nTrue\nTrue\nFalse\nTrue\nTrue\n");
}

// §5.1 (v0.340) conversion table: `char` converts implicitly to `int`, so
// `c in -5 .. 10` is a Range<int> membership test and a `char` U+0003 is inside
// it (§16.3). The literal `in` path read `char` as an unsigned operand filling
// the compare width and compared -5 unsigned (as 4294967291), printing False.
BEZEL_TEST(range_contains_unsigned_6749_6752, r7_char_in_negative_int_bounds_spec_5_1_16_3) {
    auto r = runAot(prog(
        "    var c: char := '\\u0003';\n"
        "    var z: char := '\\u0000';\n"
        "    Console.WriteLine(c in -5 .. 10);\n"
        "    Console.WriteLine(c in -5 ..< 10);\n"
        "    Console.WriteLine(z in -5 .. 10);\n"
        "    Console.WriteLine(z in -5 ..< 10);\n"
        "    Console.WriteLine((-5 .. 10).Contains(c));\n"
        "    Console.WriteLine((-5 ..< 10).Contains(z));\n"
        "    var f: char := '\\u00FF';\n"
        "    Console.WriteLine(f in -5 .. 10);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\nTrue\nTrue\nTrue\nTrue\nTrue\nFalse\n");
}

// Char-literal bounds (a Range<char>) and a high `char` (U+FFFF) against `int`
// bounds, through the literal `in` path and the method spelling (§16.3, §17.5).
BEZEL_TEST(range_contains_unsigned_6749_6752, r8_char_literal_bounds_and_high_char_spec_16_3) {
    auto r = runAot(prog(
        "    var q: char := 'q';\n"
        "    var big: char := 'Q';\n"
        "    Console.WriteLine(q in 'a' .. 'z');\n"
        "    Console.WriteLine(q in 'a' ..< 'q');\n"
        "    Console.WriteLine(big in 'a' .. 'z');\n"
        "    Console.WriteLine(('a' .. 'z').Contains(q));\n"
        "    var h: char := '\\uFFFF';\n"
        "    Console.WriteLine(h in -5 .. 65535);\n"
        "    Console.WriteLine(h in 0 ..< 65535);\n"
        "    Console.WriteLine(h in -1 .. 70000);\n"
        "    Console.WriteLine((-5 .. 65535).Contains(h));\n"
        "    Console.WriteLine(h in -70000 .. -1);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText,
        "True\nFalse\nFalse\nTrue\nTrue\nFalse\nTrue\nTrue\nFalse\n");
}
