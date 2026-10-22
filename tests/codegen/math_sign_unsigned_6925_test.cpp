// Regression tests for #6925 -- `Math.Sign` of an unsigned operand returned -1
// for every value above the signed maximum (uint, ulong, byte, ushort).
//
// stdlib §2.2 (v0.340): `Sign<T>(x: T): int where T: INumber<T>` returns
// "-1, 0, or 1: two `<` tests against T.Zero() (§2.19), so a NaN yields 0."
// No unsigned value is less than zero, and every non-zero one is greater than
// zero, so an unsigned non-zero x yields 1. The integer arm of `Sign` in
// tryLowerBuiltinClassCall compared with the signed predicate unconditionally,
// which reads the sign bit of 4000000000u, 18000000000000000000ul, byte 200 and
// ushort 60000 as negative and returned -1.

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

// `S<T>` is the generic form: its body is `Math.Sign(a)`, so the operand's
// static type is the type parameter T and is resolved per instantiation.
const char* kHelpers =
    "module M; import System; class C "
    "public static method S<T>(a: T): int where T: INumber<T> "
    "return Math.Sign(a); end S; "
    "public static method Main(): int ";
}  // namespace

// ── direct form: every unsigned width at 0, at 1 and above the signed max ──

BEZEL_TEST(math_sign_unsigned_6925, direct_uint_at_zero_one_and_above_signed_max) {
    // stdlib §2.2: Sign<T>(x: T): int with T = uint; 4000000000u is 1, not -1.
    BEZEL_EXPECT_EQ(stdoutOf(std::string(kHelpers) +
        "Console.WriteLine(Math.Sign(0u)); "
        "Console.WriteLine(Math.Sign(1u)); "
        "Console.WriteLine(Math.Sign(4000000000u)); "
        "return 0; end Main; end C;"),
        std::string("0\n1\n1\n"));
}

BEZEL_TEST(math_sign_unsigned_6925, direct_ulong_at_zero_one_and_above_signed_max) {
    // stdlib §2.2: Sign<T>(x: T): int with T = ulong; 18000000000000000000ul is 1.
    BEZEL_EXPECT_EQ(stdoutOf(std::string(kHelpers) +
        "Console.WriteLine(Math.Sign(0ul)); "
        "Console.WriteLine(Math.Sign(1ul)); "
        "Console.WriteLine(Math.Sign(18000000000000000000ul)); "
        "return 0; end Main; end C;"),
        std::string("0\n1\n1\n"));
}

BEZEL_TEST(math_sign_unsigned_6925, direct_byte_at_zero_one_and_above_signed_max) {
    // stdlib §2.2: Sign<T>(x: T): int with T = byte; byte 200 is 1, not -1.
    BEZEL_EXPECT_EQ(stdoutOf(std::string(kHelpers) +
        "Console.WriteLine(Math.Sign(byte(0))); "
        "Console.WriteLine(Math.Sign(byte(1))); "
        "Console.WriteLine(Math.Sign(byte(200))); "
        "return 0; end Main; end C;"),
        std::string("0\n1\n1\n"));
}

BEZEL_TEST(math_sign_unsigned_6925, direct_ushort_at_zero_one_and_above_signed_max) {
    // stdlib §2.2: Sign<T>(x: T): int with T = ushort; ushort 60000 is 1, not -1.
    BEZEL_EXPECT_EQ(stdoutOf(std::string(kHelpers) +
        "Console.WriteLine(Math.Sign(ushort(0))); "
        "Console.WriteLine(Math.Sign(ushort(1))); "
        "Console.WriteLine(Math.Sign(ushort(60000))); "
        "return 0; end Main; end C;"),
        std::string("0\n1\n1\n"));
}

