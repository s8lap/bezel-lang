// Regression tests for #2253 — `decimal.ToString("Pn")` and `("En")` must round
// midpoints HALF-TO-EVEN (banker's rounding), exactly like `("Fn")` and
// `Decimal.Round` do.
//
// stdlib §2.18 (v0.230, `docs/Bezel_Standard_Library.md` line 1173, the
// "**`decimal` midpoint rule.**" paragraph):
//
//   "For a `decimal` operand, digit reduction under `F`/`Fn`, `P`/`Pn`, and
//    `E`/`En` rounds a midpoint **half-to-even** (banker's rounding), agreeing
//    with `Decimal.Round` (§9.3): `0.125m` under `F2` renders `"0.12"`, and
//    `P`/`E` apply the same rule to their scaled digit strings — the format
//    specifiers and `Round` are two spellings of one rounding operation and
//    never disagree at a midpoint."
//
// Before the fix the runtime's P and E branches rounded midpoints HALF-UP, so
// the three spellings of one rounding disagreed with each other:
//
//   $"{0.125m:F2}"  →  "0.12"     (F — already half-to-even)
//   $"{0.00125m:P2}" → "0.13%"    (P — was half-up)
//   $"{0.125m:E1}"  →  "1.3E-001" (E — was half-up)
//
// The P branch scales by 100 (§2.18: "percent (×100, `%` suffix)") and the E
// branch rounds the significand; both now run the SAME half-to-even rounding
// the F branch runs.
//
// The rows below pin, per specifier: an exact midpoint whose kept digit is
// EVEN (rounds down), an exact midpoint whose kept digit is ODD (rounds up),
// the same two under a negative sign, a non-tie control (the dropped digit
// above / below 5), a value that is past the midpoint rather than AT it
// (dropped digit 5 with a non-zero tail — not a tie), and F unchanged.

#include "support/aot_support.hpp"   // runAot — AOT test harness (strict oracle)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ── P/Pn at an exact midpoint ────────────────────────────────────────────────

// §2.18 midpoint rule: P rounds a midpoint half-to-even. 0.00125m scaled by
// 100 is 0.125; the kept digit is 2 (EVEN) → "0.12%".
// (Half-up printed "0.13%" — the defect #2253 reports.)
BEZEL_TEST(decimal_format_pe_2253, P_midpoint_even_down_spec_2_18) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  return (if $"{0.00125m:P2}" = "0.12%" then 1 else 0);
end Main; end C;
)BZ"), 1);
}

// §2.18 midpoint rule: P rounds a midpoint half-to-even. 0.00135m scaled by
// 100 is 0.135; the kept digit is 3 (ODD) → "0.14%".
// (Half-up and half-to-even agree here — this row pins the odd-up half of
// the rule, so a "round every 5 down" implementation cannot pass it.)
BEZEL_TEST(decimal_format_pe_2253, P_midpoint_odd_up_spec_2_18) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  return (if $"{0.00135m:P2}" = "0.14%" then 1 else 0);
end Main; end C;
)BZ"), 1);
}

// §2.18 midpoint rule at a coarser precision: 0.125m scaled by 100 is 12.5;
// at P0 the kept digit is 2 (EVEN) → "12%", and 0.135m → 13.5 keeps 3 (ODD)
// → "14%".
BEZEL_TEST(decimal_format_pe_2253, P_midpoint_precision_0_spec_2_18) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  var ok := 0;
  if $"{0.125m:P0}" = "12%" then ok := ok + 1; end;
  if $"{0.135m:P0}" = "14%" then ok := ok + 2; end;
  return ok;
end Main; end C;
)BZ"), 3);
}

// §2.18 midpoint rule: a negative value rounds on its magnitude —
// -0.00125m (scaled: -0.125) keeps the even digit 2 → "-0.12%";
// -0.00135m (scaled: -0.135) keeps the odd digit 3 → "-0.14%".
BEZEL_TEST(decimal_format_pe_2253, P_midpoint_negative_spec_2_18) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  var ok := 0;
  if $"{(-0.00125m):P2}" = "-0.12%" then ok := ok + 1; end;
  if $"{(-0.00135m):P2}" = "-0.14%" then ok := ok + 2; end;
  return ok;
end Main; end C;
)BZ"), 3);
}
// §2.18 midpoint rule: a value PAST the midpoint is not a tie. 0.001251m
// scales to 0.1251 — the dropped digit is 5 but the tail is non-zero, so the
// value is strictly above the midpoint → "0.13%" (not the even "0.12%").
BEZEL_TEST(decimal_format_pe_2253, P_past_midpoint_not_a_tie_spec_2_18) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  return (if $"{0.001251m:P2}" = "0.13%" then 1 else 0);
end Main; end C;
)BZ"), 1);
}

// ── E/En at an exact midpoint ────────────────────────────────────────────────

