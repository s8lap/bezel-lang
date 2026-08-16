// Codegen / sema — #3078 (spec v0.247 / §6.5 / §17.14): a constant shift's
// RESULT is truncated to the left operand's width.
//
// §6.5, verbatim: "Within the domain, bits shifted beyond the left operand's
// width are discarded silently — that is the operation, exactly as `&` discards
// bits — and the result never faults."
//
// §17.14: "A constant expression is evaluated at compile time under the *same
// semantics* the expression has at run time".
//
// The AST-level fold (`sema::foldIntConst`) computes shifts in `long long` and
// returned the un-truncated value (`1 << 31` → 2147483648), which the
// destination-type range check at the const declaration then reported as
// non-fitting — even though `0x80000000` IS representable in `int` (as
// `-2147483648`). The fix re-truncates the fold's value to the LEFT operand's
// bit width with sign extension for signed types (and recognises the `L`/`l`
// literal suffix as 64-bit, so `1L << 63` no longer trips the now-correctly-
// computed count-domain rule).
//
// Three classes of case pin the bug:
//   (A) the four signed/unsigned × 32-bit/64-bit cells the original brief
//       enumerates — 1<<31 on int, 1u<<31 on uint, 1L<<63 on long, 1uL<<63 on
//       ulong;
//   (B) the value the fold now produces (the masked value, with sign
//       extension) is the one the runtime fold produces for the same source
//       — i.e. the constant and runtime arms of §17.14 AGREE;
//   (C) the unchanged half of §6.5 — out-of-domain counts (`1 << 32`, `1L << 64`)
//       are still compile errors, with the same §6.5/§17.14 wording the
//       earlier #3045 test pins.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

// Body-only program returning an int exit code.
int run(const std::string& body) {
    auto r = runAot(
        std::string("module M; class C public static method Main(): int ") +
        body + " end Main; end C;");
    return r.ran ? r.exitCode : -999;
}

// The CONSTANT arm: rejected, and rejected by the §6.5/§17.14 domain rule
// specifically (a bare hasErrors() would pass for any unrelated reject).
bool rejectsDomainFault(const std::string& body) {
    auto r = runAot(
        std::string("module M; class C public static method Main(): int ") +
        body + " end Main; end C;");
    if (!r.diagnostics.hasErrors()) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("is outside the domain") != std::string::npos &&
            d.message.find("(\xC2\xA7""6.5/\xC2\xA7""17.14)") !=
                std::string::npos)
            return true;
    return false;
}

}  // namespace

// ===========================================================================
// A. The four cells from the original issue + brief.
// ===========================================================================

// §6.5 / §17.14 (#3078): `1 << 31` on int — the in-domain shift yields
// int.MinValue (-2147483648). Pre-fix the const declaration was rejected with
// "initializer value 2147483648 does not fit its declared type".
BEZEL_TEST(const_shift_truncation_3078, int_one_shl_thirty_one_folds) {
    // The masked-bit pattern 0x80000000 read as a signed int is -2147483648;
    // the `< 0` check keeps the value inside the int exit channel.
    BEZEL_EXPECT_EQ(run(
        "const H: int := 1 << 31; "
        "return (if H < 0 then 1 else 0);"), 1);
    // …and the value is exactly int.MinValue.
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C public static method Main(): int "
        "const H: int := 1 << 31; "
        "Console.WriteLine(H); return 0; end Main; end C;"),
        "-2147483648");
}

// `1u << 31` on uint — the same bit pattern is unsigned, so the result is
// 2147483648 (a representable uint). Pre-fix the const declaration was
// rejected; the fix masks to 32 bits WITHOUT sign extension for the unsigned
// left operand.
BEZEL_TEST(const_shift_truncation_3078, uint_one_shl_thirty_one_folds) {
    BEZEL_EXPECT_EQ(run(
        "const V: uint := 1u << 31; "
        "return (if V = 2147483648u then 1 else 0);"), 1);
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C public static method Main(): int "
        "const V: uint := 1u << 31; "
        "Console.WriteLine(V); return 0; end Main; end C;"),
        "2147483648");
}

