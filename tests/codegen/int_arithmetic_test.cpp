// Codegen slice CG2 — integer arithmetic, bitwise, and shift operators,
// plus unary -/~/+ (§6.1, §17.1). Precedence/associativity come from the
// parser; codegen just lowers the resulting tree. NOTE: overflow and
// divide-by-zero checking (§6.1 OverflowException/DivideByZeroException)
// is deferred to the exception-runtime slice; these tests stay within the
// non-overflowing, non-zero-divisor range.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& expr) {
    auto r = runAot(std::string("module M; class C "
        "public static method Main(): int => ") + expr + "; end C;");
    return r.ran ? r.exitCode : -999;
}

// 8-bit exit channel (P0-E / migration plan §3): a process exit status is 8-bit
// (WEXITSTATUS masks to 0..255), so a value outside 0..255 — here the NEGATIVE
// results — cannot round-trip the exit code the way it did under the in-process
// JIT (which returned the raw 32-bit int). The program instead PRINTS the value
// (Console.WriteLine, stdlib §2.1) and the test checks the captured stdout,
// which runAot exposes as JitResult::stdoutText. WriteLine appends '\n'.
std::string runOut(const std::string& expr) {
    auto r = runAot(std::string(
        "module M; class C public static method Main(): int "
        "  Console.WriteLine(") + expr + "); return 0; end Main; end C;");
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}

} // namespace

BEZEL_TEST(codegen_cg2, addition) {
    BEZEL_EXPECT_EQ(run("2 + 3"), 5);
}

BEZEL_TEST(codegen_cg2, precedence_mul_over_add) {
    BEZEL_EXPECT_EQ(run("1 + 2 * 3"), 7);
}

BEZEL_TEST(codegen_cg2, subtraction_and_grouping) {
    BEZEL_EXPECT_EQ(run("(10 - 2) * 2"), 16);
}

BEZEL_TEST(codegen_cg2, division_truncates_toward_zero) {
    BEZEL_EXPECT_EQ(run("7 / 2"), 3);
}

BEZEL_TEST(codegen_cg2, mod_follows_dividend_sign) {
    // §6.1: -7 mod 3 == -1. Negative → observed on stdout (8-bit exit channel).
    BEZEL_EXPECT_EQ(runOut("-7 mod 3"), "-1\n");
}

BEZEL_TEST(codegen_cg2, unary_negation) {
    // Negative result → observed on stdout (8-bit exit channel).
    BEZEL_EXPECT_EQ(runOut("-(5 + 1)"), "-6\n");
}

BEZEL_TEST(codegen_cg2, bitwise_and_or_xor) {
    BEZEL_EXPECT_EQ(run("(12 & 10) | 1"), 9);   // (1100 & 1010)=1000 | 1 = 1001
}

BEZEL_TEST(codegen_cg2, left_shift) {
    BEZEL_EXPECT_EQ(run("1 << 4"), 16);
}

BEZEL_TEST(codegen_cg2, right_shift_arithmetic) {
    // Negative result → observed on stdout (8-bit exit channel).
    BEZEL_EXPECT_EQ(runOut("-16 >> 2"), "-4\n");
}

BEZEL_TEST(codegen_cg2, bitwise_not) {
    // ~0 == -1. Negative → observed on stdout (8-bit exit channel).
    BEZEL_EXPECT_EQ(runOut("~0"), "-1\n");
}