// The smallest values above each signed maximum, and each unsigned maximum.
BEZEL_TEST(math_sign_unsigned_6925, direct_unsigned_signed_max_boundaries) {
    // stdlib §2.2: every non-zero unsigned operand yields 1.
    BEZEL_EXPECT_EQ(stdoutOf(std::string(kHelpers) +
        "Console.WriteLine(Math.Sign(2147483648u)); "
        "Console.WriteLine(Math.Sign(4294967295u)); "
        "Console.WriteLine(Math.Sign(9223372036854775808ul)); "
        "Console.WriteLine(Math.Sign(18446744073709551615ul)); "
        "Console.WriteLine(Math.Sign(byte(128))); "
        "Console.WriteLine(Math.Sign(byte(255))); "
        "Console.WriteLine(Math.Sign(ushort(32768))); "
        "Console.WriteLine(Math.Sign(ushort(65535))); "
        "return 0; end Main; end C;"),
        std::string("1\n1\n1\n1\n1\n1\n1\n1\n"));
}

// ── the explicit type-argument spelling: Math.Sign<T>(x) ──

BEZEL_TEST(math_sign_unsigned_6925, explicit_type_argument_form_unsigned) {
    // stdlib §2.2: Sign<T>(x: T): int, spelled with an explicit T (§14.5).
    BEZEL_EXPECT_EQ(stdoutOf(std::string(kHelpers) +
        "Console.WriteLine(Math.Sign<uint>(4000000000u)); "
        "Console.WriteLine(Math.Sign<ulong>(18000000000000000000ul)); "
        "Console.WriteLine(Math.Sign<byte>(byte(200))); "
        "Console.WriteLine(Math.Sign<ushort>(ushort(60000))); "
        "Console.WriteLine(Math.Sign<uint>(0u)); "
        "return 0; end Main; end C;"),
        std::string("1\n1\n1\n1\n0\n"));
}

// ── the generic form: a user generic `S<T>` whose body is Math.Sign(a) ──

BEZEL_TEST(math_sign_unsigned_6925, generic_form_every_unsigned_width) {
    // stdlib §2.2: Sign<T>(x: T): int, instantiated at each unsigned T.
    BEZEL_EXPECT_EQ(stdoutOf(std::string(kHelpers) +
        "Console.WriteLine(S<uint>(0u)); "
        "Console.WriteLine(S<uint>(4000000000u)); "
        "Console.WriteLine(S<ulong>(0ul)); "
        "Console.WriteLine(S<ulong>(18000000000000000000ul)); "
        "Console.WriteLine(S<byte>(byte(0))); "
        "Console.WriteLine(S<byte>(byte(200))); "
        "Console.WriteLine(S<ushort>(ushort(0))); "
        "Console.WriteLine(S<ushort>(ushort(60000))); "
        "return 0; end Main; end C;"),
        std::string("0\n1\n0\n1\n0\n1\n0\n1\n"));
}

// ── signed controls: the signed compare is unchanged ──

BEZEL_TEST(math_sign_unsigned_6925, signed_direct_controls) {
    // stdlib §2.2: Sign<T>(x: T): int with T = int: -5 is -1, 0 is 0, 5 is 1.
    BEZEL_EXPECT_EQ(stdoutOf(std::string(kHelpers) +
        "Console.WriteLine(Math.Sign(0 - 5)); "
        "Console.WriteLine(Math.Sign(0)); "
        "Console.WriteLine(Math.Sign(5)); "
        "return 0; end Main; end C;"),
        std::string("-1\n0\n1\n"));
}

BEZEL_TEST(math_sign_unsigned_6925, signed_generic_controls) {
    // stdlib §2.2: Sign<T>(x: T): int with T = int, sbyte, long: the signed
    // operands keep the signed ordering.
    BEZEL_EXPECT_EQ(stdoutOf(std::string(kHelpers) +
        "Console.WriteLine(S<int>(0 - 5)); "
        "Console.WriteLine(S<int>(0)); "
        "Console.WriteLine(S<int>(5)); "
        "Console.WriteLine(S<sbyte>(sbyte(-5))); "
        "Console.WriteLine(S<long>(0L - 5L)); "
        "Console.WriteLine(S<long>(5L)); "
        "return 0; end Main; end C;"),
        std::string("-1\n0\n1\n-1\n-1\n1\n"));
}
