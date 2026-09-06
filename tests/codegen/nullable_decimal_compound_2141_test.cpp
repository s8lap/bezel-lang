// Codegen — #2141 (§7.11 / §6.4): a compound assignment whose LHS is `decimal?`
// must LIFT per §7.11, not crash codegen with malformed IR.
//
// The target slot is the tagged `{i8 hasValue; %bz.decimal}` struct. The #1964
// nullable-value lifting handled scalar (int/FP) payloads but EXCLUDED the
// decimal struct payload, and the non-nullable decimal intercept matches only
// the bare `%bz.decimal` (== decimalStructType()), so `var d: decimal? := m;
// d += n;` fell to the built-in scalar emitCompoundOp, which fed the whole
// `{i8; %bz.decimal}` struct to `llvm.sadd.with.overflow` and aborted the IR
// verifier (`llvm.sadd.with.overflow.sl_i8s_bz.decimalss`) — the same crash class
// #1964 fixed for scalars.
//
// §7.11 (spec v0.214) lifts the arithmetic operators to `T?`: the result is
// `null` if EITHER operand is `null`, and the underlying operation and its runtime
// checks (§7.11 names `decimal` DivideByZeroException explicitly, §6.4) run ONLY
// when both operands are present. The fix routes the unwrapped `%bz.decimal`
// payloads through the §6.4 decimal intrinsic (emitDecimalArith) on the both-
// present branch and yields `null` otherwise. This test pins the whole family:
// present/present, both-nullable present, LHS-null and RHS-null propagation,
// both-null, the chained sub/mul/div value, decimal DivideByZeroException on the
// present branch, div-by-zero SUPPRESSED when the LHS is null (present-branch
// only), and an integer RHS §5.1-converted to decimal.

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

// Returns 1 if `body` raises DivideByZeroException, 0 if it runs to completion,
// -999 on a front-end failure.
int throwsDiv0(const std::string& body) {
    auto r = runAot(
        std::string("module M; class C public static method Main(): int "
                    "  try ") +
        body +
        " return 0; catch DivideByZeroException => return 1; end; return 0; "
        "end Main; end C;");
    return r.ran ? r.exitCode : -999;
}

// ⚠ THE RULE CHANGED AT v0.267 (proposal 0114, #3298). §7.11 (v0.286) opens
// "No operator **lifts** to a nullable type" and §6.6 makes a read-modify-write
// compound "a **compile error** when the static type of its left-hand side, or
// of its right operand, is a **nullable** type". Every row below asserted a
// value the lifting produced; each now asserts the diagnosis, paired with the
// NON-NULLABLE twin so the §6.4 decimal-runtime route and §6.1's
// DivideByZeroException — the rules this file exists for — keep their value
// coverage. The narrowed-`decimal?` ARITHMETIC route (not compound) is covered
// by `codegen_lifted_ops_486_narrowed_3298`, which is where a `decimal?`
// operand still reaches the runtime after #3298.

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
// whose "still rejected" operand(s) must NOT be §7.4-narrowed. `decimal` is
// `TypeRef::Kind::Primitive` exactly like `int`/`byte`/etc, so a local
// declared `var x: decimal? := <literal>;` narrows at its initializer just
// as an int local does, and #3830 makes such a narrowed operand admitted at
// compound assignment — so a row meant to test the genuinely-nullable,
// UNGUARDED gate must route its value(s) through PARAMETER(s) instead: a
// parameter is never narrowed by what the caller passes (narrowing needs an
// in-body null test, §7.4). `paramDecl` is the `Chk` method's parameter list
// (e.g. "d: decimal?" or "d: decimal?, e: decimal?"), `argExprs` the values
// passed at the call site, and `body` the helper's statements (referencing
// the parameter(s) by name). Same diagnostic gate as `rejectedCompound`.
// Modeled on nullable_value_compound_1964_test.cpp's `rejectedCompoundP`.
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