// #1049 / §5.1 / §6.2: a relational comparison between an unsigned value and a
// non-negative constant must be UNSIGNED. The signedness used to require BOTH
// operands unsigned, so `ulongVar > 1000` (unsigned vs a signed int literal)
// used a SIGNED compare and gave the wrong result once the unsigned value's
// high bit was set: `ulong 18e18 > 1000` returned false.
namespace {
int runBody(const std::string& body) {
    auto r = bezel::test::runAot(
        std::string("module M; class C public static method Main(): int ") +
        body + " end Main; end C;");
    return r.ran ? r.exitCode : -999;
}

bool runsBody(const std::string& body) {
    auto r = runAot(
        std::string("module M; class C public static method Main(): int ") +
        body + " end Main; end C;");
    return r.ran;
}

}  // namespace
BEZEL_TEST(codegen_1049_unsigned_compare, ulong_high_bit_gt_small_literal) {
    BEZEL_EXPECT_EQ(runBody(
        "var u: ulong := 18000000000000000000; return (if (u > 1000) then 1 else 0);"), 1);
}
BEZEL_TEST(codegen_1049_unsigned_compare, ulong_maxvalue_gt_small_literal) {
    BEZEL_EXPECT_EQ(runBody("return (if (ulong.MaxValue > 1000) then 1 else 0);"), 1);
}
BEZEL_TEST(codegen_1049_unsigned_compare, uint_high_bit_gt_small_literal) {
    BEZEL_EXPECT_EQ(runBody(
        "var u: uint := 3000000000; return (if (u > 1000) then 1 else 0);"), 1);
}
BEZEL_TEST(codegen_1049_unsigned_compare, ulong_high_bit_lt_small_literal_false) {
    BEZEL_EXPECT_EQ(runBody(
        "var u: ulong := 18000000000000000000; return (if (u < 1000) then 1 else 0);"), 0);
}
BEZEL_TEST(codegen_1049_unsigned_compare, ulong_ge_small_literal) {
    BEZEL_EXPECT_EQ(runBody(
        "var u: ulong := 18000000000000000000; return (if (u >= 5) then 1 else 0);"), 1);
}
// No-regression: a small unsigned vs a NEGATIVE constant keeps the signed
// compare (`u > -5` is true), and small unsigned vs a larger positive literal
// is still correctly false.
BEZEL_TEST(codegen_1049_unsigned_compare, ulong_small_gt_negative_literal) {
    BEZEL_EXPECT_EQ(runBody(
        "var u: ulong := 10; return (if (u > 0 - 5) then 1 else 0);"), 1);
}
BEZEL_TEST(codegen_1049_unsigned_compare, uint_small_gt_large_literal_false) {
    BEZEL_EXPECT_EQ(runBody(
        "var u: uint := 5; return (if (u > 1000) then 1 else 0);"), 0);
}

// #1050 / §5.1: the (nullable) relational comparison had the same
// unsigned-vs-non-negative-constant signedness bug as #1049 (the non-nullable
// site) — `uint 3e9 > 1000` was signed-compared and returned false.
//
// BEHAVIOR FLIP (§7.4, spec:1061, read through §7.11, spec:1177; #4718 ruled
// YES, v0.309): these rows were spelled against a LIFTED relational
// (`var u: uint? := 3000000000; u > 1000` through a `bool?` local). The LIFT
// is genuinely retired — §7.11 (v0.267) opens "No operator **lifts** to a
// nullable type", so a relational over a nullable operand is not a `bool?`
// value at all — but NOT for the reason this comment previously gave. That
// text claimed init-narrowing does not admit the operator ("a local
// initialised with a non-null literal is not narrowed for this gate") and
// called the block comment's "§7.4 narrows at the initialiser … the ordinary
// operator applies to it unchanged" FALSE. That claim is not FALSE: it is the
// current rule. §7.4's establishment list names the declaration form —
// "`var x: T? := expr;` … which is the declaration's assignment" — so
// `var u: uint? := 3000000000; u > 1000` reads `u` at its non-nullable type
// and yields an ordinary `bool`, never a `bool?`. The `bool?` typing the old
// rows asserted is what the retired lift supplied, and the lift is gone.
//
// The four value rows are kept POSITIVE (conversion (a)) with the §7.4
// NULL-TEST narrowing applied explicitly, because the UNSIGNED comparison is
// what #1050 is about and these rows are the only oracle for it. The
// null-test spelling is ADMISSIBLE, not REQUIRED — an initializer with a
// non-null value reaches this gate too.
BEZEL_TEST(codegen_1050_lifted_unsigned_compare, uint_nullable_high_bit_gt_literal) {
    BEZEL_EXPECT_EQ(runBody(
        "var u: uint? := 3000000000; var r: bool := false; "
        "if u <> null then r := u > 1000; end if; "
        "return (if r then 1 else 0);"), 1);
}
BEZEL_TEST(codegen_1050_lifted_unsigned_compare, ulong_nullable_high_bit_gt_literal) {
    BEZEL_EXPECT_EQ(runBody(
        "var u: ulong? := 18000000000000000000; var r: bool := false; "
        "if u <> null then r := u > 1000; end if; "
        "return (if r then 1 else 0);"), 1);
}
BEZEL_TEST(codegen_1050_lifted_unsigned_compare, uint_nullable_lt_literal_false) {
    BEZEL_EXPECT_EQ(runBody(
        "var u: uint? := 3000000000; var r: bool := false; "
        "if u <> null then r := u < 1000; end if; "
        "return (if r then 1 else 0);"), 0);
}
// No-regression: signed nullable unchanged.
BEZEL_TEST(codegen_1050_lifted_unsigned_compare, int_nullable_signed_compare_unchanged) {
    BEZEL_EXPECT_EQ(runBody(
        "var i: int? := 0 - 5; var r: bool := false; "
        "if i <> null then r := i < 1000; end if; "
        "return (if r then 1 else 0);"), 1);
}
// …and the operand NO narrowing reaches — initialised with `null` — is the
// §7.11 rejection. This row is the control that keeps the four above honest:
// they pass because §7.4's null-TEST narrowed, not because the gate is absent.
BEZEL_TEST(codegen_1050_lifted_unsigned_compare, uint_nullable_null_rejected_3298) {
    BEZEL_EXPECT_FALSE(runsBody(
        "var u: uint? := null; var r: bool? := u > 1000; "
        "return (if (r ?? false) then 1 else 0);"));
}

