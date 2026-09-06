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

// ⚠ THE RULE CHANGED AT v0.267 (proposal 0114, #3298). §6.6 (v0.286) makes a
// read-modify-write compound "a **compile error** when the static type of its
// left-hand side, OR of its right operand, is a **nullable** type — either
// position, or both". This file's whole subject is a nullable decimal target
// with a nullable INTEGER right operand, i.e. both positions at once, so every
// row is now a rejection. The §5.1 integer→decimal promotion the file was
// really pinning is kept on the surviving non-nullable path at the bottom, and
// the decimal RUNTIME route for a §7.4-narrowed `decimal?` operand (the
// non-compound form, which #3298 does admit) is covered by
// `codegen_lifted_ops_486_narrowed_3298`.

bool rejectedCompound(const std::string& body) {
    auto r = runAot(
        std::string("module M; class C public static method Main(): int ") +
        body + " end Main; end C;");
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        const auto& m = d.message;
        if (m.find("compound assignment") == std::string::npos) continue;
        if (m.find("nullable") == std::string::npos) continue;
        if (m.find("7.11") == std::string::npos) continue;
        return true;
    }
    return false;
}

// fix #3830 (v0.288): the PARAMETER twin of `rejectedCompound`, for a row
// whose "still rejected" operand(s) must NOT be §7.4-narrowed. `decimal`
// (and every integer width this file uses) is `TypeRef::Kind::Primitive`,
// so a local declared `var x: T? := <literal>;` narrows at its initializer,
// and #3830 makes such a narrowed operand admitted at compound assignment —
// so a row meant to test the genuinely-nullable, UNGUARDED gate must route
// its value(s) through PARAMETER(s) instead: a parameter is never narrowed
// by what the caller passes (narrowing needs an in-body null test, §7.4).
// `paramDecl` is the `Chk` method's parameter list (e.g. "d: decimal?, n:
// int?"), `argExprs` the values passed at the call site, and `body` the
// helper's statements. Same diagnostic gate as `rejectedCompound`. Modeled
// on nullable_value_compound_1964_test.cpp's `rejectedCompoundP`.
bool rejectedCompoundP(const std::string& paramDecl,
                        const std::string& argExprs,
                        const std::string& body) {
    auto r = runAot(
        std::string("module M; class C "
                    "  public static method Chk(") + paramDecl + "): int " +
        body +
        "  end Chk; "
        "  public static method Main(): int return Chk(" + argExprs + "); "
        "end Main; "
        "end C;");
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        const auto& m = d.message;
        if (m.find("compound assignment") == std::string::npos) continue;
        if (m.find("nullable") == std::string::npos) continue;
        if (m.find("7.11") == std::string::npos) continue;
        return true;
    }
    return false;
}

}  // namespace

