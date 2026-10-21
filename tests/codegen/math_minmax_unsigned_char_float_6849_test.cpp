// Regression tests for #6849, #6864 and #6865 -- `Math.Min` / `Math.Max` /
// `Math.Clamp` at `float`, at the unsigned integer types, and at `char`.
//
// WHAT THE DOCUMENTS REQUIRE.  stdlib §2.2 (v0.340) declares
//     public static method Min<T>(a: T, b: T): T where T: IComparable<T>;
// (and `Max`, `Clamp` alike).  "Ordering throughout is `T`'s `CompareTo`":
// `Min(a, b)` returns `a` when `a.CompareTo(b) <= 0` and `b` otherwise.  The
// call's type is T.  byte, ushort, uint, ulong, float and char all conform to
// `IComparable<T>` at their own type (stdlib §2.8, §9.2, §9.5).
//
// THE DEFECTS (all one lowering site, two sema/codegen causes):
//   #6849  float: the FP fallback `sitofp`-ed a float operand (malformed IR) and
//          the float result call was untyped, so a consumer read it as double.
//   #6864  uint (and byte/ushort/ulong): the integer arm used signed compares,
//          and the call kept an `int` placeholder type, so the result printed
//          as a signed int (Max(3u, 4000000000u) chose 3).
//   #6865  char: the call kept the `int` placeholder, so the result printed as
//          its code point (97), not `a`.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
std::string stdoutOf(const std::string& source) {
    auto r = runAot(source);
    if (!r.ran) return std::string("<did-not-run>");
    std::string out;
    for (char c : r.stdoutText)
        if (c != '\r') out += c;
    return out;
}

std::string program(const std::string& body) {
    return "module M; import System; class C public static method Main(): int " +
           body + " return 0; end Main; end C;";
}
}  // namespace

// #6849 -- the issue's repro (Min) and its Max twin, plus Clamp.
BEZEL_TEST(math_minmax_float_6849, min_max_clamp_float) {
    BEZEL_EXPECT_EQ(stdoutOf(program(
        "var f: float := 7.5f; "
        "Console.WriteLine(Math.Min(f, 2.0f)); "
        "Console.WriteLine(Math.Max(f, 2.0f)); "
        "Console.WriteLine(Math.Clamp(f, 1.0f, 3.0f)); "
        "Console.WriteLine(Math.Clamp(-4.5f, -1.5f, 3.0f)); ")),
        std::string("2\n7.5\n3\n-1.5\n"));
}

// #6849 -- the result is a float: it binds to a float variable and an
// unannotated `var` prints as a float (not through the double renderer).
BEZEL_TEST(math_minmax_float_6849, result_is_float_typed) {
    BEZEL_EXPECT_EQ(stdoutOf(program(
        "var f: float := 7.5f; "
        "var m: float := Math.Min(f, 2.5f); "
        "var x := Math.Max(f, 2.5f); "
        "Console.WriteLine(m); Console.WriteLine(x); ")),
        std::string("2.5\n7.5\n"));
}

// #6864 -- the issue's repro, and the control from the issue (variables).
BEZEL_TEST(math_minmax_uint_6864, max_min_clamp_uint_above_int_max) {
    BEZEL_EXPECT_EQ(stdoutOf(program(
        "Console.WriteLine(Math.Max(3u, 4000000000u)); "
        "var a: uint := 4000000000; var b: uint := 3; "
        "Console.WriteLine(Math.Max(a, b)); "
        "Console.WriteLine(Math.Min(a, b)); "
        "Console.WriteLine(Math.Clamp(4294967295u, 5u, 4000000000u)); "
        "Console.WriteLine(Math.Clamp(1u, 5u, 4000000000u)); ")),
        std::string("4000000000\n4000000000\n3\n4000000000\n5\n"));
}

// #6864 -- the sibling unsigned types, at the values whose top bit is set.
BEZEL_TEST(math_minmax_uint_6864, byte_ushort_ulong_boundaries) {
    BEZEL_EXPECT_EQ(stdoutOf(program(
        "Console.WriteLine(Math.Max(byte(1), byte(200))); "
        "Console.WriteLine(Math.Min(byte(1), byte(200))); "
        "Console.WriteLine(Math.Max(ushort(3), ushort(60000))); "
        "Console.WriteLine(Math.Min(ushort(3), ushort(60000))); "
        "Console.WriteLine(Math.Max(0ul, 18446744073709551615ul)); "
        "Console.WriteLine(Math.Min(0ul, 18446744073709551615ul)); ")),
        std::string("200\n1\n60000\n3\n18446744073709551615\n0\n"));
}

// Control: the signed types keep signed ordering.
BEZEL_TEST(math_minmax_uint_6864, signed_types_unchanged) {
    BEZEL_EXPECT_EQ(stdoutOf(program(
        "Console.WriteLine(Math.Max(sbyte(-128), sbyte(127))); "
        "Console.WriteLine(Math.Min(short(-300), short(30000))); "
        "Console.WriteLine(Math.Min(-5, 2000000000)); "
        "Console.WriteLine(Math.Max(-5L, 9000000000L)); ")),
        std::string("127\n-300\n-5\n9000000000\n"));
}

// #6865 -- the issue's repro: the result is a char, printed as one.
BEZEL_TEST(math_minmax_char_6865, min_max_clamp_char_print_as_char) {
    BEZEL_EXPECT_EQ(stdoutOf(program(
        "Console.WriteLine(Math.Min('a', 'b')); "
        "Console.WriteLine(Math.Max('z', 'a')); "
        "Console.WriteLine(Math.Clamp('z', 'a', 'f')); "
        "Console.WriteLine(Math.Min<char>('a', 'b')); ")),
        std::string("a\nz\nf\na\n"));
}
