// Codegen — #2147 (§6.6 / §6.5 / §6.2 / §5.2): a BITWISE compound assignment
// (`&=`/`|=`/`^=`) whose RHS is a WIDER integer must run the operation at the
// §6.2-promoted width and apply the §6.6 CHECKED narrow-integer write-back —
// exactly as #2140 established for the arithmetic operators — not silently
// truncate the RHS to the LHS width before the AND/OR/XOR.
//
// Per §6.6 (spec v0.220) the compound `x op= e` is `x := x op e` with the
// result typed R = type of `x op e` "under the rules of this chapter (§6.2
// promotion, §6.5 shifts and bitwise, …)"; when T is an integer and R is a
// wider integer the result "is converted back to T by the corresponding §5.2
// checked narrowing … a result outside T's range raises OverflowException
// (§6.1's checked posture; never a silent truncation)". `&`/`|`/`^` promote
// mixed operands per §6.2 (§3.5: "operand-type combinations follow the
// numeric-promotion rules in §6"), so `int & long : long` and the compound
// needs the checked write-back. Only the SHIFT forms are exempt: "a shift's
// result already has the left operand's type" (§6.5), and "a shift never
// raises OverflowException".
//
// The defect (sibling of #2140, found during it): emitCompoundOp's #2140 wide
// branch covered only `+ - * / mod`; `&`/`|`/`^` fell to the #1042 tail
// `CreateIntCast(r, l->getType())`, truncating the wide RHS BEFORE the bit
// operation. `int(-1) &= 2^32L` stored 0 (SExt(-1) & 2^32 = 2^32, out of int
// range → must raise); `int(0) ^= (2^32+1)L` stored a WRONG in-range 1;
// `byte(5) |= 300` truncated 300 → 44 and stored 45 (5|300 = 301 > 255 → must
// raise). Unlike arithmetic, `&`/`|`/`^` cannot overflow the promoted width
// itself, so the wide op needs no overflow intrinsic — only the final §5.2
// narrow-CHECK back to T.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// Body-only program returning an int exit code.
int run(const std::string& body) {
    auto r = runAot(
        std::string("module M; class C public static method Main(): int ") +
        body + " end Main; end C;");
    return r.ran ? r.exitCode : -999;
}

// §6.5/§17.14 (v0.242, #3045): is `body` rejected with the constant shift-count
// domain diagnostic (and not for some unrelated reason)?
bool rejectsShiftDomain(const std::string& body) {
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

// Returns 1 if `body` raises OverflowException, 0 if it runs to completion,
// -999 on a front-end failure.
int throwsOverflow(const std::string& body) {
    auto r = runAot(
        std::string("module M; class C public static method Main(): int "
                    "  try ") +
        body +
        " return 0; catch OverflowException => return 1; end; return 0; "
        "end Main; end C;");
    return r.ran ? r.exitCode : -999;
}


bool runs(const std::string& body) {
    auto r = runAot(
        std::string("module M; class C public static method Main(): int ") +
        body + " end Main; end C;");
    return r.ran;
}

}  // namespace

// --- The exact issue repro: `int(-1) &= 2^32L`. SExt(-1) & 2^32 = 2^32,
// outside int range → OverflowException (§6.6/§5.2). FAILS WITHOUT the fix
// (truncated 2^32 → 0, stored -1 & 0 = 0, no raise). ---
BEZEL_TEST(codegen_2147_bitwise_wide_rhs, and_neg_lhs_pow2_32_throws) {
    BEZEL_EXPECT_EQ(throwsOverflow("var i: int := -1; i &= 4294967296L;"), 1);
}

// `int(0) |= 2^32L` = 2^32, out of int range → raise. FAILS WITHOUT the fix
// (stored 0).
BEZEL_TEST(codegen_2147_bitwise_wide_rhs, or_pow2_32_throws) {
    BEZEL_EXPECT_EQ(throwsOverflow("var j: int := 0; j |= 4294967296L;"), 1);
}

