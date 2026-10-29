// Spec-derived tests for #1412 — decimal E/G/P format specifiers + strict
// FormatException on unrecognized specifiers (stdlib §2.18, v0.172, spec §18.5).
//
// Before fix #1412:
//   - P, E silently degraded to plain ToString() ("1.5" instead of "150.0%" or "1.500000E+000").
//   - G silently degraded to plain ToString() (correct output by coincidence, but no spec fidelity).
//   - Unrecognized specifiers (e.g. "Z", "QQ") silently degraded to ToString() — must raise FormatException.
//
// Stdlib §2.18 decimal specifier vocabulary:
//   F/Fn — fixed-point (n decimal places; the house's default 2, which the
//          §2.18 decimal row does not fix)
//   E/En — scientific  (n decimal places in significand; the house's default
//          6, which §2.18 does not fix; 3-digit exponent; sign always printed)
//   G    — general (exact natural representation — shortest exact decimal form)
//   P/Pn — percent (×100, fixed-point with n places, default 2, "%" suffix)
//   Anything else → FormatException (strict-fault, §2.18/§18.5).

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ── F (control — existing behavior, §2.18) ────────────────────────────────────

// §2.18: F2 on 1.5m → "1.50"; F (default 2) on 1234.5m → "1234.50".
// These are pre-existing; included as a regression guard.
BEZEL_TEST(decimal_format_1412, F_control_spec_2_18) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  var ok := 0;
  var d := 1.5m;
  if $"{d:F2}" = "1.50" then ok := ok + 1; end;
  if $"{d:F}" = "1.50" then ok := ok + 2; end;   // §2.18: F default = 2
  if $"{1234.5m:F}" = "1234.50" then ok := ok + 4; end;
  if $"{0m:F2}" = "0.00" then ok := ok + 8; end;
  return ok;
end Main; end C;
)BZ"), 15);
}

// ── P/Pn: percent (×100, fixed-point, "%" suffix) — §2.18 #1412 ──────────────

// §2.18: P1 on 1.5m → "150.0%" (issue #1412 exact repro).
// Before fix: silently degraded to "1.5".
BEZEL_TEST(decimal_format_1412, P1_issue_repro_spec_2_18) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  var d: decimal := 1.5m;
  return (if $"{d:P1}" = "150.0%" then 1 else 0);
end Main; end C;
)BZ"), 1);
}

// §2.18: P (default 2 decimal places) on 0.15m → "15.00%".
BEZEL_TEST(decimal_format_1412, P_default_prec_spec_2_18) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  var ok := 0;
  if $"{0.15m:P}" = "15.00%" then ok := ok + 1; end;   // P default = 2
  if $"{0.15m:P2}" = "15.00%" then ok := ok + 2; end;
  if $"{0.15m:P1}" = "15.0%" then ok := ok + 4; end;
  if $"{0.15m:P0}" = "15%" then ok := ok + 8; end;
  return ok;
end Main; end C;
)BZ"), 15);
}

// §2.18: P on zero → "0.00%".
BEZEL_TEST(decimal_format_1412, P_zero_spec_2_18) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  var ok := 0;
  if $"{0m:P}" = "0.00%" then ok := ok + 1; end;
  if $"{0m:P0}" = "0%" then ok := ok + 2; end;
  return ok;
end Main; end C;
)BZ"), 3);
}

// §2.18: P on small fractional value — 0.005m with P1 → "0.5%".
BEZEL_TEST(decimal_format_1412, P_small_fractional_spec_2_18) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  var ok := 0;
  if $"{0.005m:P1}" = "0.5%" then ok := ok + 1; end;
  if $"{0.001m:P2}" = "0.10%" then ok := ok + 2; end;
  return ok;
end Main; end C;
)BZ"), 3);
}

// §2.18: P on negative value → "-15.00%".
BEZEL_TEST(decimal_format_1412, P_negative_spec_2_18) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  return (if $"{-0.15m:P2}" = "-15.00%" then 1 else 0);
end Main; end C;
)BZ"), 1);
}

// §2.18: P on integer-scale decimal — 1m (scale=0) P2 → "100.00%".
BEZEL_TEST(decimal_format_1412, P_integer_scale_spec_2_18) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  var ok := 0;
  if $"{1m:P2}" = "100.00%" then ok := ok + 1; end;
  if $"{2m:P0}" = "200%" then ok := ok + 2; end;
  return ok;
