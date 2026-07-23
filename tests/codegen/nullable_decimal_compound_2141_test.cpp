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

}  // namespace

// --- The exact issue repro: `decimal? := 1.0m; d += 2.0m;` no longer crashes
// codegen and yields 3.0m. §7.11 lift over the §6.4 decimal add. FAILS WITHOUT
// the fix (bezelc IR-verify abort — the compile never produces a binary). ---
BEZEL_TEST(codegen_2141_nullable_decimal, plus_equals_present) {
    BEZEL_EXPECT_EQ(run(
        "var d: decimal? := 1.0m; d += 2.0m; "
        "if (d ?? -1.0m).CompareTo(3.0m) = 0 then return 1; end; return 0;"), 1);
}

// §7.11: both operands nullable AND present → the op runs; 1.0m + 2.0m = 3.0m.
BEZEL_TEST(codegen_2141_nullable_decimal, both_nullable_present) {
    BEZEL_EXPECT_EQ(run(
        "var d: decimal? := 1.0m; var e: decimal? := 2.0m; d += e; "
        "if (d ?? -1.0m).CompareTo(3.0m) = 0 then return 1; end; return 0;"), 1);
}

// §7.11: a `null` LHS propagates null — no op runs, the target stays null.
BEZEL_TEST(codegen_2141_nullable_decimal, lhs_null_stays_null) {
    BEZEL_EXPECT_EQ(run(
        "var d: decimal? := null; d += 2.0m; "
        "if d = null then return 1; end; return 0;"), 1);
}

// §7.11 (the review's missing RHS-null case): a `null` nullable RHS propagates
// null — the present LHS is discarded, the result is null.
BEZEL_TEST(codegen_2141_nullable_decimal, rhs_null_propagates_null) {
    BEZEL_EXPECT_EQ(run(
        "var d: decimal? := 1.0m; var e: decimal? := null; d += e; "
        "if d = null then return 1; end; return 0;"), 1);
}

// §7.11: both operands null → null.
BEZEL_TEST(codegen_2141_nullable_decimal, both_null_stays_null) {
    BEZEL_EXPECT_EQ(run(
        "var d: decimal? := null; var e: decimal? := null; d += e; "
        "if d = null then return 1; end; return 0;"), 1);
}

// §6.4: the other operators lift too — 12/4 = 3, *5 = 15, -1 = 14.
BEZEL_TEST(codegen_2141_nullable_decimal, sub_mul_div_present) {
    BEZEL_EXPECT_EQ(run(
        "var d: decimal? := 12.0m; d /= 4.0m; d *= 5.0m; d -= 1.0m; "
        "if (d ?? -1.0m).CompareTo(14.0m) = 0 then return 1; end; return 0;"), 1);
}

// §7.11 / §6.4: division-by-zero raises DivideByZeroException on the present
// branch.
BEZEL_TEST(codegen_2141_nullable_decimal, div_by_zero_present_throws) {
    BEZEL_EXPECT_EQ(throwsDiv0("var d: decimal? := 10.0m; d /= 0.0m;"), 1);
}

// §7.11: the div/mod zero check is present-branch only — a `null` LHS divided by
// zero never runs the op, so it does NOT raise; the result is null.
BEZEL_TEST(codegen_2141_nullable_decimal, div_by_zero_null_lhs_no_throw) {
    BEZEL_EXPECT_EQ(throwsDiv0("var d: decimal? := null; d /= 0.0m;"), 0);
    BEZEL_EXPECT_EQ(run(
        "var d: decimal? := null; d /= 0.0m; "
        "if d = null then return 1; end; return 0;"), 1);
}

// §5.1 / §6.2: an integer RHS §5.1-converts to `decimal` — `d += 5` = 8.0m.
BEZEL_TEST(codegen_2141_nullable_decimal, int_rhs_converts_to_decimal) {
    BEZEL_EXPECT_EQ(run(
        "var d: decimal? := 3.0m; d += 5; "
        "if (d ?? -1.0m).CompareTo(8.0m) = 0 then return 1; end; return 0;"), 1);
}

// Control: a non-nullable `decimal` compound is undisturbed by the nullable
// branch (still routed through the §6.4 intercept). 1.0m + 2.0m = 3.0m.
BEZEL_TEST(codegen_2141_nullable_decimal, non_nullable_decimal_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "var d: decimal := 1.0m; d += 2.0m; "
        "if d.CompareTo(3.0m) = 0 then return 1; end; return 0;"), 1);
}
