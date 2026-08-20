// Regression tests for #3292: §2.18 (v0.230) — a malformed run of a temporal
// specifier letter in a DateTime / DateOnly / TimeOnly format pattern must
// FAULT as FormatException (§2.18 strict-fault), not pass through as literal
// text. Per stdlib §2.18's "Temporal patterns: non-specifier characters are
// literals" paragraph, the ONLY well-formed specifier runs in the Bezel
// vocabulary are:
//   yyyy  → 4-digit year
//   MM    → 2-digit month
//   dd    → 2-digit day
//   HH    → 2-digit hour 24h
//   mm    → 2-digit minute
//   ss    → 2-digit second
//   fff   → 3-digit millisecond
// Every other character is a LITERAL if it is not a specifier letter, and a
// MALFORMED SPECIFIER RUN if it is one of `y M d H m s f` — the run length
// must match exactly. (The prices the rule charges: `h` is not a specifier
// letter, so `hh` is a literal while `HH` is the hour; `hours` and `seconds`
// are unwritable.) This is compiler catch-up to v0.230 — the prior runtime
// passed malformed runs through as text.
//
// Known cases per the issue: `yyy` (3 y's, year expected to be 4), `MMM`
// (3 M's, month expected to be 2), and a lone `s` (1 s, second expected to
// be 2). The tests below cover the three issue-named shapes plus well-formed
// controls for each affected specifier letter to prove the fix does not
// over-reject.
//
// runAot returns the exit-code int from Main() (8-bit wrapped); values ≤ 255.

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

// ── WELL-FORMED CONTROLS (§2.18 v0.230 vocabulary, per-table patterns) ───────
// Every well-formed specifier run still renders correctly — the fix must not
// over-reject. Covers each of the seven valid specifier runs.
BEZEL_TEST(datetime_format_malformed_run_3292, well_formed_full_datetime) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var d := new DateTime(2024, 3, 15, 13, 5, 9); "
        "  var s := d.ToString(\"yyyy-MM-dd HH:mm:ss.fff\"); "
        "  return (if s = \"2024-03-15 13:05:09.000\" then 1 else 0); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(datetime_format_malformed_run_3292, well_formed_each_specifier) {
    // yyyy, MM, dd, HH, mm, ss, fff each render correctly in isolation.
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  var d := new DateTime(2024, 3, 15, 13, 5, 9, 123);
  var ok := 0;
  if d.ToString("yyyy") = "2024" then ok := ok + 1; end;
  if d.ToString("MM")   = "03"   then ok := ok + 2; end;
  if d.ToString("dd")   = "15"   then ok := ok + 4; end;
  if d.ToString("HH")   = "13"   then ok := ok + 8; end;
  if d.ToString("mm")   = "05"   then ok := ok + 16; end;
  if d.ToString("ss")   = "09"   then ok := ok + 32; end;
  if d.ToString("fff")  = "123"  then ok := ok + 64; end;
  return ok;
end Main; end C;
)BZ"), 127);
}

// ── LITERAL CONTROLS (non-specifier chars pass through verbatim) ──────────────
// §2.18 v0.230: non-specifier characters are LITERAL, never faulted. `h` is
// not a specifier letter, so `hh` is the literal text "hh" — the price the
// rule charges for `HH` to mean the hour. (We deliberately avoid `hh:mm:ss`
// as the test pattern because `mm` and `ss` ARE specifier letters; their
// presence in the pattern would mix literal and spec behavior in a way that
// doesn't isolate the `h` literal-copy rule.)
BEZEL_TEST(datetime_format_malformed_run_3292, hh_literal_not_hour) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  var d := new DateTime(2024, 3, 15, 13, 5, 9);
  var s := d.ToString("hh");
  return (if s = "hh" then 1 else 0);
end Main; end C;
)BZ"), 1);
}

BEZEL_TEST(datetime_format_malformed_run_3292, hh_literal_with_known_specifiers) {
    // A mixed pattern: `h` is literal, `HH` is the hour. The fix must NOT
    // confuse `hh` followed by `HH` for a 4-H run (each `h` is a literal
    // character — only `H` is a specifier letter).
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  var d := new DateTime(2024, 3, 15, 13, 5, 9);
  var s := d.ToString("hh HH");
  return (if s = "hh 13" then 1 else 0);
end Main; end C;
)BZ"), 1);
}