// #1210 / §6.5 — the const-fold of a shift. ⚠ THE RULE CHANGED AT v0.242
// (proposal 0079, #3045). #1210 made the fold MASK the count by the LEFT
// operand's bit width (int → & 31), because §6.5 then reduced the count modulo
// the width; that stopped `[1 << 40]int` from folding to 1099511627776 and
// allocating a ~16 GB stack frame (SIGSEGV). §6.5 now gives the count a DOMAIN
// (`0 .. width−1`) instead, and §17.14 makes an out-of-domain CONSTANT count an
// evaluation fault — "a compile error at the constant expression, never a
// deferred runtime fault: … `const F := 1 << 64;` [is] rejected where [it is]
// written". So the fold no longer masks: it declines, and the site reports.
// The crash #1210 fixed stays fixed — a rejected program allocates nothing.

namespace {
// Does the program get rejected with the §6.5 constant-count domain diagnostic?
bool rejectsShiftDomain(const std::string& body) {
    auto r = bezel::test::runAot(
        std::string("module M; class C public static method Main(): int ") +
        body + " end Main; end C;");
    if (!r.diagnostics.hasErrors()) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("outside the") != std::string::npos &&
            d.message.find("domain") != std::string::npos &&
            d.message.find("(\xC2\xA7""6.5/\xC2\xA7""17.14)") !=
                std::string::npos)
            return true;
    return false;
}
}  // namespace

// §6.5/§17.14 (v0.242): the count 40 is outside `int`'s `0..31` domain, so the
// inline-array size is a faulting constant expression → compile error.
// WAS: `[1 << 40]int` had Length 256 (40 & 31 == 8).
BEZEL_TEST(shift_const_fold_masks_count_1210,
           inline_array_shift_size_rejected_3045) {
    BEZEL_EXPECT_TRUE(rejectsShiftDomain(
        "var a: [1 << 40]int; return a.Length - 255;"));
}

// §6.5/§17.14 (v0.242): same for `>>`. WAS: `[1024 >> 33]int` had Length 512
// (33 & 31 == 1).
BEZEL_TEST(shift_const_fold_masks_count_1210,
           inline_array_right_shift_size_rejected_3045) {
    BEZEL_EXPECT_TRUE(rejectsShiftDomain(
        "var a: [1024 >> 33]int; return a.Length - 511;"));
}

// NO-REGRESSION: an IN-DOMAIN shift const still folds exactly as before.
// 1 << 4 = 16; array length must be 16, not 0 or some other value.
BEZEL_TEST(shift_const_fold_masks_count_1210,
           inline_array_normal_shift_size_unchanged_1210) {
    BEZEL_EXPECT_EQ(runBody(
        "var a: [1 << 4]int; return a.Length;"), 16);
}

// CONST-THROUGH-IDENTIFIER: `const SZ: int := 1 << 40;` used as an array size.
// The declaration is itself the faulting constant expression (§17.14), so the
// program is rejected there. WAS: the fold recursed into the initialiser and
// masked, giving Length 256.
BEZEL_TEST(shift_const_fold_masks_count_1210,
           inline_array_const_shift_size_rejected_3045) {
    BEZEL_EXPECT_TRUE(rejectsShiftDomain(
        "const SZ: int := 1 << 40; "
        "var a: [SZ]int; return a.Length - 255;"));
}

// NO-REGRESSION: the same const-through-identifier path with an IN-DOMAIN
// count still folds — `const SZ: int := 1 << 5;` gives `[32]int`.
BEZEL_TEST(shift_const_fold_masks_count_1210,
           inline_array_const_shift_size_in_domain_unchanged) {
    BEZEL_EXPECT_EQ(runBody(
        "const SZ: int := 1 << 5; var a: [SZ]int; return a.Length;"), 32);
}
