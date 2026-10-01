// Codegen tests for fix #5190: §5.2 v0.311 shortest round-trip decimal conversion.
//
// Spec reference:
//   §5.2 (v0.311): "decimal(x) for binary-float x yields the value of x's
//   shortest round-trip form — the digits x's no-format ToString() produces,
//   the shortest decimal string that parses back to x in x's own type and,
//   of several such, the one nearest x — rounded as §4.4 states where that
//   value is not in decimal's set:
//     decimal(0.1)           → 0.1
//     decimal(0.1 + 0.2)     → 0.30000000000000004
//     decimal(0.1f)          → 0.1
//     decimal(2.5e-28)       → 0.0000000000000000000000000002  (tie at scale 28 → even digit)
//
// Issue: #5190 (OPEN, spec-ahead-of-impl)
// Branch: a24-5190 / WT: /mnt/fast/worktrees/a24-wt-5190
// Base commit: e2aa55d0

#include <cstdio>
#include <string>
#include <sys/wait.h>

#include "driver/Driver.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

std::string prog(const std::string& mainBody) {
    return "module M; "
           "class C public static method Main(): int "
           + mainBody +
           " end Main; end C;";
}

// Assert a decimal value equals the given decimal string literal.
int assertDecimalEq(const std::string& expr, const char* expected, int ok = 42) {
    return run(prog(
        "var d := " + expr + "; "
        "if d <> " + std::string(expected) + "m then return 1; end; "
        "return " + std::to_string(ok) + ";"));
}

} // namespace

// ---------------------------------------------------------------------------
// §5.2 v0.311: shortest round-trip decimal conversion
// ---------------------------------------------------------------------------

// decimal(0.1) — double 0.1 shortest round-trip is "0.1" → decimal(0.1) = 0.1m
BEZEL_TEST(codegen_5190_shortest_roundtrip, decimal_0_1_double_shortest_spec_5_2_v0_311) {
    // 0.1 is exactly representable as double; shortest round-trip = "0.1"
    BEZEL_EXPECT_EQ(assertDecimalEq("decimal(0.1)", "0.1"), 42);
}

// decimal(0.1 + 0.2) — double result's shortest round-trip is "0.30000000000000004"
BEZEL_TEST(codegen_5190_shortest_roundtrip, decimal_0_1_plus_0_2_double_shortest_spec_5_2_v0_311) {
    // 0.1 + 0.2 = 0.30000000000000004 in double (binary FP accumulation).
    // The shortest round-trip of 0.30000000000000004 is "0.30000000000000004".
    BEZEL_EXPECT_EQ(assertDecimalEq("decimal(0.1 + 0.2)", "0.30000000000000004"), 42);
}

// decimal(0.1f) — float 0.1 shortest round-trip is "0.1" (not "0.100000024")
BEZEL_TEST(codegen_5190_shortest_roundtrip, decimal_0_1f_float_shortest_spec_5_2_v0_311) {
    // The old %.15g approach emitted "0.100000024" for float 0.1f, losing the
    // shortest round-trip property. The fix uses float-specific search (1..9 digits).
    BEZEL_EXPECT_EQ(assertDecimalEq("decimal(0.1f)", "0.1"), 42);
}

// decimal(1.5) — double 1.5 is exactly representable; shortest = "1.5"
BEZEL_TEST(codegen_5190_shortest_roundtrip, decimal_1_5_double_shortest_spec_5_2_v0_311) {
    BEZEL_EXPECT_EQ(assertDecimalEq("decimal(1.5)", "1.5"), 42);
}

// decimal(2.5e-28) — SPEC ROW (§5.2 v0.311 line 768, §4.4 ties-to-even).
// Shortest round-trip of 2.5e-28 is "2.5e-28" → parses to mantissa 25 scale 29.
// Reducing scale 29→28: dropped digit is 5, no further digits → exact tie.
// Ties-to-even: mantissa 25 has last digit 5 (odd) → round UP to 26 → 26e-28 = 2.6e-27.
// Wait: that's wrong. Let me re-trace:
//   mantissa 25, scale 29 → "2.5e-28" = 2.5 × 10^-28 = 2.5e-28
//   Scale 28 target: 2.5e-28 = X × 10^-28 = X × 10^-28 → X = 2.5 → not integer
//   Scale 28 tie: 2 vs 3 at scale 28
//     2e-28 = 0.000...0002  (mantissa 2, scale 28)
//     3e-28 = 0.000...0003  (mantissa 3, scale 28)
//   2.5e-28 is EXACTLY between them → tie, ties-to-even picks EVEN mantissa = 2.
//   So decimal(2.5e-28) = 2e-28 = 0.000...0002 ✓ per spec.
BEZEL_TEST(codegen_5190_shortest_roundtrip, decimal_2_5e_28_spec_5_2_4_4_ties_to_even) {
    // §5.2 v0.311 line 768: "decimal(2.5e-28) is 0.0000000000000000000000000002,
    //  the form's tie at scale 28 going to the even digit"
    BEZEL_EXPECT_EQ(assertDecimalEq("decimal(2.5e-28)", "0.0000000000000000000000000002"), 42);
}

