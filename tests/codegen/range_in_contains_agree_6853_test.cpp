// #6853 (spec §17.5, §16.3 Normative formula, §5.1, v0.340): `x in r` IS
// `r.Contains(x)`, so over one range the two spellings must give one answer.
//
// MEASURED on the pre-fix base (`b470e397`): with `r = -5 .. 10U` and `int`
// operands, `x in -5 .. 10U` printed False for -1 and 0 while
// `(-5 .. 10U).Contains(x)` printed True. The closed stepless literal had its
// own inline lowering that took the compare signedness from the THREE operand
// expressions (any unsigned-looking one forced the unsigned predicates), while
// `Contains` keys it on the range's element type; both now lower through
// `emitRangeContains`.
//
// Each row prints the literal `in`, the stored-range `in`, and `Contains`.

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

// The issue's repro 1, verbatim values: -1 and 0 lie in [-5, 10].
BEZEL_TEST(range_in_contains_agree_6853, issue_repro_minus5_to_10U_int_operands_spec_17_5) {
    auto r = runAot(prog(
        "    var x0: int := -2147483648;\n"
        "    Console.WriteLine(x0 in -5 .. 10U);\n"
        "    Console.WriteLine((-5 .. 10U).Contains(x0));\n"
        "    var x1: int := -1;\n"
        "    Console.WriteLine(x1 in -5 .. 10U);\n"
        "    Console.WriteLine((-5 .. 10U).Contains(x1));\n"
        "    var x2: int := 0;\n"
        "    Console.WriteLine(x2 in -5 .. 10U);\n"
        "    Console.WriteLine((-5 .. 10U).Contains(x2));\n"
        "    var x3: int := 2147483647;\n"
        "    Console.WriteLine(x3 in -5 .. 10U);\n"
        "    Console.WriteLine((-5 .. 10U).Contains(x3));\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText,
        "False\nFalse\nTrue\nTrue\nTrue\nTrue\nFalse\nFalse\n");
}

// Boundaries and negatives, inclusive and exclusive, literal / stored / Contains.
BEZEL_TEST(range_in_contains_agree_6853, boundaries_three_spellings_agree_spec_17_5) {
    auto r = runAot(prog(
        "    var r := -5 .. 10U;\n"
        "    var h := -5 ..< 10U;\n"
        "    for var v in [-6, -5, -1, 0, 9, 10, 11] do\n"
        "      Console.WriteLine($\"{v} {v in -5 .. 10U} {v in r} {r.Contains(v)} \" +\n"
        "                        $\"{v in -5 ..< 10U} {v in h} {h.Contains(v)}\");\n"
        "    end;\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText,
        "-6 False False False False False False\n"
        "-5 True True True True True True\n"
        "-1 True True True True True True\n"
        "0 True True True True True True\n"
        "9 True True True True True True\n"
        "10 True True True False False False\n"
        "11 False False False False False False\n");
}

// Control: the unsigned element type keeps its unsigned compare in all three
// spellings (#6749 / #6752): 4294967295 is above the signed midpoint.
BEZEL_TEST(range_in_contains_agree_6853, uint_range_above_signed_midpoint_agree_spec_16_3) {
    auto r = runAot(prog(
        "    var u: uint := 4294967295;\n"
        "    var r := 4000000000U .. 4294967295U;\n"
        "    Console.WriteLine(u in 4000000000U .. 4294967295U);\n"
        "    Console.WriteLine(u in r);\n"
        "    Console.WriteLine(r.Contains(u));\n"
        "    var z: uint := 0;\n"
        "    Console.WriteLine(z in 4000000000U .. 4294967295U);\n"
        "    Console.WriteLine(r.Contains(z));\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\nTrue\nTrue\nFalse\nFalse\n");
}

// A narrower operand widens into the range's element type (§5.1): `short` and
// `byte` against an `int` range with a negative bound.
BEZEL_TEST(range_in_contains_agree_6853, narrow_operands_widen_into_int_range_spec_5_1) {
    auto r = runAot(prog(
        "    var s: short := -32768;\n"
        "    var b: byte := 255;\n"
        "    Console.WriteLine(s in -40000 .. 10U);\n"
        "    Console.WriteLine((-40000 .. 10U).Contains(s));\n"
        "    Console.WriteLine(b in -5 .. 10U);\n"
        "    Console.WriteLine((-5 .. 10U).Contains(b));\n"
        "    Console.WriteLine(b in -5 .. 255);\n"
        "    Console.WriteLine((-5 .. 255).Contains(b));\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText,
        "True\nTrue\nFalse\nFalse\nTrue\nTrue\n");
}

// fix-forward #6853 (§17.5, §16.3 Normative formula, §16.6, v0.340): an
// operand admitted against a constant-endpoint literal whose `T` differs from
// its own type in signedness is compared on the MATHEMATICAL values. A `ulong`
// above `long`'s range is not negative (`in 31 ..` holds, `in ..< 5` does
// not), a negative `long` is not huge (`in 5UL ..` does not hold), and a
// `uint` end above `int`'s range keeps its value (`0 .. 3000000000U`).
BEZEL_TEST(range_in_contains_agree_6853, cross_signedness_literal_operands_spec_16_3) {
    auto r = runAot(prog(
        "    var g: ulong := 18446744073709551615UL;\n"
        "    Console.WriteLine($\"{g in 31 ..} {(31 ..).Contains(g)} {g in ..< 5} {(..< 5).Contains(g)}\");\n"
        "    g := 9223372036854775808UL;\n"
        "    Console.WriteLine($\"{g in 10 ..} {(10 ..).Contains(g)} {g in 0 .. 10}\");\n"
        "    var l: long := -1;\n"
        "    Console.WriteLine($\"{l in 5UL ..} {(5UL ..).Contains(l)} {l in 5UL .. 10UL}\");\n"
        "    l := 7;\n"
        "    Console.WriteLine($\"{l in 5UL ..} {(5UL .. 10UL).Contains(l)}\");\n"
        "    var i: int := 5;\n"
        "    Console.WriteLine($\"{i in 0 .. 3000000000U} {(0 .. 3000000000U).Contains(i)}\");\n"
        "    i := -1;\n"
        "    Console.WriteLine($\"{i in 0 .. 3000000000U} {(0 .. 3000000000U).Contains(i)}\");\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText,
        "True True False False\n"
        "True True False\n"
        "False False False\n"
        "True True\n"
        "True True\n"
        "False False\n");
}

// fix-forward #6853 (§17.5, §7.4, v0.340): `(lo .. hi).Contains(m)` over a
// §7.4-narrowed nullable agrees with `m in lo .. hi` — both see the scalar.
BEZEL_TEST(range_in_contains_agree_6853, narrowed_nullable_contains_agrees_with_in_spec_17_5) {
    auto r = runAot(prog(
        "    var m: int? := 3;\n"
        "    if m <> null then\n"
        "      Console.WriteLine($\"{m in 1 .. 5} {(1 .. 5).Contains(m)} {(4 .. 5).Contains(m)}\");\n"
        "    end;\n"
        "    var k: long? := 6;\n"
        "    if k <> null then\n"
        "      Console.WriteLine($\"{k in 1 .. 6} {(1 .. 6).Contains(k)} {(1 ..< 6).Contains(k)}\");\n"
        "    end;\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "True True False\nTrue True False\n");
}