end Main; end C;
)BZ"), 3);
}

// ── E/En: scientific notation — §2.18 #1412 ──────────────────────────────────

// §2.18: E2 on 1.5m → "1.50E+000" (issue #1412 repro).
// Before fix: silently degraded to "1.5".
// Per §2.18 "exact decimal semantics": 3-digit exponent, sign always printed.
BEZEL_TEST(decimal_format_1412, E2_issue_repro_spec_2_18) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  var d: decimal := 1.5m;
  return (if $"{d:E2}" = "1.50E+000" then 1 else 0);
end Main; end C;
)BZ"), 1);
}

// §2.18: E (default 6 significant decimal places) on 1.5m → "1.500000E+000".
BEZEL_TEST(decimal_format_1412, E_default_prec_spec_2_18) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  return (if $"{1.5m:E}" = "1.500000E+000" then 1 else 0);
end Main; end C;
)BZ"), 1);
}

// §2.18: positive exponent — 1500.0m:E2 → "1.50E+003".
BEZEL_TEST(decimal_format_1412, E_positive_exponent_spec_2_18) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  return (if $"{1500.0m:E2}" = "1.50E+003" then 1 else 0);
end Main; end C;
)BZ"), 1);
}

// §2.18: zero exponent — 1.0m:E4 → "1.0000E+000".
BEZEL_TEST(decimal_format_1412, E_zero_exponent_spec_2_18) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  return (if $"{1.0m:E4}" = "1.0000E+000" then 1 else 0);
end Main; end C;
)BZ"), 1);
}

// §2.18: negative exponent — 0.015m:E2 → "1.50E-002".
BEZEL_TEST(decimal_format_1412, E_negative_exponent_spec_2_18) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  return (if $"{0.015m:E2}" = "1.50E-002" then 1 else 0);
end Main; end C;
)BZ"), 1);
}

// §2.18: negative value — (-0.015m):E2 → "-1.50E-002".
BEZEL_TEST(decimal_format_1412, E_negative_value_spec_2_18) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  return (if $"{-0.015m:E2}" = "-1.50E-002" then 1 else 0);
end Main; end C;
)BZ"), 1);
}

// §2.18: zero value — 0m:E2 → "0.00E+000".
BEZEL_TEST(decimal_format_1412, E_zero_value_spec_2_18) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  return (if $"{0m:E2}" = "0.00E+000" then 1 else 0);
end Main; end C;
)BZ"), 1);
}

// §2.18: lowercase 'e' → lowercase exponent marker.
BEZEL_TEST(decimal_format_1412, e_lowercase_spec_2_18) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  return (if $"{1.5m:e2}" = "1.50e+000" then 1 else 0);
end Main; end C;
)BZ"), 1);
}

// §2.18: rounding in E — 1.2345679m:E5 rounds to "1.23457E+000".
BEZEL_TEST(decimal_format_1412, E_rounding_spec_2_18) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  return (if $"{1.2345679m:E5}" = "1.23457E+000" then 1 else 0);
end Main; end C;
)BZ"), 1);
}

// ── G: general (exact natural representation) — §2.18 #1412 ─────────────────

// §2.18: G on 1.5m → "1.5" (natural/exact shortest decimal form).
// Before fix: silently degraded (happened to produce correct output but no spec fidelity).
BEZEL_TEST(decimal_format_1412, G_basic_spec_2_18) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  var ok := 0;
  if $"{1.5m:G}" = "1.5" then ok := ok + 1; end;
  if $"{1234m:G}" = "1234" then ok := ok + 2; end;
  if $"{0.001m:G}" = "0.001" then ok := ok + 4; end;
  return ok;
end Main; end C;
)BZ"), 7);
}

// §2.18: G must equal no-format ToString for decimal (both = natural form).
BEZEL_TEST(decimal_format_1412, G_equals_noformat_spec_2_18) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  var ok := 0;
  var d1 := 3.14m;
  var d2 := 0.001m;
  if $"{d1:G}" = $"{d1}" then ok := ok + 1; end;
  if $"{d2:G}" = $"{d2}" then ok := ok + 2; end;
  return ok;
end Main; end C;
)BZ"), 3);
}

// ── Unrecognized specifier → FormatException (§2.18 strict-fault) ────────────

