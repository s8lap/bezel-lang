// fix #5079 (stdlib §2.18, v0.307): the double/float round-trip guarantee
// ("R" specifier and no-format ToString()/interpolation both produce the
// shortest string that reparses to the identical value) was violated for
// round magnitudes >= 1 needing few significant digits: 200.0 rendered as
// "2e+02" instead of "200". Root cause: the search for the minimal
// round-tripping *precision* was done with "%.*g", which inherits C's own
// fixed/scientific notation switch (scientific iff exponent >= precision);
// once the digit count is minimized (200.0 needs only 1 significant digit,
// exponent 2), that rule forces scientific even though the fixed rendering
// of the SAME minimal digit string is shorter. The fix (bz_render_shortest
// in src/runtime/bezel_runtime_time.cpp) instead renders both the fixed and
// scientific forms of the minimal digit string and picks whichever is not
// longer, ties favoring fixed — for exponent >= 0 only.
//
// The negative-exponent side is deliberately left on the original threshold
// (scientific iff exponent < -4, per C99 "%g" — independent of precision, so
// it was never subject to the bug above) rather than a length comparison:
// 0.0001 (exponent -4) is "0.0001" (6 chars) vs "1e-04" (5 chars) — literally
// shorter as scientific, but the issue's own worked example states the fixed
// form is correct. This file pins that boundary explicitly so a future
// "generalize the length comparison to all exponents" change cannot silently
// regress it.

#include "support/aot_support.hpp"   // runAot — AOT test harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ── Positive exponent: fixed notation wins when it is not longer (the bug) ──
BEZEL_TEST(double_string_shortest_5079, positive_exponent_prefers_fixed_when_shorter) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  var ok := 0;
  var a: double := 200.0;
  if $"{a}" = "200" then ok := ok + 1; end;
  var b: double := 1000.0;
  if $"{b}" = "1000" then ok := ok + 2; end;
  var c: double := 10.0;
  if $"{c}" = "10" then ok := ok + 4; end;
  var d: double := -200.0;
  if $"{d}" = "-200" then ok := ok + 8; end;
  var e: double := 10000.0;
  if $"{e}" = "10000" then ok := ok + 16; end;   // tie (5 vs 5 chars): fixed wins
  return ok;
end Main; end C;
)BZ"), 31);
}

// ── Positive exponent: scientific notation still wins when it is strictly
// shorter (this must NOT regress: an existing test elsewhere already pins
// 5000000000.0 -> "5e+09") ──────────────────────────────────────────────────
BEZEL_TEST(double_string_shortest_5079, positive_exponent_keeps_scientific_when_shorter) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  var ok := 0;
  var a: double := 1e21;
  if $"{a}" = "1e+21" then ok := ok + 1; end;
  var b: double := 100000.0;
  if $"{b}" = "1e+05" then ok := ok + 2; end;   // fixed "100000" (6) > sci "1e+05" (5)
  var c: double := 1e300;
  if $"{c}" = "1e+300" then ok := ok + 4; end;
  return ok;
end Main; end C;
)BZ"), 7);
}

// ── Negative exponent: the original exponent >= -4 threshold is unchanged —
// NOT a length comparison. This is the regression the fix must not introduce. ─
BEZEL_TEST(double_string_shortest_5079, negative_exponent_threshold_unchanged) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  var ok := 0;
  var a: double := 0.1;
  if $"{a}" = "0.1" then ok := ok + 1; end;
  var b: double := 0.001;
  if $"{b}" = "0.001" then ok := ok + 2; end;
  // Exponent -4 boundary: fixed ("0.0001", 6 chars) is LONGER than scientific
  // ("1e-04", 5 chars) but must still be chosen — the threshold, not length,
  // governs this side.
  var c: double := 0.0001;
  if $"{c}" = "0.0001" then ok := ok + 4; end;
  var d: double := -0.0001;
  if $"{d}" = "-0.0001" then ok := ok + 8; end;
  // Exponent -5: past the threshold, scientific.
  var e: double := 0.00001;
  if $"{e}" = "1e-05" then ok := ok + 16; end;
  var f: double := 1e-7;
  if $"{f}" = "1e-07" then ok := ok + 32; end;
  return ok;
end Main; end C;
)BZ"), 63);
}

// ── Values already exercised by format_revision_812_1339_test.cpp's
// double_round_trip_shortest must remain unaffected (many significant digits;
// the notation choice was already correct pre-fix on this side). ────────────
BEZEL_TEST(double_string_shortest_5079, many_significant_digits_unaffected) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  var ok := 0;
  if $"{0.1 + 0.2}" = "0.30000000000000004" then ok := ok + 1; end;
  if $"{1.0 / 3.0}" = "0.3333333333333333" then ok := ok + 2; end;
  return ok;
end Main; end C;
)BZ"), 3);
}

// ── Zero, negative zero, NaN, and the infinities are unaffected (all bypass
// the shortest-string search: NaN/Infinity are special-cased; +/-0.0 pick the
// fixed candidate "0"/"-0" as strictly shorter than any scientific form).
//
// The three NON-FINITE spellings are the §2.18 ones as of #7301 (stdlib
// §2.18, v0.344): "NaN", "Infinity" and "-Infinity" — the names of the §9.2
// constants. This row pinned C's "%g" spellings ("nan", "inf", "-inf")
// before #7301; the flip is the DOCUMENT's spelling, and the zero /
// negative-zero arms are unchanged. ─────────────────────────────────────────
BEZEL_TEST(double_string_shortest_5079, zero_nan_infinity_unaffected) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  var ok := 0;
  var z: double := 0.0;
  if $"{z}" = "0" then ok := ok + 1; end;
  var nz: double := -0.0;
  if $"{nz}" = "-0" then ok := ok + 2; end;
  var n: double := double.NaN;
  if $"{n}" = "NaN" then ok := ok + 4; end;
  var pi: double := double.PositiveInfinity;
  if $"{pi}" = "Infinity" then ok := ok + 8; end;
  var ni: double := double.NegativeInfinity;
  if $"{ni}" = "-Infinity" then ok := ok + 16; end;
  return ok;
end Main; end C;
)BZ"), 31);
}

// ── No-format ToString()/interpolation and the "R" specifier must still
// agree on a value the fix newly changes (200.0), proving both paths share
// bz_double_to_string_r. ─────────────────────────────────────────────────────
BEZEL_TEST(double_string_shortest_5079, noformat_equals_R_on_fixed_case) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  var x: double := 200.0;
  return (if ($"{x}" = "200") and ($"{x:R}" = "200") and ($"{x}" = $"{x:R}") then 3 else 0);
end Main; end C;
)BZ"), 3);
}

// ── float (32-bit) shares the same path (bz_float_to_string_r) and must show
// the identical fix. ─────────────────────────────────────────────────────────
BEZEL_TEST(double_string_shortest_5079, float_positive_exponent_prefers_fixed) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  var ok := 0;
  var a: float := 200.0f;
  if $"{a}" = "200" then ok := ok + 1; end;
  var b: float := 0.1f;
  if $"{b}" = "0.1" then ok := ok + 2; end;
  return ok;
end Main; end C;
)BZ"), 3);
}