// The sharpest case: `int(0) ^= (2^32+1)L` truncated to a VALID in-range 1 —
// a WRONG value silently stored. Full-width result 2^32+1 is out of int
// range → raise. FAILS WITHOUT the fix (stored 1).
BEZEL_TEST(codegen_2147_bitwise_wide_rhs, xor_truncate_to_valid_throws) {
    BEZEL_EXPECT_EQ(throwsOverflow("var k: int := 0; k ^= 4294967297L;"), 1);
}

// §5.2 narrow-check boundary, both sides: `int(-1) &= int.MaxValue-as-long`
// = 2147483647, IN range → stores (no false raise); `int(-1) &= 2^31L`
// = 2147483648, boundary+1 → raise. WITHOUT the fix the second stored
// INT_MIN (0x80000000 truncated) — garbage.
BEZEL_TEST(codegen_2147_bitwise_wide_rhs, and_boundary_intmax_stores) {
    BEZEL_EXPECT_EQ(run(
        "var i: int := -1; i &= 2147483647L; "
        "if i = 2147483647 then return 1; end; return 0;"), 1);
}
BEZEL_TEST(codegen_2147_bitwise_wide_rhs, and_boundary_plus_one_throws) {
    BEZEL_EXPECT_EQ(throwsOverflow("var i: int := -1; i &= 2147483648L;"), 1);
}

// A negative (sign-extended) wide RHS computes on the SExt'd value — `int(5)
// &= -1L` = 5, in range → stores 5, no false raise (§6.2: each operand
// extends by its own signedness).
BEZEL_TEST(codegen_2147_bitwise_wide_rhs, and_negative_long_rhs_stores) {
    BEZEL_EXPECT_EQ(run("var i: int := 5; i &= -1L; return i;"), 5);
}

// A negative in-range result narrows correctly (trunc/SExt round-trip):
// `int(-4) ^= 3L` = -1 → stores -1.
BEZEL_TEST(codegen_2147_bitwise_wide_rhs, xor_negative_result_in_range_stores) {
    BEZEL_EXPECT_EQ(run(
        "var i: int := -4; i ^= 3L; "
        "if i = -1 then return 1; end; return 0;"), 1);
}

// --- Sub-i32 LHS with a wider (int) RHS — same §6.2/§6.6 cell one width
// down. `byte(5) |= 300` = 301, out of byte range → raise. FAILS WITHOUT the
// fix (300 truncated to 44, stored 45). ---
BEZEL_TEST(codegen_2147_bitwise_wide_rhs, byte_or_wider_int_throws) {
    BEZEL_EXPECT_EQ(throwsOverflow("var b: byte := 5; b |= 300;"), 1);
}

// In-range wider-RHS result stores: `byte(5) ^= 34` = 39.
BEZEL_TEST(codegen_2147_bitwise_wide_rhs, byte_xor_wider_int_in_range_stores) {
    BEZEL_EXPECT_EQ(run("var b: byte := 5; b ^= 34; return int(b);"), 39);
}

// Signed sub-i32: `sbyte(-1) &= 200` — SExt(-1) & 200 = 200, above sbyte max
// 127 → raise. FAILS WITHOUT the fix (200 truncated to -56, stored -56).
BEZEL_TEST(codegen_2147_bitwise_wide_rhs, sbyte_and_wider_positive_throws) {
    BEZEL_EXPECT_EQ(throwsOverflow("var s: sbyte := -1; s &= 200;"), 1);
}

// --- Unsigned pair: uint &=/|= ulong. `uint(1) |= 2^32UL` is out of uint
// range → raise (was stored 1). An in-range ulong RHS above int.MaxValue
// stores under the UNSIGNED narrow-check (no spurious raise). ---
BEZEL_TEST(codegen_2147_bitwise_wide_rhs, uint_or_pow2_32_throws) {
    BEZEL_EXPECT_EQ(throwsOverflow("var u: uint := 1U; u |= 4294967296UL;"), 1);
}
BEZEL_TEST(codegen_2147_bitwise_wide_rhs, uint_or_ulong_in_range_stores) {
    BEZEL_EXPECT_EQ(run(
        "var u: uint := 0U; u |= 3000000000UL; "
        "if u = 3000000000U then return 1; end; return 0;"), 1);
}