BEZEL_TEST(datetime_format_malformed_run_3292, t_literal_in_pattern) {
    // `T` and `Z` are explicit examples in §2.18 of literal characters.
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  var d := new DateTime(2024, 3, 15, 13, 5, 9);
  var s := d.ToString("yyyy-MM-ddTHH:mm:ssZ");
  return (if s = "2024-03-15T13:05:09Z" then 1 else 0);
end Main; end C;
)BZ"), 1);
}

// ── MALFORMED RUNS (must FAULT as FormatException, per #3292) ────────────────
// Each of the three issue-named shapes raises a catchable FormatException
// instead of silently rendering the letters as text.

// `yyy` — 3 y's, year requires 4.
BEZEL_TEST(datetime_format_malformed_run_3292, malformed_yyy_faults) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  var result := 0;
  var d := new DateTime(2024, 3, 15);
  try
    var s := d.ToString("yyy");
    result := 99;        // BUG pre-fix: passed through as literal "yyy"
  catch FormatException => result := 7;
  end;
  return result;
end Main; end C;
)BZ"), 7);
}

// `MMM` — 3 M's, month requires 2. Pre-fix the runtime matched `MM` (valid)
// and emitted the lone `M` as a literal, producing the value "03M" — silently
// wrong, exactly the silent-pass-through the v0.230 rule was written to stop.
BEZEL_TEST(datetime_format_malformed_run_3292, malformed_MMM_faults) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  var result := 0;
  var d := new DateTime(2024, 3, 15);
  try
    var s := d.ToString("MMM");
    result := 99;        // BUG pre-fix: matched "MM", then "M" as literal
  catch FormatException => result := 11;
  end;
  return result;
end Main; end C;
)BZ"), 11);
}

// Lone `s` — 1 s, second requires 2.
BEZEL_TEST(datetime_format_malformed_run_3292, malformed_lone_s_faults) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  var result := 0;
  var d := new DateTime(2024, 3, 15, 13, 5, 9);
  try
    var s := d.ToString("s");
    result := 99;        // BUG pre-fix: emitted as literal "s"
  catch FormatException => result := 13;
  end;
  return result;
end Main; end C;
)BZ"), 13);
}

// ── EXHAUSTIVE MALFORMED CASES (one per specifier letter, all lengths ≠ spec) ─
// Covers every specifier letter with a length OTHER than the documented one
// (and the `y`/`f` families, where only 4 / 3 respectively are valid).
// `yyyyy` is 5 y's — not 4, so malformed. `arm` increments in each catch
// arm, so a successful malformed-run fault across all 18 patterns = 18.
BEZEL_TEST(datetime_format_malformed_run_3292, all_specifier_letters_malformed_lengths_fault) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  var d := new DateTime(2024, 3, 15, 13, 5, 9, 123);
  var arm := 0;
  try
    var s := d.ToString("y");
    arm := arm;
  catch FormatException => arm := arm + 1;
  end;
  try
    var s := d.ToString("M");
    arm := arm;
  catch FormatException => arm := arm + 1;
  end;
  try
    var s := d.ToString("d");
    arm := arm;
  catch FormatException => arm := arm + 1;
  end;
  try
    var s := d.ToString("H");
    arm := arm;
  catch FormatException => arm := arm + 1;
  end;
  try
    var s := d.ToString("m");
    arm := arm;
  catch FormatException => arm := arm + 1;
  end;
  try
    var s := d.ToString("s");
    arm := arm;
  catch FormatException => arm := arm + 1;
  end;
  try
    var s := d.ToString("f");
    arm := arm;
  catch FormatException => arm := arm + 1;
  end;
  try
    var s := d.ToString("yy");
    arm := arm;
  catch FormatException => arm := arm + 1;
  end;
  try
    var s := d.ToString("yyy");
    arm := arm;
  catch FormatException => arm := arm + 1;
  end;
  try
    var s := d.ToString("yyyyy");
    arm := arm;
  catch FormatException => arm := arm + 1;
  end;
  try
    var s := d.ToString("ff");
    arm := arm;
  catch FormatException => arm := arm + 1;
  end;
  try
    var s := d.ToString("ffff");
    arm := arm;
  catch FormatException => arm := arm + 1;
  end;
  try
    var s := d.ToString("MMM");
    arm := arm;
  catch FormatException => arm := arm + 1;
  end;
  try
    var s := d.ToString("MMMM");
    arm := arm;
  catch FormatException => arm := arm + 1;
  end;
  try
    var s := d.ToString("ddd");
    arm := arm;
  catch FormatException => arm := arm + 1;
  end;
  try
    var s := d.ToString("HHH");
    arm := arm;
  catch FormatException => arm := arm + 1;
  end;
  try
    var s := d.ToString("mmm");
    arm := arm;
  catch FormatException => arm := arm + 1;
  end;
  try
    var s := d.ToString("sss");
    arm := arm;
  catch FormatException => arm := arm + 1;
  end;
  return arm;
