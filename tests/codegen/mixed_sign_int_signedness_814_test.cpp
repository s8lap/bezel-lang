// fix #814 / §6.2 / §5.1: a binary integer OPERATION runs at the COMMON
// (promoted) type, whose signedness is unsigned ONLY when BOTH operands are
// unsigned. Before the fix the operation signedness OR'd the operands ("either
// unsigned"), so a narrower-unsigned + wider-signed pair (byte+int, uint+long)
// used an UNSIGNED comparison / overflow intrinsic — treating the negative wider
// value as a huge unsigned number: wrong booleans and spurious OverflowException.
// Per-operand WIDENING (zext/sext) and right-SHIFT fill use each operand's own
// signedness and are verified to stay correct.
//
// Value tests — the program return value is observed as the process exit code.
// All expected values here are in 0..255, so they round-trip the 8-bit exit
// status (WEXITSTATUS); runAot compiles+runs the program natively (JIT->AOT
// migration, P1).

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
std::string mainOf(const std::string& body) {
    return "module M; class C public static method Main(): int " + body +
           " end Main; end C;";
}

bool runs(const std::string& body) {
    auto r = runAot(
        std::string("module M; class C public static method Main(): int ") +
        body + " end Main; end C;");
    return r.ran;
}

}  // namespace

// §6.2: byte(5) < int(-3) is false — the common type int is signed. 0 (was 1).
BEZEL_TEST(mixed_sign_int_814, byte_lt_negative_int) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var b: byte := 5; var i: int := -3; "
        "if b < i then return 1; end; return 0;")), 0);
}

// §6.2: uint(5) < long(-3) is false — the common type long is signed. 0 (was 1).
BEZEL_TEST(mixed_sign_int_814, uint_lt_negative_long) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var u: uint := 5u; var l: long := -3L; "
        "if u < l then return 1; end; return 0;")), 0);
}

// §6.1/§6.2: byte(5) + int(-3) = 2 — signed overflow check, no spurious throw.
BEZEL_TEST(mixed_sign_int_814, byte_plus_negative_int_no_overflow) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var b: byte := 5; var i: int := -3; var r: int := b + i; return r;")), 2);
}

// §6.1/§6.2: uint(5) + long(-3) = 2 — signed overflow check at long width.
BEZEL_TEST(mixed_sign_int_814, uint_plus_negative_long_no_overflow) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var u: uint := 5u; var l: long := -3L; var r: long := u + l; "
        "return int(r);")), 2);
}

// Regression: pure-unsigned comparison stays unsigned. 5u < 4_000_000_000u is
// true (a signed i32 read of 4e9 would be negative). 1.
BEZEL_TEST(mixed_sign_int_814, pure_unsigned_large_compare) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var a: uint := 5u; var b: uint := 4000000000u; "
        "if a < b then return 1; end; return 0;")), 1);
}

// Regression: byte < byte (both unsigned) stays unsigned. 3 < 4 → 1.
BEZEL_TEST(mixed_sign_int_814, pure_unsigned_byte_compare) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var a: byte := 3; var b: byte := 4; "
        "if a < b then return 1; end; return 0;")), 1);
}

// Regression: pure-signed comparison stays signed. -3 < 5 → 1.
BEZEL_TEST(mixed_sign_int_814, pure_signed_compare) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var a: int := -3; var b: int := 5; "
        "if a < b then return 1; end; return 0;")), 1);
}

// §6.5: `>>` on a SIGNED value is arithmetic. -4 >> 1 = -2; +100 = 98 (a logical
// shift would yield ~2.1e9, not 98). 98.
BEZEL_TEST(mixed_sign_int_814, signed_right_shift_arithmetic) {
    BEZEL_EXPECT_EQ(run(mainOf(
        "var x: int := -4; var s: int := x >> 1; return s + 100;")), 98);
}

// §6.5: `>>` on an UNSIGNED value is logical. 100u >> 1 = 50.
BEZEL_TEST(mixed_sign_int_814, unsigned_right_shift_logical) {
    BEZEL_EXPECT_EQ(run(mainOf("var x: uint := 100u; return int(x >> 1);")), 50);
}

// §7.11: the nullable-lifted mixed comparison is signed too. byte?(5) < int?(-3)
// is false → `?? false` yields false → 0.
BEZEL_TEST(mixed_sign_int_814, nullable_lifted_mixed_compare) {
    BEZEL_EXPECT_FALSE(runs(mainOf(
        "var b: byte? := 5; var i: int? := -3; "
        "if (b < i) ?? false then return 1; end; return 0;")));
}