// Nullable target, nullable RHS, and both — §6.6 rejects each ("either
// position, or both"). The `null` spellings are rejected for the same reason
// as the present ones: the gate reads the DECLARED type.
BEZEL_TEST(codegen_2141_nullable_decimal, nullable_target_rejected_3298) {
    // fix #3830: `1.0m` is a decimal literal — a local initialized with it
    // narrows (§7.4); routed through a parameter to keep testing the
    // genuinely nullable, unguarded target.
    BEZEL_EXPECT_TRUE(rejectedCompoundP("d: decimal?", "1.0m", "d += 2.0m; return 0;"));
    BEZEL_EXPECT_TRUE(rejectedCompound("var d: decimal? := null; d += 2.0m; return 0;"));
}
BEZEL_TEST(codegen_2141_nullable_decimal, nullable_rhs_rejected_3298) {
    // fix #3830: both `d` and `e` are literal-initialized locals, so BOTH
    // narrow (§7.4) and the row is fully admitted post-fix — parameterize
    // both to keep testing the "both nullable" verdict this row names.
    BEZEL_EXPECT_TRUE(rejectedCompoundP(
        "d: decimal?, e: decimal?", "1.0m, 2.0m", "d += e; return 0;"));
    // fix #3830: `d` here is already non-nullable (`decimal`, not
    // `decimal?`) so it is unaffected; `e`'s literal initializer narrows it
    // — parameterize `e` only, keeping the file's "RIGHT-operand half" shape
    // (non-nullable target, nullable RHS).
    BEZEL_EXPECT_TRUE(rejectedCompoundP(
        "e: decimal?", "2.0m", "var d: decimal := 1.0m; d += e; return 0;"));
    BEZEL_EXPECT_TRUE(rejectedCompound(
        "var d: decimal? := null; var e: decimal? := null; d += e; return 0;"));
}
BEZEL_TEST(codegen_2141_nullable_decimal, sub_mul_div_rejected_3298) {
    // fix #3830: all three targets are literal-initialized locals —
    // parameterized, see nullable_target_rejected_3298 above.
    BEZEL_EXPECT_TRUE(rejectedCompoundP("d: decimal?", "12.0m", "d /= 4.0m; return 0;"));
    BEZEL_EXPECT_TRUE(rejectedCompoundP("d: decimal?", "12.0m", "d *= 5.0m; return 0;"));
    BEZEL_EXPECT_TRUE(rejectedCompoundP("d: decimal?", "12.0m", "d -= 1.0m; return 0;"));
}
BEZEL_TEST(codegen_2141_nullable_decimal, int_rhs_rejected_3298) {
    // fix #3830: parameterized — see nullable_target_rejected_3298 above.
    BEZEL_EXPECT_TRUE(rejectedCompoundP("d: decimal?", "3.0m", "d += 5; return 0;"));
}

// --- the rules this file exists for, on the surviving (non-nullable) path ---

BEZEL_TEST(codegen_2141_nullable_decimal, non_nullable_decimal_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "var d: decimal := 1.0m; d += 2.0m; "
        "if d.CompareTo(3.0m) = 0 then return 1; end; return 0;"), 1);
}
BEZEL_TEST(codegen_2141_nullable_decimal, non_nullable_sub_mul_div_and_int_rhs) {
    BEZEL_EXPECT_EQ(run(
        "var d: decimal := 12.0m; d /= 4.0m; d *= 5.0m; d -= 1.0m; "
        "if d.CompareTo(14.0m) = 0 then return 1; end; return 0;"), 1);
    BEZEL_EXPECT_EQ(run(
        "var d: decimal := 3.0m; d += 5; "
        "if d.CompareTo(8.0m) = 0 then return 1; end; return 0;"), 1);
}
// §6.1's DivideByZeroException — §7.11: the operators' runtime checks are
// "unchanged and unmoved … reached after the operands are narrowed".
BEZEL_TEST(codegen_2141_nullable_decimal, div_by_zero_throws_nonnullable) {
    BEZEL_EXPECT_EQ(throwsDiv0("var d: decimal := 10.0m; d /= 0.0m;"), 1);
}
