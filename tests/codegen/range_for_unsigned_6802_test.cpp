// #6802 (spec §16.5 Iteration, §16.4 Stride, §16.3 `Step`, v0.340): `..`
// iterates inclusively and `..<` excludes the upper bound, over the element
// type's own ordering. The counted-loop lowering of `for` compared the cursor
// with signed predicates, so a `ulong` range straddling 2^63 (or a `uint` range
// straddling 2^31) visited no values, and the Range-variable form truncated its
// endpoints to i32. §16.3: "Iteration stops silently, raising no
// OverflowException, before the first value that would pass the range's endpoint
// or fall outside T's representable range, whichever comes first" -- so a step
// past the type's maximum ends the loop and never wraps. Every row prints from
// the loop body (exact stdout).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
std::string prog(const std::string& members, const std::string& body) {
    return "module Conf;\n"
           "public class Program\n" + members +
           "  public static method Main()\n" + body +
           "  end Main;\n"
           "end Program;\n";
}
}  // namespace

BEZEL_TEST(range_for_unsigned_6802, r1_issue_6802_repro_ulong_straddle_spec_16_5) {
    auto r = runAot(prog("",
        "    var lo: ulong := 9223372036854775806UL;\n"
        "    var hi: ulong := 9223372036854775809UL;\n"
        "    var count := 0;\n"
        "    for var x in lo..hi do\n"
        "      count := count + 1;\n"
        "      Console.WriteLine(x);\n"
        "    end;\n"
        "    Console.WriteLine(count);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText,
        "9223372036854775806\n9223372036854775807\n"
        "9223372036854775808\n9223372036854775809\n4\n");
}

BEZEL_TEST(range_for_unsigned_6802, r2_uint_straddle_literal_stored_exclusive_spec_16_5) {
    auto r = runAot(prog("",
        "    var lo: uint := 2147483646U;\n"
        "    var hi: uint := 2147483649U;\n"
        "    for var x in lo..hi do Console.WriteLine(x); end;\n"
        "    Console.WriteLine(\"lit\");\n"
        "    for var x in 2147483646U .. 2147483649U do Console.WriteLine(x); end;\n"
        "    Console.WriteLine(\"excl\");\n"
        "    for var x in lo..<hi do Console.WriteLine(x); end;\n"
        "    Console.WriteLine(\"stored\");\n"
        "    var r := lo..<hi;\n"
        "    for var x in r do Console.WriteLine(x); end;\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText,
        "2147483646\n2147483647\n2147483648\n2147483649\n"
        "lit\n2147483646\n2147483647\n2147483648\n2147483649\n"
        "excl\n2147483646\n2147483647\n2147483648\n"
        "stored\n2147483646\n2147483647\n2147483648\n");
}

BEZEL_TEST(range_for_unsigned_6802, r3_ulong_stored_and_descending_spec_16_5) {
    auto r = runAot(prog("",
        "    var lo: ulong := 9223372036854775806UL;\n"
        "    var hi: ulong := 9223372036854775809UL;\n"
        "    var r := lo..hi;\n"
        "    for var x in r do Console.WriteLine(x); end;\n"
        "    Console.WriteLine(\"desc\");\n"
        "    for var x in hi..lo step -1 do Console.WriteLine(x); end;\n"
        "    Console.WriteLine(\"desc excl\");\n"
        "    for var x in hi..<lo step -1 do Console.WriteLine(x); end;\n"
        "    Console.WriteLine(\"desc stored\");\n"
        "    var rd := hi..lo step -1;\n"
        "    for var x in rd do Console.WriteLine(x); end;\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText,
        "9223372036854775806\n9223372036854775807\n"
        "9223372036854775808\n9223372036854775809\n"
        "desc\n9223372036854775809\n9223372036854775808\n"
        "9223372036854775807\n9223372036854775806\n"
        "desc excl\n9223372036854775809\n9223372036854775808\n9223372036854775807\n"
        "desc stored\n9223372036854775809\n9223372036854775808\n"
        "9223372036854775807\n9223372036854775806\n");
}

BEZEL_TEST(range_for_unsigned_6802, r4_endpoints_at_type_maximum_terminate_spec_16_3) {
    auto r = runAot(prog("",
        "    var m: ulong := 18446744073709551615UL;\n"
        "    var m2: ulong := 18446744073709551613UL;\n"
        "    for var x in m2..m do Console.WriteLine(x); end;\n"
        "    Console.WriteLine(\"uint\");\n"
        "    var um: uint := 4294967295U;\n"
        "    var um2: uint := 4294967293U;\n"
        "    for var x in um2..um do Console.WriteLine(x); end;\n"
        "    Console.WriteLine(\"ushort\");\n"
        "    var sm: ushort := 65535;\n"
        "    var sm2: ushort := 65534;\n"
        "    for var x in sm2..sm do Console.WriteLine(x); end;\n"
        "    Console.WriteLine(\"byte\");\n"
        "    var bm: byte := 255;\n"
        "    var bm2: byte := 253;\n"
        "    for var x in bm2..bm do Console.WriteLine(x); end;\n"
        "    Console.WriteLine(\"byte straddle\");\n"
        "    var b3: byte := 126;\n"
        "    var b4: byte := 129;\n"
        "    for var x in b3..b4 do Console.WriteLine(x); end;\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText,
        "18446744073709551613\n18446744073709551614\n18446744073709551615\n"
        "uint\n4294967293\n4294967294\n4294967295\n"
        "ushort\n65534\n65535\n"
        "byte\n253\n254\n255\n"
        "byte straddle\n126\n127\n128\n129\n");
}

BEZEL_TEST(range_for_unsigned_6802, r5_step_past_the_maximum_stops_without_wrapping_spec_16_3) {
    auto r = runAot(prog("",
        "    var big: ulong := 18446744073709551614UL;\n"
        "    var m: ulong := 18446744073709551615UL;\n"
        "    var z: ulong := 0;\n"
        "    for var x in big..m step 4611686018427387904L do Console.WriteLine(x); end;\n"
        "    Console.WriteLine(\"from zero\");\n"
        "    for var x in z..m step 4611686018427387904L do Console.WriteLine(x); end;\n"
        "    Console.WriteLine(\"step 2 to max\");\n"
        "    var m2: ulong := 18446744073709551613UL;\n"
        "    for var x in m2..m step 2 do Console.WriteLine(x); end;\n"
        "    var b0: byte := 0;\n"
        "    var b10: byte := 10;\n"
        "    var b200: byte := 200;\n"
        "    var b255: byte := 255;\n"
        "    Console.WriteLine(\"byte 0..10 step 256\");\n"
        "    for var x in b0..b10 step 256 do Console.WriteLine(x); end;\n"
        "    Console.WriteLine(\"byte 0..255 step 128\");\n"
        "    for var x in b0..b255 step 128 do Console.WriteLine(x); end;\n"
        "    Console.WriteLine(\"byte 200..255 step 100\");\n"
        "    for var x in b200..b255 step 100 do Console.WriteLine(x); end;\n"
        "    Console.WriteLine(\"byte runtime step\");\n"
        "    var s: int := 128;\n"
        "    for var x in b0..b255 step s do Console.WriteLine(x); end;\n"
        "    var sn: int := -100;\n"
        "    for var x in b255..b0 step sn do Console.WriteLine(x); end;\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText,
        "18446744073709551614\n"
        "from zero\n0\n4611686018427387904\n9223372036854775808\n13835058055282163712\n"
        "step 2 to max\n18446744073709551613\n18446744073709551615\n"
        "byte 0..10 step 256\n0\n"
        "byte 0..255 step 128\n0\n128\n"
        "byte 200..255 step 100\n200\n"
        "byte runtime step\n0\n128\n255\n155\n55\n");
}

BEZEL_TEST(range_for_unsigned_6802, r6_unsigned_descent_stops_at_zero_spec_16_4) {
    auto r = runAot(prog("",
        "    var u2: uint := 2;\n"
        "    var u0: uint := 0;\n"
        "    for var x in u2..u0 step -1 do Console.WriteLine(x); end;\n"
        "    Console.WriteLine(\"ulong\");\n"
        "    var l3: ulong := 3;\n"
        "    var l0: ulong := 0;\n"
        "    for var x in l3..l0 step -2 do Console.WriteLine(x); end;\n"
        "    Console.WriteLine(\"excl\");\n"
        "    for var x in l3..<l0 step -1 do Console.WriteLine(x); end;\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText,
        "2\n1\n0\nulong\n3\n1\nexcl\n3\n2\n1\n");
}

BEZEL_TEST(range_for_unsigned_6802, r7_iterator_method_ulong_straddle_spec_16_5) {
    auto r = runAot(prog(
        "  public static method Gen(lo: ulong, hi: ulong): IEnumerable<ulong>\n"
        "    for var x in lo..hi do yield return x; end;\n"
        "  end Gen;\n",
        "    for var v in Gen(9223372036854775806UL, 9223372036854775809UL) do\n"
        "      Console.WriteLine(v);\n"
        "    end;\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText,
        "9223372036854775806\n9223372036854775807\n"
        "9223372036854775808\n9223372036854775809\n");
}

BEZEL_TEST(range_for_unsigned_6802, r8_signed_controls_spec_16_5) {
    auto r = runAot(prog("",
        "    var lm: long := -9223372036854775807L;\n"
        "    var lx: long := 9223372036854775807L;\n"
        "    for var x in lm..lx step 4611686018427387904L do Console.WriteLine(x); end;\n"
        "    Console.WriteLine(\"short\");\n"
        "    var sh: short := 32766;\n"
        "    var sh2: short := 32767;\n"
        "    for var x in sh..sh2 do Console.WriteLine(x); end;\n"
        "    Console.WriteLine(\"sbyte\");\n"
        "    var sb: sbyte := -127;\n"
        "    var sb2: sbyte := -128;\n"
        "    for var x in sb..sb2 step -1 do Console.WriteLine(x); end;\n"
        "    Console.WriteLine(\"stored long across 2^31\");\n"
        "    var a: long := 2147483646L;\n"
        "    var b: long := 2147483649L;\n"
        "    var r := a..b;\n"
        "    for var x in r do Console.WriteLine(x); end;\n"
        "    Console.WriteLine(\"neg\");\n"
        "    for var x in -2..1 do Console.WriteLine(x); end;\n"
        "    for var x in 3..0 step -1 do Console.WriteLine(x); end;\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText,
        "-9223372036854775807\n-4611686018427387903\n1\n4611686018427387905\n"
        "short\n32766\n32767\n"
        "sbyte\n-127\n-128\n"
        "stored long across 2^31\n2147483646\n2147483647\n2147483648\n2147483649\n"
        "neg\n-2\n-1\n0\n1\n3\n2\n1\n0\n");
}

// §16.3: the iteration stops "before the first value that would ... fall outside
// T's representable range". A signed range ending at `int.MaxValue` used to
// wrap to MinValue and run forever.
BEZEL_TEST(range_for_unsigned_6802, r9_signed_max_and_min_terminate_spec_16_3) {
    auto r = runAot(prog("",
        "    var im: int := 2147483647;\n"
        "    var im2: int := 2147483645;\n"
        "    for var x in im2..im do Console.WriteLine(x); end;\n"
        "    Console.WriteLine(\"min\");\n"
        "    var imn: int := -2147483648;\n"
        "    var imn2: int := -2147483646;\n"
        "    for var x in imn2..imn step -1 do Console.WriteLine(x); end;\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText,
        "2147483645\n2147483646\n2147483647\n"
        "min\n-2147483646\n-2147483647\n-2147483648\n");
}