end Main; end C;
)BZ"), 18);
}

// ── SAME FAULTS THROUGH STRING INTERPOLATION FORMAT SPEC (§18.5 hole) ────────
// The interpolation format spec dispatches to the SAME
// bz_datetime_to_string_fmt; the fix must apply there too.
BEZEL_TEST(datetime_format_malformed_run_3292, interpolation_malformed_yyy_faults) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  var result := 0;
  var d := new DateTime(2024, 3, 15);
  try
    var s := $"{d:yyy}";
    result := 99;
  catch FormatException => result := 17;
  end;
  return result;
end Main; end C;
)BZ"), 17);
}

BEZEL_TEST(datetime_format_malformed_run_3292, interpolation_malformed_MMM_faults) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  var result := 0;
  var d := new DateTime(2024, 3, 15);
  try
    var s := $"{d:MMM}";
    result := 99;
  catch FormatException => result := 19;
  end;
  return result;
end Main; end C;
)BZ"), 19);
}

BEZEL_TEST(datetime_format_malformed_run_3292, interpolation_malformed_lone_s_faults) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  var result := 0;
  var d := new DateTime(2024, 3, 15, 13, 5, 9);
  try
    var s := $"{d:s}";
    result := 99;
  catch FormatException => result := 23;
  end;
  return result;
end Main; end C;
)BZ"), 23);
}

// ── INTERPOLATION WELL-FORMED CONTROL (regression guard) ─────────────────────
// A well-formed interpolation format must still produce the right string.
BEZEL_TEST(datetime_format_malformed_run_3292, interpolation_well_formed_unchanged) {
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  var d := new DateTime(2024, 3, 15, 13, 5, 9);
  var s := $"{d:yyyy-MM-dd HH:mm:ss.fff}";
  return (if s = "2024-03-15 13:05:09.000" then 1 else 0);
end Main; end C;
)BZ"), 1);
}

// ── DATEONLY / TIMEONLY COVERAGE (§2.18 v0.230) ──────────────────────────────
// DateOnly / TimeOnly share the same bz_datetime_to_string_fmt runtime path
// after day-number / ms-of-day are widened to ticks (via the interpolation
// format spec dispatcher in LlvmBackend_String.cpp). The fix must apply
// identically. (DateOnly/TimeOnly do not have a `ToString(format)` member
// declared in stdlib §2.5 — only `ToString()` no-arg — so we exercise the
// runtime through the §18.5 string-interpolation format spec, which is the
// same dispatcher.)
BEZEL_TEST(datetime_format_malformed_run_3292, dateonly_interpolation_malformed_M_faults) {
    // DateOnly's valid month specifier is "MM" (length 2). "M" (length 1) is
    // a malformed run on DateOnly exactly as on DateTime.
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  var result := 0;
  var d := new DateOnly(2024, 3, 15);
  try
    var s := $"{d:M}";
    result := 99;
  catch FormatException => result := 29;
  end;
  return result;
end Main; end C;
)BZ"), 29);
}

BEZEL_TEST(datetime_format_malformed_run_3292, timeonly_interpolation_malformed_H_faults) {
    // TimeOnly's valid hour specifier is "HH" (length 2). "H" is malformed.
    BEZEL_EXPECT_EQ(run(R"BZ(
module M; class C public static method Main(): int
  var result := 0;
  var t := new TimeOnly(13, 5, 9);
  try
    var s := $"{t:H}";
    result := 99;
  catch FormatException => result := 31;
  end;
  return result;
end Main; end C;
)BZ"), 31);
}