// decimal(2.6e-28) — NON-TIE: dropped digit is 6 (> 5) → round UP.
// Shortest round-trip "2.6e-28" → 26e-28 (mantissa 26, scale 28) = 2.6e-28 exactly.
BEZEL_TEST(codegen_5190_shortest_roundtrip, decimal_2_6e_28_non_tie_round_up_spec_5_2_4_4) {
    // Dropped digit 6 > 5: always round up → mantissa 26 → 0.000...0003
    BEZEL_EXPECT_EQ(assertDecimalEq("decimal(2.6e-28)", "0.0000000000000000000000000003"), 42);
}

// decimal(3.5e-28) — ODD-TIE: dropped digit is 5, all subsequent digits are 0,
// mantissa's last digit is 5 (odd) → ties-to-even rounds UP to 6.
// Shortest round-trip "3.5e-28" → 35e-28 (mantissa 35, scale 28).
// Reducing scale 28→27: dropped digit 5, tie, mantissa 35 last digit 5 (odd) → +1 → 36.
// Result: 36e-28 = 3.6e-27 = 0.000...0004? No:
//   35e-28 = 3.5 × 10^-28
//   Scale 28: 35e-28 = X × 10^-28 → X = 35
//   3.5e-28 tie between 3e-28 and 4e-28 (mantissa 3 vs 4 at scale 28).
//   Mantissa 35 → last digit 5 (odd) → round up to 36e-28.
//   36e-28 = 0.000...0004 at scale 28 (wait: 36e-28 = 3.6 × 10^-27 ≠ 4 × 10^-28)
// Actually: 36e-28 with scale 28 = 36 × 10^-28 = 3.6 × 10^-27.
// But the string "0.000...0004" at scale 28 = 4 × 10^-28 = 4e-28.
// Let me re-trace:
//   2.5e-28 = 25e-29 (mantissa 25, scale 29) → round to scale 28: tie 2e-28 vs 3e-28.
//   2.5e-28 is exactly in between → ties-to-even → 2e-28 (mantissa 2, even) → 0.000...0002
//   2.6e-28 = 26e-29 (mantissa 26, scale 29) → reduce to scale 28: 26e-29 = 2.6 × 10^-28.
//   At scale 28: 2.6e-28 is NOT exactly representable; 2.6 is between 2e-28 and 3e-28.
//   2.6e-28 is closer to 3e-28 (distance 0.4e-28) than 2e-28 (distance 0.6e-28) → rounds to 3.
//   So decimal(2.6e-28) = 3e-28 = 0.000...0003. ✓
//   3.5e-28 = 35e-29 (mantissa 35, scale 29) → reduce to scale 28: 35e-29 = 3.5 × 10^-28.
//   At scale 28: 3.5e-28 is exactly between 3e-28 and 4e-28 (mantissa 3 vs 4).
//   Mantissa 35 → last digit 5 (odd) → round up to 36e-28 = 3.6 × 10^-27 = 0.000...0004 at scale 27? No!
//   36e-28 at scale 28 = 36 × 10^-28 = 3.6 × 10^-27.
//   Wait: 0.000...0004 at scale 28 = 4 × 10^-28 = 4e-28.
//   Let me redo:
//   - 2.5e-28: mantissa 25, scale 29. Reduce 1 digit (k=0): q=2, r=5. first_drop_eq_5=true.
//     m=2 after loop. any_nonzero_dropped=false. first_drop_eq_5 && m%2==1 → 2%2==0 (even), so NO round-up.
//     Result: mantissa 2, scale 28 = 0.000...0002 ✓
//   - 2.6e-28: mantissa 26, scale 29. Reduce 1 digit: q=2, r=6. first_drop_eq_5=false.
//     any_nonzero_dropped=true. Round up → m=3. Result: mantissa 3, scale 28 = 0.000...0003 ✓
//   - 3.5e-28: mantissa 35, scale 29. Reduce 1 digit: q=3, r=5. first_drop_eq_5=true, m=3.
//     any_nonzero_dropped=false. first_drop_eq_5 && m%2==1 → 3%2==1 (odd) → round up → m=4.
//     Result: mantissa 4, scale 28 = 0.000...0004 ✓
BEZEL_TEST(codegen_5190_shortest_roundtrip, decimal_3_5e_28_odd_tie_round_up_spec_5_2_4_4) {
    // §4.4 / §6.4 ties-to-even: 3.5e-28 tie at scale 28, mantissa 35 last digit 5 (odd)
    // → round to even 6, so 36e-28 = 3.6e-27 = 0.000...0004? NO:
    // Re-trace: 3.5e-28 = 35e-29. Reduce scale 29→28: q=3, r=5. Tie. m=3 (odd).
    // Round UP → m=4. 4e-28 = 0.000...0004. ✓
    BEZEL_EXPECT_EQ(assertDecimalEq("decimal(3.5e-28)", "0.0000000000000000000000000004"), 42);
}

