// Spec-derived regression tests for fix #747 (R42) — two gaps in the primitive
// numeric `Parse` codegen handler.
//
// Authority: stdlib §5.2 (int.Parse / int.Parse(s, radix)) and §6.4
// (decimal.Parse). Both `int` and `decimal` reach the primitive `Parse` handler
// via their lowercase keyword receiver.
//
// Bug 1 (WRONG_VALUE): `int.Parse(s, radix)` — the two-argument form was not
// matched (the handler required exactly 1 arg), so the call was dropped and the
// result read an uninitialized slot (int.Parse("10", 16) returned 0).
//
// Bug 2 (CRASH): `decimal.Parse(s)` via the lowercase keyword reached this
// handler with pt==Decimal, matched none of the integer/float branches, and
// fell through to `return arg` — handing back the string pointer as a decimal
// struct, which crashed codegen with an IntegerType cast assertion.
//
// Fix: add a 2-arg strtol(base=radix) branch, and route the decimal case to
// bz_decimal_parse like `Decimal.Parse`.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §5.2: int.Parse(s, 16) — hexadecimal.
BEZEL_TEST(parse_overloads_747, int_parse_radix_hex) {
    BEZEL_EXPECT_EQ(run(
        "module M; static class F public static method Main(): int "
        "  return int.Parse(\"ff\", 16); "  // 255
        "end Main; end F;"), 255);
}

// §5.2: int.Parse(s, 2) — binary.
BEZEL_TEST(parse_overloads_747, int_parse_radix_binary) {
    BEZEL_EXPECT_EQ(run(
        "module M; static class F public static method Main(): int "
        "  return int.Parse(\"101\", 2); "  // 5
        "end Main; end F;"), 5);
}

// §5.2: the one-argument int.Parse is unaffected.
BEZEL_TEST(parse_overloads_747, int_parse_single_arg_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; static class F public static method Main(): int "
        "  return int.Parse(\"42\"); "  // 42
        "end Main; end F;"), 42);
}

// §6.4: decimal.Parse via the lowercase keyword — was a compiler crash.
BEZEL_TEST(parse_overloads_747, decimal_parse_lowercase_keyword) {
    BEZEL_EXPECT_EQ(run(
        "module M; static class F public static method Main(): int "
        "  var d := decimal.Parse(\"3\"); return int(d); "  // 3
        "end Main; end F;"), 3);
}

// §6.4: the capitalized Decimal.Parse (stdlib class name) still works.
BEZEL_TEST(parse_overloads_747, decimal_parse_capitalized_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; static class F public static method Main(): int "
        "  var d := Decimal.Parse(\"7\"); return int(d); "  // 7
        "end Main; end F;"), 7);
}

// ── #1134 / §5.2: int.Parse / int.TryParse range-check ───────────────────────
// An out-of-range string must raise OverflowException (Parse) / return false
// (TryParse), not silently truncate to the low 32 bits.

// int.Parse of a value beyond int.MaxValue raises OverflowException.
BEZEL_TEST(parse_overloads_747, int_parse_overflow_throws_1134) {
    BEZEL_EXPECT_EQ(run(
        "module M; static class F public static method Main(): int "
        "  try "
        "    var x := int.Parse(\"99999999999\"); return 0; "  // no throw = bug
        "  catch var e: OverflowException => return 1; "       // caught = correct
        "  end; "
        "end Main; end F;"), 1);
}

// A valid in-range value still parses correctly (no spurious throw).
BEZEL_TEST(parse_overloads_747, int_parse_in_range_unaffected_1134) {
    BEZEL_EXPECT_EQ(run(
        "module M; static class F public static method Main(): int "
        "  return int.Parse(\"2147483647\") - 2147483605; "    // MaxValue - 2147483605 = 42
        "end Main; end F;"), 42);
}

// byte.Parse range-check: "300" exceeds byte range → OverflowException.
BEZEL_TEST(parse_overloads_747, byte_parse_overflow_throws_1134) {
    BEZEL_EXPECT_EQ(run(
        "module M; static class F public static method Main(): int "
        "  try "
        "    var b := byte.Parse(\"300\"); return 0; "
        "  catch var e: OverflowException => return 1; "
        "  end; "
        "end Main; end F;"), 1);
}

// int.TryParse returns false for an out-of-range value (no exception).
BEZEL_TEST(parse_overloads_747, int_tryparse_overflow_returns_false_1134) {
    BEZEL_EXPECT_EQ(run(
        "module M; static class F public static method Main(): int "
        "  var v: int := 0; "
        "  var ok: bool := int.TryParse(\"99999999999\", out v); "
        "  return (if not ok then 1 else 0); "                      // ok must be false
        "end Main; end F;"), 1);
}

