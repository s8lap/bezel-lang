// Regression tests for #6948 -- `Math.Min` / `Max` / `Clamp` with an explicit
// type list and an argument whose type is not T.
//
// spec §14.5 (v0.340): with an explicit type-argument list, each argument is
// converted to T at its conversion site (an int literal or `const` narrows by
// §5.1; a wider-typed value widens by §5.1's implicit conversions).  The
// lowering compared the operands at their own LLVM types, so
// `Math.Max<long>(l, i)` and `Math.Max<byte>(b, N)` (`const N: int`) emitted
// `icmp i64, i32` and failed the IR verifier.  stdlib §2.2.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
std::string stdoutOf(const std::string& body) {
    auto r = runAot("module M; import System; class C "
                    "public static method Main(): int " + body +
                    " return 0; end Main; end C;");
    if (!r.ran) return std::string("<did-not-run>");
    std::string out;
    for (char c : r.stdoutText)
        if (c != '\r') out += c;
    return out;
}
}  // namespace

// The issue's repros.
BEZEL_TEST(math_minmax_explicit_list_6948, max_long_with_int_operand) {
    BEZEL_EXPECT_EQ(stdoutOf(
        "var l: long := 5000000000L; var i: int := 7; "
        "var r: long := Math.Max<long>(l, i); Console.WriteLine(r); "
        "var q: long := Math.Max<long>(i, l); Console.WriteLine(q);"),
        std::string("5000000000\n5000000000\n"));
}

BEZEL_TEST(math_minmax_explicit_list_6948, max_byte_with_const_int) {
    BEZEL_EXPECT_EQ(stdoutOf(
        "const N: int := 200; var b: byte := byte(3); "
        "var r: byte := Math.Max<byte>(b, N); Console.WriteLine(r); "
        "var s: byte := Math.Min<byte>(b, N); Console.WriteLine(s);"),
        std::string("200\n3\n"));
}

BEZEL_TEST(math_minmax_explicit_list_6948, min_double_with_int_operand) {
    BEZEL_EXPECT_EQ(stdoutOf(
        "var d: double := 1.5; var i: int := 7; "
        "var r: double := Math.Min<double>(d, i); Console.WriteLine(r); "
        "var q: double := Math.Max<double>(d, i); Console.WriteLine(q);"),
        std::string("1.5\n7\n"));
}

BEZEL_TEST(math_minmax_explicit_list_6948, clamp_long_with_int_bounds) {
    BEZEL_EXPECT_EQ(stdoutOf(
        "var l: long := 5; var i: int := 7; var j: int := 9; "
        "var r: long := Math.Clamp<long>(l, i, j); Console.WriteLine(r); "
        "var big: long := 50000000000L; "
        "var q: long := Math.Clamp<long>(big, i, j); Console.WriteLine(q);"),
        std::string("7\n9\n"));
}

// Signedness comes from the SOURCE type: a uint operand zero-extends (a sign
// extension would read 4000000000 as negative), an int operand sign-extends.
BEZEL_TEST(math_minmax_explicit_list_6948, widening_uses_source_signedness) {
    BEZEL_EXPECT_EQ(stdoutOf(
        "var u: uint := 4000000000u; var n: int := -5; var l: long := 3; "
        "var r: long := Math.Max<long>(l, u); Console.WriteLine(r); "
        "var q: long := Math.Min<long>(l, n); Console.WriteLine(q); "
        "var ul: ulong := 3ul; "
        "var w: ulong := Math.Max<ulong>(ul, u); Console.WriteLine(w); "
        "var d: double := 0.5; "
        "var x: double := Math.Max<double>(d, u); Console.WriteLine(x);"),
        std::string("4000000000\n-5\n4000000000\n4e+09\n"));
}

// Controls: no list, and a list whose operands are already T, keep working.
BEZEL_TEST(math_minmax_explicit_list_6948, same_type_controls) {
    BEZEL_EXPECT_EQ(stdoutOf(
        "var a: int := 3; var b: int := 9; "
        "Console.WriteLine(Math.Max(a, b)); "
        "Console.WriteLine(Math.Max<int>(a, 4)); "
        "var u: ulong := 18000000000000000000ul; var v: ulong := 3ul; "
        "var r: ulong := Math.Max(u, v); Console.WriteLine(r); "
        "var s: ulong := Math.Max<ulong>(u, v); Console.WriteLine(s);"),
        std::string("9\n4\n18000000000000000000\n18000000000000000000\n"));
}
