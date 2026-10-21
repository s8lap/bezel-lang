// Regression tests for #6908 -- `Math.Min` / `Math.Max` / `Math.Clamp` inside a
// generic body instantiated at an unsigned type.
//
// stdlib §2.2 (v0.340): `Max<T>(a: T, b: T): T where T: IComparable<T>` orders
// by T's own CompareTo.  The non-generic unsigned fix (#6864) keyed the
// unsigned predicate on the operand's static type; inside `Big<T>` that type
// is `T`, so uint/byte/ushort/ulong instantiations took the signed compare.

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

const char* kHelpers =
    "module M; import System; class C "
    "public static method Big<T>(a: T, b: T): T where T: IComparable<T> "
    "return Math.Max<T>(a, b); end Big; "
    "public static method Big2<T>(a: T, b: T): T where T: IComparable<T> "
    "return Math.Max(a, b); end Big2; "
    "public static method Sm<T>(a: T, b: T): T where T: IComparable<T> "
    "return Math.Min(a, b); end Sm; "
    "public static method Cl<T>(a: T, b: T, c: T): T where T: IComparable<T> "
    "return Math.Clamp(a, b, c); end Cl; "
    "public static method Main(): int ";
}  // namespace

// The issue's repro, every unsigned width and char.
BEZEL_TEST(math_minmax_generic_unsigned_6908, max_at_every_unsigned_width) {
    BEZEL_EXPECT_EQ(stdoutOf(std::string(kHelpers) +
        "Console.WriteLine(Big<uint>(3u, 4000000000u)); "
        "Console.WriteLine(Big2<uint>(3u, 4000000000u)); "
        "Console.WriteLine(Big2<byte>(byte(1), byte(200))); "
        "Console.WriteLine(Big2<ushort>(ushort(3), ushort(60000))); "
        "Console.WriteLine(Big2<ulong>(3ul, 18000000000000000000ul)); "
        "Console.WriteLine(Big2<char>('a', 'b')); "
        "return 0; end Main; end C;"),
        std::string("4000000000\n4000000000\n200\n60000\n18000000000000000000\nb\n"));
}

BEZEL_TEST(math_minmax_generic_unsigned_6908, min_and_clamp_unsigned) {
    BEZEL_EXPECT_EQ(stdoutOf(std::string(kHelpers) +
        "Console.WriteLine(Sm<uint>(3u, 4000000000u)); "
        "Console.WriteLine(Sm<byte>(byte(200), byte(1))); "
        "Console.WriteLine(Sm<ulong>(3ul, 18000000000000000000ul)); "
        "Console.WriteLine(Cl<uint>(4000000000u, 3u, 5u)); "
        "Console.WriteLine(Cl<byte>(byte(250), byte(3), byte(200))); "
        "Console.WriteLine(Cl<ulong>(18000000000000000000ul, 3ul, 5ul)); "
        "return 0; end Main; end C;"),
        std::string("3\n1\n3\n5\n200\n5\n"));
}

// Controls: signed and floating instantiations keep their ordering.
BEZEL_TEST(math_minmax_generic_unsigned_6908, signed_and_double_controls) {
    BEZEL_EXPECT_EQ(stdoutOf(std::string(kHelpers) +
        "Console.WriteLine(Big2<int>(-3, 4)); "
        "Console.WriteLine(Sm<int>(-3, 4)); "
        "Console.WriteLine(Cl<int>(-9, -3, 4)); "
        "Console.WriteLine(Sm<sbyte>(sbyte(-5), sbyte(3))); "
        "Console.WriteLine(Big2<double>(-3.5, 4.5)); "
        "return 0; end Main; end C;"),
        std::string("4\n-3\n-3\n-5\n4.5\n"));
}