// §2.18 midpoint rule: E rounds a midpoint half-to-even. 0.125m under E1
// keeps the significand digits "12" (the kept digit 2 is EVEN) → "1.2E-001".
// (Half-up printed "1.3E-001" — the defect #2253 reports.)
BEZEL_TEST(decimal_format_pe_2253, E_midpoint_even_down_spec_2_18) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  return (if $"{0.125m:E1}" = "1.2E-001" then 1 else 0);
end Main; end C;
)BZ"), 1);
}

// §2.18 midpoint rule: E rounds a midpoint half-to-even. 0.135m under E1
// keeps the significand digits "13" (the kept digit 3 is ODD) → "1.4E-001".
BEZEL_TEST(decimal_format_pe_2253, E_midpoint_odd_up_spec_2_18) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  return (if $"{0.135m:E1}" = "1.4E-001" then 1 else 0);
end Main; end C;
)BZ"), 1);
}

// §2.18 midpoint rule: a negative value rounds on its magnitude —
// -0.125m under E1 → "-1.2E-001", -0.135m under E1 → "-1.4E-001".
BEZEL_TEST(decimal_format_pe_2253, E_midpoint_negative_spec_2_18) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  var ok := 0;
  if $"{(-0.125m):E1}" = "-1.2E-001" then ok := ok + 1; end;
  if $"{(-0.135m):E1}" = "-1.4E-001" then ok := ok + 2; end;
  return ok;
end Main; end C;
)BZ"), 3);
}

// §2.18 midpoint rule at a coarser precision: 12.5m under E1 keeps the
// significand digits "12" (even) → "1.2E+001"; 13.5m keeps "13" (odd) →
// "1.4E+001".
BEZEL_TEST(decimal_format_pe_2253, E_midpoint_larger_magnitude_spec_2_18) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  var ok := 0;
  if $"{12.5m:E1}" = "1.2E+001" then ok := ok + 1; end;
  if $"{13.5m:E1}" = "1.4E+001" then ok := ok + 2; end;
  return ok;
end Main; end C;
)BZ"), 3);
}

// ── Non-tie controls (the rounding position did not move) ────────────────────

// §2.18: a dropped digit above 5 rounds up, below 5 rounds down — for P as
// well. (0.00126m scales to 0.126; 0.00124m scales to 0.124.)
BEZEL_TEST(decimal_format_pe_2253, P_non_tie_control_spec_2_18) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  var ok := 0;
  if $"{0.00126m:P2}" = "0.13%" then ok := ok + 1; end;
  if $"{0.00124m:P2}" = "0.12%" then ok := ok + 2; end;
  return ok;
end Main; end C;
)BZ"), 3);
}

// §2.18: a dropped digit above 5 rounds up, below 5 rounds down — for E as
// well.
BEZEL_TEST(decimal_format_pe_2253, E_non_tie_control_spec_2_18) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  var ok := 0;
  if $"{0.126m:E1}" = "1.3E-001" then ok := ok + 1; end;
  if $"{0.124m:E1}" = "1.2E-001" then ok := ok + 2; end;
  return ok;
end Main; end C;
)BZ"), 3);
}

// ── F is unchanged (the control for the branch the fix did not touch) ────────

// §2.18 midpoint rule: F already rounded half-to-even (via Decimal.Round's
// rule) — the fix must not move it. 0.125m → "0.12" (even), 0.135m → "0.14"
// (odd), and the non-ties 0.126m → "0.13", 0.124m → "0.12".
BEZEL_TEST(decimal_format_pe_2253, F_unchanged_control_spec_2_18) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  var ok := 0;
  if $"{0.125m:F2}" = "0.12" then ok := ok + 1; end;
  if $"{0.135m:F2}" = "0.14" then ok := ok + 2; end;
  if $"{0.126m:F2}" = "0.13" then ok := ok + 4; end;
  if $"{0.124m:F2}" = "0.12" then ok := ok + 8; end;
  return ok;
end Main; end C;
)BZ"), 15);
}

// ── The three spellings of one rounding agree ───────────────────────────────

// §2.18: "the format specifiers and `Round` are two spellings of one rounding
// operation and never disagree at a midpoint." P is F applied to the value
// ×100, so P of 0.00125m must be F of 0.125m with a "%" suffix — and
// `Decimal.Round` must land on the same decimal.
BEZEL_TEST(decimal_format_pe_2253, P_agrees_with_F_and_Round_spec_2_18) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  var ok := 0;
  var d125 := 0.125m;
  var d135 := 0.135m;
  if $"{0.00125m:P2}" = $"{0.125m:F2}" + "%" then ok := ok + 1; end;
  if $"{0.00135m:P2}" = $"{0.135m:F2}" + "%" then ok := ok + 2; end;
  if d125.Round(2) = 0.12m then ok := ok + 4; end;
  if d135.Round(2) = 0.14m then ok := ok + 8; end;
  return ok;
end Main; end C;
)BZ"), 15);
}