// `1L << 63` on long — the in-domain shift (long's domain is `0..63`, the
// `L`/`l` literal suffix correctly widens the AST-only width inference to 64)
// yields long.MinValue. Pre-fix the count check fired at 32 bits, folding
// `1L << 63` to nullopt; with the fix the count-domain rule accepts 63 and
// the value is truncated to 64 bits with sign extension.
BEZEL_TEST(const_shift_truncation_3078, long_one_shl_sixty_three_folds) {
    // long.MinValue is -9223372036854775808; we cannot print a literal that
    // big in the source, so we compare against it via a runtime expression.
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C public static method Main(): int "
        "const L: long := 1L << 63; "
        "Console.WriteLine(L); return 0; end Main; end C;"),
        "-9223372036854775808");
}

// `1uL << 63` on ulong — the same bit pattern, unsigned this time, so the
// value is 9223372036854775808. The fold's 64-bit truncation gives the
// right bit pattern; `ulong`'s range check accepts it.
BEZEL_TEST(const_shift_truncation_3078, ulong_one_shl_sixty_three_folds) {
    // ulong is not an exit-channel type — observed on stdout.
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; class C public static method Main(): int "
        "const UL: ulong := 1uL << 63; "
        "Console.WriteLine(UL); return 0; end Main; end C;"),
        "9223372036854775808");
}

// ===========================================================================
// B. The constant and runtime arms of §17.14 AGREE.
// ===========================================================================

// §17.14 says the constant-time evaluation must match the runtime arm. Pre-fix
// the const arm gave an un-truncated `long long` (failing the int range
// check) while the runtime arm gave the §6.5-truncated value. The fix puts
// them back in agreement.
BEZEL_TEST(const_shift_truncation_3078, constant_matches_runtime_spec_17_14) {
    // The runtime arm: `1 << 31` on int is int.MinValue (-2147483648).
    // The constant arm (the fix) folds to the same value.
    BEZEL_EXPECT_EQ(run(
        "const H: int := 1 << 31; "
        "var v: int := 1; var k: int := 31; var r := v << k; "
        "return (if H = r then 1 else 0);"), 1);
    BEZEL_EXPECT_EQ(run(
        "const L: long := 1L << 63; "
        "var v: long := 1L; var k: int := 63; var r := v << k; "
        "return (if L = r then 1 else 0);"), 1);
}

// Right shift: `int.MinValue >> 31` fills with the sign bit, yielding -1. The
// fold's new truncation handles the signed case correctly: the 32-bit result
// of shifting `-2147483648` right by 31 is `0xFFFFFFFF`, which sign-extends
// back to `-1` as `long long`.
BEZEL_TEST(const_shift_truncation_3078, signed_right_shift_fills_sign_bit) {
    // We need a way to express int.MinValue as a constant expression; the
    // obvious one is `1 << 31` — the very fold under test — so chain the two.
    BEZEL_EXPECT_EQ(run(
        "const MIN: int := 1 << 31; "
        "const R: int := MIN >> 31; "
        "return (if R = -1 then 1 else 0);"), 1);
}

// ===========================================================================
// C. The unchanged half of §6.5 — out-of-domain counts are still rejected.
// ===========================================================================

// `1 << 32` — count is outside int's `0..31` domain; this is the SAME rule
// the earlier #3045 fix pins. The #3078 fix must not regress it.
BEZEL_TEST(const_shift_truncation_3078, out_of_domain_int_count_still_rejected) {
    BEZEL_EXPECT_TRUE(rejectsDomainFault("const F: int := 1 << 32;"));
}

// `1L << 64` — count is outside long's `0..63` domain. With the fix's widened
// width inference for `L` literals, 64 is now correctly seen as out-of-domain
// (where pre-fix, the 32-bit assumption made `1L << 63` itself look out of
// domain — a different bug from this one).
BEZEL_TEST(const_shift_truncation_3078, out_of_domain_long_count_still_rejected) {
    BEZEL_EXPECT_TRUE(rejectsDomainFault("const F: long := 1L << 64;"));
}