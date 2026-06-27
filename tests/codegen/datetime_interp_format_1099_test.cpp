// Regression tests for #1099 part A (§18.5 / §2.5): a DateTime value with a
// format spec in string interpolation (`$"{dt:yyyy-MM-dd}"`) ignored the format
// and rendered the no-arg ToString ("2024-03-15T00:00:00"). emitWithFormatSpec
// only handled single-letter numeric codes (F/E/G/P/X/D) and returned null for a
// DateTime format, so the interpolation fell through to emitValueToString. The
// fix dispatches a DateTime format spec to the runtime DateTime formatter
// (bz_datetime_to_string_fmt — the same one DateTime.ToString(format) uses).
// Found by adversarial discovery sweep #7. (The decimal.ToString(format) half of
// #1099 needs a runtime change and is tracked separately.)
//
// runJit returns the FULL int from Main() (not 8-bit wrapped); values ≤ 255.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// #1099A: `$"{dt:yyyy-MM-dd}"` applies the format (was the full "…T00:00:00").
BEZEL_TEST(datetime_interp_format_1099, ymd_format_applied) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var d := new DateTime(2024, 3, 15); "
        "  var s := $\"{d:yyyy-MM-dd}\"; "
        "  var ok := 0; "
        "  if s.Length = 10 then ok := ok + 1; end; "
        "  if s = \"2024-03-15\" then ok := ok + 2; end; "
        "  return ok; "                                    // 1 + 2 = 3
        "end Main; end C;"), 3);
}

// The interpolation format agrees with an explicit DateTime.ToString(format).
BEZEL_TEST(datetime_interp_format_1099, agrees_with_explicit_tostring) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var d := new DateTime(2024, 3, 15); "
        "  var a := $\"{d:yyyy-MM-dd}\"; "
        "  var b := d.ToString(\"yyyy-MM-dd\"); "
        "  return 1 if a = b else 0; "                     // 1
        "end Main; end C;"), 1);
}

// No-format DateTime interpolation (`$"{d}"`) is unchanged (the full form).
BEZEL_TEST(datetime_interp_format_1099, no_format_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var d := new DateTime(2024, 3, 15); "
        "  var s := $\"{d}\"; "
        "  return 1 if s.Length > 10 else 0; "             // full form > 10 chars
        "end Main; end C;"), 1);
}

// Regression guard: numeric format specs (`$"{x:X}"`, `$"{p:F2}"`) still apply.
BEZEL_TEST(datetime_interp_format_1099, numeric_format_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var x := 255; var pi := 3.14159; "
        "  var a := $\"{x:X}\"; var b := $\"{pi:F2}\"; "
        "  var ok := 0; "
        "  if a = \"FF\" then ok := ok + 1; end; "
        "  if b = \"3.14\" then ok := ok + 2; end; "
        "  return ok; "                                    // 1 + 2 = 3
        "end Main; end C;"), 3);
}

// #1099A (residual): a DateTime format spec containing `:` (HH:mm:ss) was
// mis-parsed — the interpolation format-spec lexer rejected the colon (and `/`,
// `.`, space), so `$"{d:HH:mm:ss}"` left the whole `d:HH:mm:ss` as the
// expression and the format was discarded. Bezel has no ternary `:`, so a
// depth-0 `:` is always the format separator; the lexer now admits the date/time
// separators.
BEZEL_TEST(datetime_interp_format_1099, time_format_with_colons) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var d := new DateTime(2024, 3, 15); "
        "  var s := $\"{d:HH:mm:ss}\"; "
        "  return (3 if s.Length = 8 and s = \"00:00:00\" else 0); "
        "end Main; end C;"), 3);
}

BEZEL_TEST(datetime_interp_format_1099, combined_date_time_format) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var d := new DateTime(2024, 3, 15); "
        "  var s := $\"{d:yyyy-MM-dd HH:mm:ss}\"; "
        "  return (3 if s.Length = 19 and s = \"2024-03-15 00:00:00\" else 0); "
        "end Main; end C;"), 3);
}

BEZEL_TEST(datetime_interp_format_1099, slash_date_format) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var d := new DateTime(2024, 3, 15); "
        "  var s := $\"{d:yyyy/MM/dd}\"; "
        "  return (1 if s = \"2024/03/15\" else 0); "
        "end Main; end C;"), 1);
}

// #1099B in interpolation: a decimal fixed-point format in an interpolation hole
// (`$"{d:F2}"`) now uses the same runtime decimal formatter as the explicit
// `d.ToString("F2")` — was the no-format ToString, dropping the precision.
BEZEL_TEST(datetime_interp_format_1099, decimal_fixed_format_in_interpolation) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var x: decimal := 1.5m; "
        "  var s := $\"{x:F2}\"; "
        "  return (3 if s.Length = 4 and s = \"1.50\" else 0); "
        "end Main; end C;"), 3);
}

// ── #1123: `:X` / `:D` integer format must not truncate 64-bit values nor ──────
// sign-extend sub-32-bit unsigned values. Before the fix the value was widened
// to i64 but formatted with a 32-bit conversion (`%X`/`%d`), so snprintf read
// only the low 32 bits; and a sub-32-bit unsigned value was sign-extended.

// A 64-bit value whose low 32 bits are zero printed "0"; must be the full hex.
BEZEL_TEST(datetime_interp_format_1099, hex_64bit_no_truncation_1123) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var v: long := 4294967296L; "
        "  return (1 if $\"{v:X}\" = \"100000000\" else 0); "
        "end Main; end C;"), 1);
}

// ulong max: all 16 hex digits, no truncation to FFFFFFFF.
BEZEL_TEST(datetime_interp_format_1099, hex_ulong_max_full_width_1123) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var u: ulong := 18446744073709551615; "
        "  return (1 if $\"{u:X}\" = \"FFFFFFFFFFFFFFFF\" else 0); "
        "end Main; end C;"), 1);
}

// `:D` of a 64-bit value must print the full decimal, not a wrapped i32.
BEZEL_TEST(datetime_interp_format_1099, decimal_D_64bit_no_truncation_1123) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var d: long := 1000000000000L; "
        "  return (1 if $\"{d:D}\" = \"1000000000000\" else 0); "
        "end Main; end C;"), 1);
}

// A sub-32-bit unsigned value (byte 255) must be zero-extended: "FF", not the
// sign-extended "FFFFFFFF".
BEZEL_TEST(datetime_interp_format_1099, hex_unsigned_byte_no_sext_1123) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var b: byte := 255; "
        "  return (1 if $\"{b:X2}\" = \"FF\" else 0); "
        "end Main; end C;"), 1);
}

// Regression: a signed int's `:X` is its 32-bit two's-complement pattern
// (`-1` → "FFFFFFFF"), and a large unsigned `:D` is not rendered negative.
BEZEL_TEST(datetime_interp_format_1099, signed_int_hex_and_unsigned_decimal_1123) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var n: int := -1; "
        "  var u: ulong := 12345678901234567890; "
        "  var ok := 0; "
        "  if $\"{n:X}\" = \"FFFFFFFF\" then ok := ok + 1; end; "
        "  if $\"{u:D}\" = \"12345678901234567890\" then ok := ok + 2; end; "
        "  return ok; "                                    // 1 + 2 = 3
        "end Main; end C;"), 3);
}
