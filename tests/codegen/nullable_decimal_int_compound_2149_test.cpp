// Codegen — #2149 (§7.11 / §6.2 / §5.1): a compound assignment whose LHS is
// `decimal?` and whose RHS is a nullable-INTEGER (`int?`, `long?`, …) must LIFT
// per §7.11 with the RHS payload §5.1-coerced int→decimal — not silently
// miscompile.
//
// #2141 taught the `decimal?` compound-assignment lift to route the unwrapped
// `%bz.decimal` payloads through the §6.4 decimal intrinsic, but detected a
// NULLABLE RHS only as `rhs->getType() == elemTy` — i.e. only a `decimal?` RHS
// (`{i8; %bz.decimal}`). A nullable-int RHS (`decimal? += int?`, `{i8; i32}`)
// != elemTy, so it was treated ALWAYS-PRESENT: the whole `{i8; i32}` struct
// flowed as the "payload" to coerceNumericToType, whose int→decimal branch
// requires an INTEGER source and so fell through unchanged, and then to
// emitDecimalArith — a silent miscompile (the struct's bits reinterpreted as a
// decimal; the sum was wrong and RHS-null was never observed). Sema ADMITS the
// mixed shape: §6.2 promotes the smaller-range int to `decimal` (§5.1 int→decimal
// implicit conversion), and §5.1's nullable-lifted conversions carry `int?` to
// `decimal?`, so `d := d + n` typechecks and assigns back to `decimal?`.
//
// The fix widens the nullable-RHS test to "is a struct that is NOT the bare
// %bz.decimal" (== decimalStructType()), covering both a `decimal?` RHS and a
// nullable-int RHS; the unwrapped int payload then §5.1-coerces to decimal via
// the existing coerceNumericToType int→decimal branch before emitDecimalArith.
// §7.11 lift is unchanged: `null` if EITHER operand is null, the §6.4 op (and its
// DivideByZeroException guard) runs on the both-present branch only.
//
// This suite pins the mixed nullable-int family per the #2141 pattern:
// present / LHS-null / RHS-null / both-null, over an `int?` and a `long?` RHS,
// plus a `uint?` RHS (unsigned int→decimal), and re-asserts the neighbor cells
// #2141 already covered — `decimal? += decimal` (bare) and `decimal? += int`
// (plain) — stay green. Spec: §7.11 (v0.220, lifted arithmetic), §6.2
// (mixed-type promotion), §5.1 (int→decimal + nullable-lifted conversions),
// §6.4 (decimal arithmetic).

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

}  // namespace

// --- The exact issue repro: `decimal? += int?` (both present) no longer
// miscompiles; 1.0m + 2 = 3.0m. FAILS WITHOUT the fix (the whole `{i8; i32}`
// struct is fed to emitDecimalArith → wrong sum → returns 0). ---
BEZEL_TEST(codegen_2149_nullable_decimal_int, int_rhs_present) {
    BEZEL_EXPECT_EQ(run(
        "var d: decimal? := 1.0m; var n: int? := 2; d += n; "
        "if (d ?? -1.0m).CompareTo(3.0m) = 0 then return 1; end; return 0;"), 1);
}

// §7.11: a `null` LHS propagates null even with a present int? RHS — no op runs.
BEZEL_TEST(codegen_2149_nullable_decimal_int, int_rhs_lhs_null) {
    BEZEL_EXPECT_EQ(run(
        "var d: decimal? := null; var n: int? := 2; d += n; "
        "if d = null then return 1; end; return 0;"), 1);
}

// §7.11: a `null` int? RHS propagates null — the present decimal? LHS is
// discarded. Pre-fix the RHS tag was never read (always-present), so this
// silently kept the LHS instead of yielding null.
BEZEL_TEST(codegen_2149_nullable_decimal_int, int_rhs_rhs_null) {
    BEZEL_EXPECT_EQ(run(
        "var d: decimal? := 1.0m; var n: int? := null; d += n; "
        "if d = null then return 1; end; return 0;"), 1);
}

// §7.11: both null → null.
BEZEL_TEST(codegen_2149_nullable_decimal_int, int_rhs_both_null) {
    BEZEL_EXPECT_EQ(run(
        "var d: decimal? := null; var n: int? := null; d += n; "
        "if d = null then return 1; end; return 0;"), 1);
}

// §6.2 / §5.1: a `long?` RHS §5.1-widens (long→decimal) and lifts — 1.0m + 2 =
// 3.0m.
BEZEL_TEST(codegen_2149_nullable_decimal_int, long_rhs_present) {
    BEZEL_EXPECT_EQ(run(
        "var d: decimal? := 1.0m; var n: long? := 2; d += n; "
        "if (d ?? -1.0m).CompareTo(3.0m) = 0 then return 1; end; return 0;"), 1);
}

// §7.11: `long?` RHS null propagation.
BEZEL_TEST(codegen_2149_nullable_decimal_int, long_rhs_rhs_null) {
    BEZEL_EXPECT_EQ(run(
        "var d: decimal? := 1.0m; var n: long? := null; d += n; "
        "if d = null then return 1; end; return 0;"), 1);
}

// §7.11: `long?` RHS with null LHS stays null.
BEZEL_TEST(codegen_2149_nullable_decimal_int, long_rhs_lhs_null) {
    BEZEL_EXPECT_EQ(run(
        "var d: decimal? := null; var n: long? := 5; d += n; "
        "if d = null then return 1; end; return 0;"), 1);
}

// §5.1: an UNSIGNED nullable-int RHS zero-interprets int→decimal — 1.0m + 7 =
// 8.0m (exercises coerceNumericToType's srcUnsigned int→decimal path).
BEZEL_TEST(codegen_2149_nullable_decimal_int, uint_rhs_present) {
    BEZEL_EXPECT_EQ(run(
        "var d: decimal? := 1.0m; var n: uint? := 7; d += n; "
        "if (d ?? -1.0m).CompareTo(8.0m) = 0 then return 1; end; return 0;"), 1);
}

// §6.4: the other operators lift over the mixed nullable-int RHS too —
// 12/4 = 3, *5 = 15, -1 = 14, all with `int?` operands.
BEZEL_TEST(codegen_2149_nullable_decimal_int, int_rhs_sub_mul_div) {
    BEZEL_EXPECT_EQ(run(
        "var d: decimal? := 12.0m; var a: int? := 4; var b: int? := 5; "
        "var c: int? := 1; d /= a; d *= b; d -= c; "
        "if (d ?? -1.0m).CompareTo(14.0m) = 0 then return 1; end; return 0;"), 1);
}

// Neighbor control (#2141): `decimal? += decimal` (bare non-nullable) stays the
// non-nullable-RHS lift — the widened rNullable test must NOT reclassify the bare
// %bz.decimal RHS (== decimalStructType()) as nullable. 2.0m + 3.0m = 5.0m.
BEZEL_TEST(codegen_2149_nullable_decimal_int, neighbor_bare_decimal_rhs) {
    BEZEL_EXPECT_EQ(run(
        "var d: decimal? := 2.0m; d += 3.0m; "
        "if (d ?? -1.0m).CompareTo(5.0m) = 0 then return 1; end; return 0;"), 1);
}

// Neighbor control (#2141): `decimal? += int` (plain non-nullable int) stays
// green — 4.0m + 6 = 10.0m.
BEZEL_TEST(codegen_2149_nullable_decimal_int, neighbor_plain_int_rhs) {
    BEZEL_EXPECT_EQ(run(
        "var d: decimal? := 4.0m; d += 6; "
        "if (d ?? -1.0m).CompareTo(10.0m) = 0 then return 1; end; return 0;"), 1);
}