// decimal(-0.1) — negative double
BEZEL_TEST(codegen_5190_shortest_roundtrip, decimal_neg_0_1_double_shortest_spec_5_2_v0_311) {
    BEZEL_EXPECT_EQ(assertDecimalEq("decimal(-0.1)", "-0.1"), 42);
}

// decimal(0.0) — zero
BEZEL_TEST(codegen_5190_shortest_roundtrip, decimal_0_0_double_shortest_spec_5_2_v0_311) {
    BEZEL_EXPECT_EQ(assertDecimalEq("decimal(0.0)", "0"), 42);
}

// decimal(1.0) — one
BEZEL_TEST(codegen_5190_shortest_roundtrip, decimal_1_0_double_shortest_spec_5_2_v0_311) {
    BEZEL_EXPECT_EQ(assertDecimalEq("decimal(1.0)", "1"), 42);
}

// decimal(0.2f) — float 0.2 shortest round-trip
BEZEL_TEST(codegen_5190_shortest_roundtrip, decimal_0_2f_float_shortest_spec_5_2_v0_311) {
    BEZEL_EXPECT_EQ(assertDecimalEq("decimal(0.2f)", "0.2"), 42);
}

// decimal(NaN) — raises OverflowException per §5.2 (compile-time constant) or
// is rejected at compile time (fix #4801 v0.307). This tests a RUNTIME NaN
// (non-constant operand) which raises OverflowException at runtime.
BEZEL_TEST(codegen_5190_shortest_roundtrip, decimal_nan_overflow_spec_5_2_v0_311) {
    BEZEL_EXPECT_EQ(run(prog(
        "var n: double := 0.0 / 0.0; "   // non-constant: runtime NaN
        "try "
        "  var d := decimal(n); "
        "  return 1; "   // should not reach
        "catch OverflowException => return 42; "
        "end; "
        "return 2; ")), 42);
}

// decimal(+Infinity) — raises OverflowException per §5.2
BEZEL_TEST(codegen_5190_shortest_roundtrip, decimal_pos_inf_overflow_spec_5_2_v0_311) {
    BEZEL_EXPECT_EQ(run(prog(
        "try "
        "  var inf := 1.0 / 0.0; "
        "  var d := decimal(inf); "
        "  return 1; "
        "catch OverflowException => return 42; "
        "end; "
        "return 2; ")), 42);
}

// decimal(-Infinity) — raises OverflowException per §5.2
BEZEL_TEST(codegen_5190_shortest_roundtrip, decimal_neg_inf_overflow_spec_5_2_v0_311) {
    BEZEL_EXPECT_EQ(run(prog(
        "try "
        "  var inf := -1.0 / 0.0; "
        "  var d := decimal(inf); "
        "  return 1; "
        "catch OverflowException => return 42; "
        "end; "
        "return 2; ")), 42);
}

// decimal(3.14159) — double that is not exactly representable
BEZEL_TEST(codegen_5190_shortest_roundtrip, decimal_pi_double_shortest_spec_5_2_v0_311) {
    // double pi ≈ 3.141592653589793; shortest round-trip is "3.141592653589793"
    BEZEL_EXPECT_EQ(assertDecimalEq("decimal(3.14159)", "3.14159"), 42);
}