// Nullable decimal target + nullable integer RHS, across the integer widths
// this file enumerates and across the present/absent combinations. §6.6
// rejects on either position, so all sixteen shapes take one verdict.
BEZEL_TEST(codegen_2149_nullable_decimal_int, int_rhs_rejected_3298) {
    // fix #3830: BOTH `d` and `n` are literal-initialized locals, so both
    // narrow (§7.4) and the row is fully admitted post-fix — parameterize
    // both to keep testing the "both present" verdict.
    BEZEL_EXPECT_TRUE(rejectedCompoundP(
        "d: decimal?, n: int?", "1.0m, 2", "d += n; return 0;"));
    // `d := null` never narrows, so it alone keeps this row rejected —
    // unaffected by #3830.
    BEZEL_EXPECT_TRUE(rejectedCompound(
        "var d: decimal? := null; var n: int? := 2; d += n; return 0;"));
    // `n := null` never narrows, so it alone keeps this row rejected even
    // though `d`'s literal initializer narrows — unaffected by #3830.
    BEZEL_EXPECT_TRUE(rejectedCompound(
        "var d: decimal? := 1.0m; var n: int? := null; d += n; return 0;"));
    BEZEL_EXPECT_TRUE(rejectedCompound(
        "var d: decimal? := null; var n: int? := null; d += n; return 0;"));
}
BEZEL_TEST(codegen_2149_nullable_decimal_int, long_and_uint_rhs_rejected_3298) {
    // fix #3830: both literal-narrowed — parameterized, see int_rhs above.
    BEZEL_EXPECT_TRUE(rejectedCompoundP(
        "d: decimal?, n: long?", "1.0m, 2", "d += n; return 0;"));
    // `n := null` keeps this rejected regardless of `d`'s narrowing.
    BEZEL_EXPECT_TRUE(rejectedCompound(
        "var d: decimal? := 1.0m; var n: long? := null; d += n; return 0;"));
    // `d := null` keeps this rejected regardless of `n`'s narrowing.
    BEZEL_EXPECT_TRUE(rejectedCompound(
        "var d: decimal? := null; var n: long? := 5; d += n; return 0;"));
    // fix #3830: both literal-narrowed — parameterized, see int_rhs above.
    BEZEL_EXPECT_TRUE(rejectedCompoundP(
        "d: decimal?, n: uint?", "1.0m, 7", "d += n; return 0;"));
}
BEZEL_TEST(codegen_2149_nullable_decimal_int, sub_mul_div_rejected_3298) {
    // fix #3830: all three rows are both literal-initialized locals — both
    // narrow, so both are parameterized. See int_rhs above.
    BEZEL_EXPECT_TRUE(rejectedCompoundP(
        "d: decimal?, a: int?", "12.0m, 4", "d /= a; return 0;"));
    BEZEL_EXPECT_TRUE(rejectedCompoundP(
        "d: decimal?, b: int?", "12.0m, 5", "d *= b; return 0;"));
    BEZEL_EXPECT_TRUE(rejectedCompoundP(
        "d: decimal?, c: int?", "12.0m, 1", "d -= c; return 0;"));
}
// The two "neighbour" rows: a nullable decimal target with a NON-nullable RHS
// is rejected on the target's side alone — which is the half of §6.6 the
// pre-#3298 rule did not have.
BEZEL_TEST(codegen_2149_nullable_decimal_int, neighbour_nonnullable_rhs_rejected_3298) {
    // fix #3830: `d`'s literal initializer narrows it (§7.4), and the RHS
    // here was ALREADY non-nullable (a bare literal), so once `d` narrows
    // both sides are non-nullable and the row is fully admitted — `d` is
    // parameterized to keep testing the genuinely nullable, unguarded
    // target this row names.
    BEZEL_EXPECT_TRUE(rejectedCompoundP("d: decimal?", "2.0m", "d += 3.0m; return 0;"));
    BEZEL_EXPECT_TRUE(rejectedCompoundP("d: decimal?", "4.0m", "d += 6; return 0;"));
}

// --- §5.1's integer→decimal promotion, on the surviving non-nullable path ---
BEZEL_TEST(codegen_2149_nullable_decimal_int, nonnullable_int_rhs_promotes) {
    BEZEL_EXPECT_EQ(run(
        "var d: decimal := 1.0m; var n: int := 2; d += n; "
        "if d.CompareTo(3.0m) = 0 then return 1; end; return 0;"), 1);
    BEZEL_EXPECT_EQ(run(
        "var d: decimal := 1.0m; var n: long := 2; d += n; "
        "if d.CompareTo(3.0m) = 0 then return 1; end; return 0;"), 1);
    BEZEL_EXPECT_EQ(run(
        "var d: decimal := 1.0m; var n: uint := 7; d += n; "
        "if d.CompareTo(8.0m) = 0 then return 1; end; return 0;"), 1);
    BEZEL_EXPECT_EQ(run(
        "var d: decimal := 12.0m; var a: int := 4; var b: int := 5; var c: int := 1; "
        "d /= a; d *= b; d -= c; "
        "if d.CompareTo(14.0m) = 0 then return 1; end; return 0;"), 1);
}
