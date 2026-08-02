// #2339 (stdlib §9.3, v0.195): Decimal.Round(digits) must raise
// ArgumentOutOfRangeException for digits outside the documented 0..28 range
// — NOT silently clamp digits < 0 to 0 and NOT silently return the operand
// for digits >= 28. Per stdlib §9.3:
//
//     public pure method Round(digits: int): decimal;    // banker's rounding
//                                                 //   (round-half-to-even),
//                                                 //   matching Math.Round
//                                                 //   (§2.2); digits outside
//                                                 //   0..28:
//                                                 //   ArgumentOutOfRangeException
//                                                 //   (v0.195)
//
// Test axes (each fails on pre-fix HEAD, passes after the codegen guard at
// LlvmBackend_Calls.cpp "Decimal.Round(digits: int): decimal" fires before
// the runtime call):
//   1. negative digit count        → AORE   (cardinal, the issue's repro)
//   2. digits > 28                  → AORE   (upper bound)
//   3. digits = 0                   → no throw, value is 2m for 1.5m (boundary)
//   4. digits = 28                  → no throw, value is 1.5m for 1.5m (boundary)
//   5. digits = 2                   → no throw, value is 1.5m for 1.5m (control)

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& body) {
    auto r = runAot(
        std::string("module M; class C public static method Main(): int ") +
        body + " end Main; end C;");
    return r.ran ? r.exitCode : -999;
}

} // namespace

// --- Cardinals: out-of-range digits MUST raise ArgumentOutOfRangeException.

// The issue's literal repro: (-1.5m).Round(-1). Pre-fix this silently
// clamped digits < 0 to 0 and returned -2m (banker's rounding of -15 at
// scale 0). Post-fix the codegen guard throws AORE before the runtime is
// reached. 42 is the canonical AORE sentinel used in
// negative_capacity_aoor_1396_test.cpp.
BEZEL_TEST(codegen_2339_round_aoore, negative_digits_raises_spec_9_3) {
    BEZEL_EXPECT_EQ(run(
        "try var v: decimal := (-1.5m).Round(-1); return 0; "
        "catch ArgumentOutOfRangeException => return 42; end;"), 42);
}

// Mirror cardinal: digits above the documented upper bound of 28. Pre-fix
// this silently returned the operand (scale is already ≤ 28 for 1.5m so
// the early-return path `digits >= s → *out = *a` fired). Post-fix AORE.
BEZEL_TEST(codegen_2339_round_aoore, digits_above_28_raises_spec_9_3) {
    BEZEL_EXPECT_EQ(run(
        "try var v: decimal := (1.5m).Round(29); return 0; "
        "catch ArgumentOutOfRangeException => return 42; end;"), 42);
}

// digits = 100 (a wildly-out-of-range value) — guards against an
// accidental `<=` vs `<` off-by-one in the upper-bound check.
BEZEL_TEST(codegen_2339_round_aoore, digits_100_raises_spec_9_3) {
    BEZEL_EXPECT_EQ(run(
        "try var v: decimal := (1.5m).Round(100); return 0; "
        "catch ArgumentOutOfRangeException => return 42; end;"), 42);
}

// digits = -1000000 (a wildly-out-of-range negative value) — guards against
// an accidental `>=` vs `>` off-by-one in the lower-bound check.
BEZEL_TEST(codegen_2339_round_aoore, digits_huge_negative_raises_spec_9_3) {
    BEZEL_EXPECT_EQ(run(
        "try var v: decimal := (1.5m).Round(-1000000); return 0; "
        "catch ArgumentOutOfRangeException => return 42; end;"), 42);
}

// --- Boundaries: in-range digits (0 and 28) MUST NOT throw.

// digits = 0: lower bound is inclusive. For 1.5m (mantissa 15, scale 1),
// banker's rounding at 0 places rounds the units-up: kept digit is 1 (odd),
// first-dropped digit is 5 (exact midpoint), so round up to 2m.
BEZEL_TEST(codegen_2339_round_aoore, digits_zero_accepted_lower_bound_spec_9_3) {
    BEZEL_EXPECT_EQ(run(
        "var v: decimal := (1.5m).Round(0); "
        "if v = 2m then return 1; end; "
        "return 0;"), 1);
}

// digits = 28: upper bound is inclusive. For 1.5m (scale 1), digits (28)
// is already >= scale, so the operand is returned unchanged. The guard
// must NOT fire on this boundary.
BEZEL_TEST(codegen_2339_round_aoore, digits_28_accepted_upper_bound_spec_9_3) {
    BEZEL_EXPECT_EQ(run(
        "var v: decimal := (1.5m).Round(28); "
        "if v = 1.5m then return 1; end; "
        "return 0;"), 1);
}

// --- Control: in-range digits do the right thing (not a rejection test).

// digits = 2 on 1.5m (scale 1): digits >= scale, operand returned. 1.5m
// stays 1.5m. Establishes that the guard doesn't accidentally trigger on
// the typical in-range call.
BEZEL_TEST(codegen_2339_round_aoore, digits_2_control_unchanged_spec_9_3) {
    BEZEL_EXPECT_EQ(run(
        "var v: decimal := (1.5m).Round(2); "
        "if v = 1.5m then return 1; end; "
        "return 0;"), 1);
}

// Variable digits (not a compile-time literal) — the guard must work for
// runtime digits, not just constants. `digits` is bound to -1 via a local,
// proving the runtime ICmpSLT/ICmpSGT fires correctly.
BEZEL_TEST(codegen_2339_round_aoore, runtime_negative_digits_raises_spec_9_3) {
    BEZEL_EXPECT_EQ(run(
        "var d: int := -1; "
        "try var v: decimal := (1.5m).Round(d); return 0; "
        "catch ArgumentOutOfRangeException => return 42; end;"), 42);
}

// Variable digits = 29 (upper bound breach via a local). Same shape as
// the literal 29 cardinal but proves the runtime guard isn't being
// constant-folded into a static reject.
BEZEL_TEST(codegen_2339_round_aoore, runtime_digits_above_28_raises_spec_9_3) {
    BEZEL_EXPECT_EQ(run(
        "var d: int := 29; "
        "try var v: decimal := (1.5m).Round(d); return 0; "
        "catch ArgumentOutOfRangeException => return 42; end;"), 42);
}