// §2.18: unrecognized specifier "Z" on decimal must raise FormatException.
// Issue #1412 exact repro: `$"{d:Z}"` should throw, not silently return "1.5".
BEZEL_TEST(decimal_format_1412, unrecognized_Z_raises_FormatException_spec_2_18) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  var result := 0;
  var d: decimal := 1.5m;
  try
    var s := $"{d:Z}";
    result := 99;   // must not reach here
  catch FormatException => result := 7;
  end;
  return result;
end Main; end C;
)BZ"), 7);
}

// §2.18: unrecognized multi-char specifier "QQ" on decimal → FormatException.
BEZEL_TEST(decimal_format_1412, unrecognized_QQ_raises_FormatException_spec_2_18) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  var result := 0;
  var d := 2.5m;
  try
    var s := $"{d:QQ}";
    result := 99;
  catch FormatException => result := 3;
  end;
  return result;
end Main; end C;
)BZ"), 3);
}

// §2.18: "R" is NOT in the decimal vocabulary (floating only) → FormatException.
BEZEL_TEST(decimal_format_1412, R_not_decimal_spec_raises_FormatException_spec_2_18) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  var result := 0;
  var d := 1.5m;
  try
    var s := $"{d:R}";
    result := 99;
  catch FormatException => result := 5;
  end;
  return result;
end Main; end C;
)BZ"), 5);
}

// §2.18: "B" is NOT in the decimal vocabulary (integer only) → FormatException.
BEZEL_TEST(decimal_format_1412, B_not_decimal_spec_raises_FormatException_spec_2_18) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  var result := 0;
  var d := 1.5m;
  try
    var s := $"{d:B}";
    result := 99;
  catch FormatException => result := 2;
  end;
  return result;
end Main; end C;
)BZ"), 2);
}

// §2.18: decimal.ToString("Z") also raises FormatException (explicit call path).
BEZEL_TEST(decimal_format_1412, tostring_unrecognized_raises_FormatException_spec_2_18) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  var result := 0;
  var d := 1.5m;
  try
    var s := d.ToString("Z");
    result := 99;
  catch FormatException => result := 11;
  end;
  return result;
end Main; end C;
)BZ"), 11);
}

// §2.18: Gn (G with a digit count) is NOT in the decimal vocabulary (decimal has only G,
// not Gn) → FormatException.
BEZEL_TEST(decimal_format_1412, Gn_not_decimal_spec_raises_FormatException_spec_2_18) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  var result := 0;
  var d := 1.5m;
  try
    var s := d.ToString("G2");
    result := 99;
  catch FormatException => result := 4;
  end;
  return result;
end Main; end C;
)BZ"), 4);
}

// fix #2231: F29 (count > max scale 28) raises FormatException — the produced
// string would have 29 fractional digits, which Decimal.Parse cannot represent
// (scale bound is 0..28 per §9.3 v0.195).
BEZEL_TEST(decimal_format_1412, F29_above_max_raises_FormatException_spec_2_18_9_3) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  var result := 0;
  var d := 1.5m;
  try
    var s := d.ToString("F29");
    result := 99;   // BUG: should not reach here
  catch FormatException => result := 5;
  end;
  return result;
end Main; end C;
)BZ"), 5);
}

// fix #2231: F30 (well above max) also raises FormatException (the previous
// off-by-one would have rejected 30+ but not 29; F30 is the regression guard
// for the upper-bound logic staying strict).
BEZEL_TEST(decimal_format_1412, F30_above_max_raises_FormatException_spec_2_18_9_3) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  var result := 0;
  var d := 1.5m;
  try
    var s := d.ToString("F30");
    result := 99;
  catch FormatException => result := 6;
  end;
  return result;
end Main; end C;
)BZ"), 6);
}

// fix #2231: same off-by-one applied to E29 / E30 — rejected.
BEZEL_TEST(decimal_format_1412, E29_above_max_raises_FormatException_spec_2_18_9_3) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  var result := 0;
  var d := 1.5m;
  try
    var s := d.ToString("E29");
    result := 99;
  catch FormatException => result := 7;
  end;
  return result;
end Main; end C;
)BZ"), 7);
}

// fix #2231: P29 / P30 — rejected (the P formatter scales the digit-string
// representation, but the same scale bound applies).
BEZEL_TEST(decimal_format_1412, P29_above_max_raises_FormatException_spec_2_18_9_3) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  var result := 0;
  var d := 1.5m;
  try
    var s := d.ToString("P29");
    result := 99;
  catch FormatException => result := 8;
  end;
  return result;
end Main; end C;
)BZ"), 8);
}
