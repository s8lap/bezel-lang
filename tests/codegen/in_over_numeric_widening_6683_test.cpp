// #6683 (spec §17.5, §5.1, v0.339): `x in c` IS `c.Contains(x)`, and a numeric
// left operand that sema admits by implicit WIDENING (`float -> double`,
// `int -> long`, `int -> double`, `byte -> int`) must be converted to the
// element type before the per-element compare. Before the fix `f in a` with
// `f: float`, `a: Array<double>` (and `.Contains(f)`) died in LLVM IR
// verification ("Both operands to FCmp instruction are not of the same type"),
// as did `int` into `Array<double>`.
//
// Each row prints a hit AND a miss through both spellings, with a negative
// operand where the widening is signed, so a zero-extend or a missing
// conversion changes the answer.

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

BEZEL_TEST(in_over_numeric_widening_6683,
           r1_issue_repro_float_in_array_double_spec_5_1) {
    auto r = runAot(prog(
        "    var a: Array<double> := [1.5, 2.5];\n"
        "    var f: float := 2.5f;\n"
        "    Console.WriteLine(f in a);\n"
        "    Console.WriteLine(a.Contains(f));\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\nTrue\n");
}

BEZEL_TEST(in_over_numeric_widening_6683,
           r2_int_in_array_double_hit_and_miss_spec_5_1) {
    auto r = runAot(prog(
        "    var a: Array<double> := [-2.0, 4.0];\n"
        "    var i: int := -2;\n"
        "    var j: int := 3;\n"
        "    Console.WriteLine(i in a);\n"
        "    Console.WriteLine(a.Contains(i));\n"
        "    Console.WriteLine(j in a);\n"
        "    Console.WriteLine(a.Contains(j));\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\nTrue\nFalse\nFalse\n");
}

BEZEL_TEST(in_over_numeric_widening_6683,
           r3_byte_in_array_int_zero_extends_spec_5_1) {
    auto r = runAot(prog(
        "    var a: Array<int> := [200, 3];\n"
        "    var b: byte := 200;\n"
        "    Console.WriteLine(b in a);\n"
        "    Console.WriteLine(a.Contains(b));\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\nTrue\n");
}

BEZEL_TEST(in_over_numeric_widening_6683,
           r4_int_in_array_long_sign_extends_spec_5_1) {
    auto r = runAot(prog(
        "    var a: Array<long> := [-2L, 7L];\n"
        "    var i: int := -2;\n"
        "    Console.WriteLine(i in a);\n"
        "    Console.WriteLine(a.Contains(i));\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\nTrue\n");
}

BEZEL_TEST(in_over_numeric_widening_6683,
           r5_float_in_set_stack_linkedlist_double_spec_17_5) {
    auto r = runAot(prog(
        "    var f: float := 2.5f;\n"
        "    var s := new Set<double>();\n"
        "    s.Add(2.5);\n"
        "    var st := new Stack<double>();\n"
        "    st.Push(2.5);\n"
        "    var ll := new LinkedList<double>();\n"
        "    _ := ll.AddLast(2.5);\n"
        "    Console.WriteLine(f in s);\n"
        "    Console.WriteLine(s.Contains(f));\n"
        "    Console.WriteLine(f in st);\n"
        "    Console.WriteLine(st.Contains(f));\n"
        "    Console.WriteLine(f in ll);\n"
        "    Console.WriteLine(ll.Contains(f));\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\nTrue\nTrue\nTrue\nTrue\nTrue\n");
}

// The Range-literal receiver: an i8 left operand against i32 literal bounds died
// in IR verification ("Both operands to ICmp instruction are not of the same
// type") on the same widening gap. 200 pins the zero-extend.
BEZEL_TEST(in_over_numeric_widening_6683,
           r6_byte_in_int_range_literal_zero_extends_spec_17_5) {
    auto r = runAot(prog(
        "    var b: byte := 200;\n"
        "    var c: byte := 3;\n"
        "    Console.WriteLine(b in 100 .. 255);\n"
        "    Console.WriteLine(c in 100 .. 255);\n"
        "    Console.WriteLine(c in 1 .. 5);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\nFalse\nTrue\n");
}