// --- §7.11 composition: the lifted compound applies the same write-back on
// the both-present branch (`int?(-1) &= 2^32L` → raise), and a null LHS
// propagates null with NO check fired ("when the lifted result is null … no
// underlying operation runs and no check fires", §7.11). The raise case
// FAILS WITHOUT the fix (present branch stored 0). ---
BEZEL_TEST(codegen_2147_bitwise_wide_rhs, lifted_and_present_throws) {
    // v0.267 / #3298: `int? &= long` is rejected at sema (§6.6/§7.11).
    // The acceptance shape pinned the pre-fix lifted writeback raising.
    BEZEL_EXPECT_FALSE(runs("var i: int? := -1; i &= 4294967296L;"));
}
BEZEL_TEST(codegen_2147_bitwise_wide_rhs, lifted_and_null_no_check) {
    BEZEL_EXPECT_FALSE(runs(
        "var i: int? := null; i &= 4294967296L; return (i ?? 42);"));
}

// --- Controls: neighbor cells the fix must NOT perturb. ---
// Same-width bitwise takes no narrow-check (R = T, §6.6): int(-1) &= int(-1)
// stays -1, no raise possible.
BEZEL_TEST(codegen_2147_bitwise_neighbors, same_width_and_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "var i: int := -1; var j: int := -1; i &= j; "
        "if i = -1 then return 1; end; return 0;"), 1);
}

// Same-width sub-i32 pair stays exact (byte | byte : byte, §6.6's
// `checksum ^= b` cell): 200 | 100 = 236, fits byte.
BEZEL_TEST(codegen_2147_bitwise_neighbors, byte_or_byte_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "var b: byte := 200; var c: byte := 100; b |= c; return int(b);"), 236);
}

// A NARROWER RHS widens up to the LHS type (unchanged path): long(255) &=
// int(-2) — SExt(-2) → 254.
BEZEL_TEST(codegen_2147_bitwise_neighbors, long_and_narrower_int_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "var l: long := 255L; var w: int := -2; l &= w; "
        "if l = 254L then return 1; end; return 0;"), 1);
}

// In-range wider-RHS AND stores the plain value: int(12) &= 10L → 8.
BEZEL_TEST(codegen_2147_bitwise_neighbors, and_wide_in_range_stores) {
    BEZEL_EXPECT_EQ(run("var m: int := 12; m &= 10L; return m;"), 8);
}

// Shifts stay on the LHS-width path (§6.5: the result has the left operand's
// type, so §6.6's checked write-back never applies to them). ⚠ THE COUNT RULE
// CHANGED AT v0.242 (proposal 0079, #3045): the count is no longer reduced mod
// the width — a CONSTANT count outside the LHS type's `0..width−1` domain is a
// compile error (§17.14). WAS: `s <<= 33` gave 2 and `t <<= 40L` gave 256.
// The in-domain cell (`byte(5) <<= 1` → 10) is unchanged, and is what still
// proves shifts skip the wide-bitwise branch this file is about.
BEZEL_TEST(codegen_2147_bitwise_neighbors, shift_out_of_domain_count_rejected) {
    BEZEL_EXPECT_TRUE(rejectsShiftDomain("var s: int := 1; s <<= 33;"));
    BEZEL_EXPECT_TRUE(rejectsShiftDomain("var t: int := 1; t <<= 40L;"));
    BEZEL_EXPECT_EQ(run("var b: byte := 5; b <<= 1; return int(b);"), 10);
}

// The #2140 arithmetic wide-RHS cells are this fix's structural neighbors —
// one anchor to confirm the shared branch still checks arithmetic: int(0) +=
// 2^32L raises; byte(200) += 30L stores 230.
BEZEL_TEST(codegen_2147_bitwise_neighbors, arithmetic_wide_anchor_unchanged) {
    BEZEL_EXPECT_EQ(throwsOverflow("var i: int := 0; i += 4294967296L;"), 1);
    BEZEL_EXPECT_EQ(
        run("var b: byte := 200; var v: long := 30L; b += v; return int(b);"),
        230);
}