// int.TryParse still succeeds and sets the value for an in-range string.
BEZEL_TEST(parse_overloads_747, int_tryparse_in_range_unaffected_1134) {
    BEZEL_EXPECT_EQ(run(
        "module M; static class F public static method Main(): int "
        "  var v: int := 0; "
        "  var ok: bool := int.TryParse(\"123\", out v); "
        "  return (if ok then v else 0); "                          // 123
        "end Main; end F;"), 123);
}

// ── #1227 / §9.1: int.Parse / int.TryParse reject invalid input ───────────────
// Parse("abc") / ("") / ("123abc") raise FormatException; TryParse leaves
// out=0 (the default) on failure, not the partial-parse value.
// Authority: stdlib §9.1 + §9.3 strict-fault model ("no surrounding whitespace;
// callers trim explicitly").

// int.Parse("abc") — leading non-numeric → FormatException.
BEZEL_TEST(parse_overloads_747, int_parse_non_numeric_throws_1227) {
    // stdlib §9.1: Parse raises FormatException for non-numeric input.
    BEZEL_EXPECT_EQ(run(
        "module M; static class F public static method Main(): int "
        "  try "
        "    var x := int.Parse(\"abc\"); return 0; "
        "  catch var e: FormatException => return 1; "
        "  end; "
        "end Main; end F;"), 1);
}

// int.Parse("") — empty string → FormatException.
BEZEL_TEST(parse_overloads_747, int_parse_empty_throws_1227) {
    // stdlib §9.1: empty string is not a valid integer representation.
    BEZEL_EXPECT_EQ(run(
        "module M; static class F public static method Main(): int "
        "  try "
        "    var x := int.Parse(\"\"); return 0; "
        "  catch var e: FormatException => return 1; "
        "  end; "
        "end Main; end F;"), 1);
}

// int.Parse("123abc") — trailing garbage → FormatException.
BEZEL_TEST(parse_overloads_747, int_parse_trailing_garbage_throws_1227) {
    // stdlib §9.1: every character in the string must be part of the numeric
    // representation; trailing non-numeric chars raise FormatException.
    BEZEL_EXPECT_EQ(run(
        "module M; static class F public static method Main(): int "
        "  try "
        "    var x := int.Parse(\"123abc\"); return 0; "
        "  catch var e: FormatException => return 1; "
        "  end; "
        "end Main; end F;"), 1);
}

// int.TryParse("123abc", out v): false, v=0 (not the partial value 123).
BEZEL_TEST(parse_overloads_747, int_tryparse_trailing_garbage_zeroes_out_1227) {
    // stdlib §9.1 (shape (c): false, out = 0, where Parse raises) and §1
    // shape (c): the out parameter receives default(T) = 0 (spec §17.11).
    BEZEL_EXPECT_EQ(run(
        "module M; static class F public static method Main(): int "
        "  var v: int := 99; "
        "  var ok: bool := int.TryParse(\"123abc\", out v); "
        "  return (if not ok then v else 1); "  // ok=false, v must be 0
        "end Main; end F;"), 0);
}

// long.Parse("abc") — non-numeric → FormatException.
BEZEL_TEST(parse_overloads_747, long_parse_non_numeric_throws_1227) {
    // stdlib §9.1: same strict-fault model applies to long.Parse.
    BEZEL_EXPECT_EQ(run(
        "module M; static class F public static method Main(): int "
        "  try "
        "    var x := long.Parse(\"abc\"); return 0; "
        "  catch var e: FormatException => return 1; "
        "  end; "
        "end Main; end F;"), 1);
}

// long.Parse("123abc") — trailing garbage → FormatException.
BEZEL_TEST(parse_overloads_747, long_parse_trailing_garbage_throws_1227) {
    BEZEL_EXPECT_EQ(run(
        "module M; static class F public static method Main(): int "
        "  try "
        "    var x := long.Parse(\"123abc\"); return 0; "
        "  catch var e: FormatException => return 1; "
        "  end; "
        "end Main; end F;"), 1);
}

// long.Parse("9223372036854775807") (Int64.MaxValue) — valid, no throw.
BEZEL_TEST(parse_overloads_747, long_parse_maxvalue_valid_1227) {
    BEZEL_EXPECT_EQ(run(
        "module M; static class F public static method Main(): int "
        "  var x := long.Parse(\"9223372036854775807\"); return 42; "
        "end Main; end F;"), 42);
}

// ── #1227 / §9.2: double.Parse / double.TryParse reject invalid input ─────────

