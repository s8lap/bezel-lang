// Spec-derived tests for #1411 — no-format interpolation of TimeSpan,
// DateOnly, TimeOnly, and Guid must use each type's canonical ToString()
// rendering (stdlib §2.5/§2.6/§2.18), not raw tick/day counts or type names.
//
// The FORMAT path (`$"{x:spec}"`) was fixed in v0.172 (#812/#1339); this file
// covers the companion NO-FORMAT path (`$"{x}"`), which was left unchanged
// and fell through to the LLVM-type integer fallback (rendering raw ticks)
// or the class-name fallback (rendering "Guid").
//
// Per stdlib §2.18:
//   TimeSpan  no-format ≡ "c"          (e.g. "01:01:01")
//   DateOnly  no-format ≡ "yyyy-MM-dd" (ISO 8601 date)
//   TimeOnly  no-format ≡ "HH:mm:ss"   (ISO 8601 time-of-day)
//   Guid      no-format ≡ "D"          (8-4-4-4-12 with hyphens)
//
// Fix sites: LlvmBackend_String.cpp (emitValueToString) and
//            LlvmBackend_Exprs.cpp (interpolation no-format path).

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

// ── TimeSpan no-format → "c" (§2.5/§2.18) ─────────────────────────────────

// §2.18: TimeSpan no-format ToString() ≡ "c" — 3661 seconds = "01:01:01".
// Before fix #1411: `$"{ts}"` rendered raw ticks ("36610000000").
BEZEL_TEST(noformat_interp_1411, timespan_noformat_is_c_spec_2_18) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  var ts := TimeSpan.FromSeconds(3661.0);
  return 1 if $"{ts}" = "01:01:01" else 0;
end Main; end C;
)BZ"), 1);
}

// TimeSpan zero duration no-format: zero span → "00:00:00".
BEZEL_TEST(noformat_interp_1411, timespan_zero_noformat_spec_2_18) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  var ts := TimeSpan.FromSeconds(0.0);
  return 1 if $"{ts}" = "00:00:00" else 0;
end Main; end C;
)BZ"), 1);
}

// No-format and explicit "c" spec must produce the same string (§2.18).
BEZEL_TEST(noformat_interp_1411, timespan_noformat_equals_explicit_c_spec_2_18) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  var ts := TimeSpan.FromSeconds(90.0);
  return 1 if $"{ts}" = $"{ts:c}" else 0;
end Main; end C;
)BZ"), 1);
}

// ── DateOnly no-format → ISO 8601 "yyyy-MM-dd" (§2.5/§2.18) ───────────────

// §2.18: DateOnly no-format → ISO 8601 date.
// Before fix #1411: `$"{d}"` rendered raw day-number-from-epoch.
BEZEL_TEST(noformat_interp_1411, dateonly_noformat_is_iso8601_spec_2_18) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  var d := new DateOnly(2024, 3, 15);
  return 1 if $"{d}" = "2024-03-15" else 0;
end Main; end C;
)BZ"), 1);
}

// No-format and explicit "yyyy-MM-dd" spec must agree.
BEZEL_TEST(noformat_interp_1411, dateonly_noformat_equals_explicit_fmt_spec_2_18) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  var d := new DateOnly(2000, 1, 1);
  return 1 if $"{d}" = $"{d:yyyy-MM-dd}" else 0;
end Main; end C;
)BZ"), 1);
}

// ── TimeOnly no-format → "HH:mm:ss" (§2.5/§2.18) ──────────────────────────

// §2.18: TimeOnly no-format → "HH:mm:ss" ISO 8601 time-of-day.
// Before fix #1411: `$"{t}"` rendered raw ms-from-midnight.
BEZEL_TEST(noformat_interp_1411, timeonly_noformat_is_hhmmss_spec_2_18) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  var t := new TimeOnly(13, 5, 9);
  return 1 if $"{t}" = "13:05:09" else 0;
end Main; end C;
)BZ"), 1);
}

// No-format and explicit "HH:mm:ss" spec must agree.
BEZEL_TEST(noformat_interp_1411, timeonly_noformat_equals_explicit_fmt_spec_2_18) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  var t := new TimeOnly(0, 0, 0);
  return 1 if $"{t}" = $"{t:HH:mm:ss}" else 0;
end Main; end C;
)BZ"), 1);
}

// ── Guid no-format → "D" (§2.6/§2.18) ─────────────────────────────────────

// §2.18: Guid no-format ≡ "D" (8-4-4-4-12 hyphenated hex).
// Before fix #1411: `$"{g}"` rendered the literal type name "Guid".
BEZEL_TEST(noformat_interp_1411, guid_noformat_is_D_spec_2_18) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  var g := Guid.Empty;
  return 1 if $"{g}" = "00000000-0000-0000-0000-000000000000" else 0;
end Main; end C;
)BZ"), 1);
}

// No-format and explicit "D" spec must produce the same string (§2.18).
BEZEL_TEST(noformat_interp_1411, guid_noformat_equals_explicit_D_spec_2_18) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  var g := Guid.Empty;
  return 1 if $"{g}" = $"{g:D}" else 0;
end Main; end C;
)BZ"), 1);
}