// double.Parse("abc") → FormatException.
BEZEL_TEST(parse_overloads_747, double_parse_non_numeric_throws_1227) {
    // stdlib §9.2: Parse raises FormatException for non-numeric input.
    BEZEL_EXPECT_EQ(run(
        "module M; static class F public static method Main(): int "
        "  try "
        "    var x := double.Parse(\"abc\"); return 0; "
        "  catch var e: FormatException => return 1; "
        "  end; "
        "end Main; end F;"), 1);
}

// double.Parse("") — empty string → FormatException.
BEZEL_TEST(parse_overloads_747, double_parse_empty_throws_1227) {
    BEZEL_EXPECT_EQ(run(
        "module M; static class F public static method Main(): int "
        "  try "
        "    var x := double.Parse(\"\"); return 0; "
        "  catch var e: FormatException => return 1; "
        "  end; "
        "end Main; end F;"), 1);
}

// double.Parse("1.5abc") — trailing garbage → FormatException.
BEZEL_TEST(parse_overloads_747, double_parse_trailing_garbage_throws_1227) {
    BEZEL_EXPECT_EQ(run(
        "module M; static class F public static method Main(): int "
        "  try "
        "    var x := double.Parse(\"1.5abc\"); return 0; "
        "  catch var e: FormatException => return 1; "
        "  end; "
        "end Main; end F;"), 1);
}

// double.Parse("1.5") — valid, no throw.
BEZEL_TEST(parse_overloads_747, double_parse_valid_unaffected_1227) {
    // stdlib §9.2: valid numeric strings still parse correctly.
    BEZEL_EXPECT_EQ(run(
        "module M; static class F public static method Main(): int "
        "  var x := double.Parse(\"1.5\"); return 42; "
        "end Main; end F;"), 42);
}

// double.TryParse("1.5abc", out v): false, v=0.0 (not 1.5).
BEZEL_TEST(parse_overloads_747, double_tryparse_trailing_garbage_zeroes_out_1227) {
    // stdlib §9.2 (Double/Float TryParse): the out param receives default(T)
    // = 0.0 on failure (stdlib §1 shape (c), spec §17.11).
    BEZEL_EXPECT_EQ(run(
        "module M; static class F public static method Main(): int "
        "  var v: double := 99.0; "
        "  var ok: bool := double.TryParse(\"1.5abc\", out v); "
        "  return (if not ok then int(v) else 1); "  // ok=false, v must be 0.0
        "end Main; end F;"), 0);
}

// ── #1231 / §6.1: Math.Abs(MinValue) raises OverflowException ─────────────────
// |int.MinValue| = 2147483648 is not representable as int (max is 2147483647).
// |long.MinValue| similarly. Per §6.1: operations whose result is not in range
// raise OverflowException.

// Math.Abs(int.MinValue) → OverflowException.
BEZEL_TEST(parse_overloads_747, math_abs_int_minvalue_throws_1231) {
    // stdlib §2.2 Abs(x: int): int, §6.1: overflow raises OverflowException.
    BEZEL_EXPECT_EQ(run(
        "module M; static class F public static method Main(): int "
        "  try "
        "    var r := Math.Abs(int.MinValue); return 0; "
        "  catch var e: OverflowException => return 1; "
        "  end; "
        "end Main; end F;"), 1);
}

// Math.Abs(long.MinValue) → OverflowException.
BEZEL_TEST(parse_overloads_747, math_abs_long_minvalue_throws_1231) {
    // stdlib §2.2 Abs(x: long): long, §6.1: overflow raises OverflowException.
    BEZEL_EXPECT_EQ(run(
        "module M; static class F public static method Main(): int "
        "  try "
        "    var r := Math.Abs(long.MinValue); return 0; "
        "  catch var e: OverflowException => return 1; "
        "  end; "
        "end Main; end F;"), 1);
}

// Math.Abs(-5) → 5 (unchanged — non-MinValue inputs still work).
BEZEL_TEST(parse_overloads_747, math_abs_neg5_unaffected_1231) {
    // stdlib §2.2: Abs(-5) = 5; OverflowException guard must not fire here.
    BEZEL_EXPECT_EQ(run(
        "module M; static class F public static method Main(): int "
        "  return Math.Abs(0 - 5); "  // 5
        "end Main; end F;"), 5);
}

// Math.Abs(-2147483647) = 2147483647 (MaxValue, representable, no throw).
BEZEL_TEST(parse_overloads_747, math_abs_near_minvalue_unaffected_1231) {
    // stdlib §2.2: Abs(MinValue + 1) = MaxValue, no overflow.
    BEZEL_EXPECT_EQ(run(
        "module M; static class F public static method Main(): int "
        "  var r := Math.Abs(int.MinValue + 1); "  // 2147483647 = MaxValue
        "  return (if r = int.MaxValue then 42 else 0); "
        "end Main; end F;"), 42);
}
