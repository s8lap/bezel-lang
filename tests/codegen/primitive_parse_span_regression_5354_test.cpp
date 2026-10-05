// Regression tests for #5354 — int.Parse(ReadOnlyStringSpan) returning a silent
// garbage value; double/decimal.Parse(span) emitting malformed IR.
//
// WHAT THE DOCUMENTS REQUIRE:
// - stdlib §9.1 (v0.313, #1810): int.Parse(ReadOnlyStringSpan) accepts exactly
//   the inputs the string forms accept — same FormatException/OverflowException
//   contract, no whitespace tolerance — over a ReadOnlyStringSpan window.
//   `int.Parse("ab 42 cd".AsSpan(3, 2))` is `42`.
// - stdlib §9.2 (v0.313, #1810): double.Parse(ReadOnlyStringSpan) — same
//   contract as the string form, span overload.
// - stdlib §9.3 (v0.313, #1810): decimal.Parse(ReadOnlyStringSpan) — same
//   contract as the string form, span overload.
//
// MEASURED PRE-FIX (r23-c28):
//   A1. int.Parse("ab 42 cd".AsSpan(3, 2)): prints 4435344, exit 0 — the span
//       struct {ptr,i32,i32} was returned as the int result; the adjacent
//       memory values landed in the return value slot.
//   A2. double.Parse(sp): IR verification failure (span passed to bz_double_to_string_r).
//   A3. decimal.Parse(sp): IR verification failure (span passed to bz_decimal_parse).
//
// THE FIX (stdlib §9.1–§9.3, #1810):
//   parseWindow(arg) materializes the ReadOnlyStringSpan window as a fresh
//   +1 bz.string whose buffer IS NUL-terminated at the window's end. The
//   existing validation ("must advance AND *endptr == '\0'") means exactly
//   "the window, and only the window, was valid input" for a span, unchanged
//   from the string forms. The window is released on the normal (non-throw) exit.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

std::string main_(const std::string& body) {
    return std::string("module M; class C public static method Main(): int ") +
           body + " return 0; end Main; end C;";
}

// Assert stdout byte-for-byte and exit 0.
void expectOut(const std::string& body, const std::string& want) {
    auto r = runAot(main_(body));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, want);
}

// Assert stdout byte-for-byte and exit 0 — runAotChecked (ASan/LSan armed).
void expectOutChecked(const std::string& body, const std::string& want) {
    auto r = runAotChecked(main_(body));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, want);
}

// Assert FormatException is raised and caught: on the fixed code, the
// FormatException IS thrown and caught → stdout is "". On the pre-fix code,
// int.Parse(span) does NOT throw (returns garbage int) → stdout is "0\n".
// Asserting stdoutText != "" distinguishes the two: pre-fix fails, fix passes.
void expectParseFormatException(const std::string& body) {
    auto r = runAot(main_(body));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // On pre-fix: Parse returns garbage (not throwing) → Console.WriteLine prints "0\n".
    // On fix: FormatException is thrown and caught → stdout is empty.
    BEZEL_EXPECT_TRUE(r.stdoutText != "0\n");
    BEZEL_EXPECT_EQ(r.stdoutText, "");
}

// Assert OverflowException is raised and NOT caught: on the fixed code, the
// OverflowException is thrown and NOT caught by the test's try/catch (the test
// has no OverflowException arm) → terminate() → abort() → non-zero exit.
// On the pre-fix code: int.Parse(span) does NOT throw (returns garbage 0) → exit 0.
void expectParseOverflowException(const std::string& body) {
    auto r = runAot(main_(body));
    BEZEL_EXPECT_TRUE(r.ran);
    // On pre-fix: Parse returns garbage (not throwing) → exits normally with 0.
    // On fix: OverflowException is NOT caught → terminate() → abort() → non-zero.
    BEZEL_EXPECT_TRUE(r.exitCode != 0);
}

}  // namespace

// ---- stdlib §9.1 int.Parse(ReadOnlyStringSpan) -----------------------------

// fix #5354 A1: int.Parse(sp) must return 42, not a garbage value.
// Pre-fix: prints 4435344 (adjacent memory returned as int).
BEZEL_TEST(primitive_parse_span_regression_5354, int_parse_span_success) {
    // "ab 42 cd".AsSpan(3, 2) is the window over "42" (start=3, length=2).
    // int.Parse must return 42 (§9.1, #1810).
    expectOutChecked(
        "var sp := \"ab 42 cd\".AsSpan(3, 2); "
        "Console.WriteLine(int.Parse(sp));",
        "42\n");
}

// fix #5354 A1: FormatException on an invalid span window.
// "abc" is not a valid integer; int.Parse must raise FormatException.
// FormatException is caught → exit 0.
BEZEL_TEST(primitive_parse_span_regression_5354, int_parse_span_format_exception) {
    expectParseFormatException(
        "var sp := \"abc\".AsSpan(0, 3); "
        "try "
        "    int.Parse(sp); "
        "catch FormatException => "
        "    return 0; "
        "end try;");
}

// fix #5354 A1: OverflowException on a window whose value overflows int.
// "99999999999" is outside INT32_MAX; the span window must raise OverflowException.
// OverflowException is NOT caught → uncaught → terminate() → exit(1).
BEZEL_TEST(primitive_parse_span_regression_5354, int_parse_span_overflow_exception) {
    expectParseOverflowException(
        "var sp := \"99999999999\".AsSpan(0, 11); "
        "int.Parse(sp);");
}

// ---- stdlib §9.2 double.Parse(ReadOnlyStringSpan) --------------------------

// fix #5354 A2: double.Parse(sp) must return 3.14, not malformed IR.
// Pre-fix: IR verification failure (span passed to bz_double_to_string_r).
BEZEL_TEST(primitive_parse_span_regression_5354, double_parse_span_success) {
    expectOutChecked(
        "var sp := \"3.14\".AsSpan(0, 4); "
        "Console.WriteLine(double.Parse(sp));",
        "3.14\n");
}

// fix #5354 A2: FormatException.
BEZEL_TEST(primitive_parse_span_regression_5354, double_parse_span_format_exception) {
    expectParseFormatException(
        "var sp := \"abc\".AsSpan(0, 3); "
        "try "
        "    double.Parse(sp); "
        "catch FormatException => "
        "    return 0; "
        "end try;");
}

// ---- stdlib §9.3 decimal.Parse(ReadOnlyStringSpan) --------------------------

// fix #5354 A3: decimal.Parse(sp) must return 2.5, not malformed IR.
// Pre-fix: IR verification failure (span record passed to bz_decimal_parse).
BEZEL_TEST(primitive_parse_span_regression_5354, decimal_parse_span_success) {
    expectOutChecked(
        "var sp := \"2.5\".AsSpan(0, 3); "
        "Console.WriteLine(decimal.Parse(sp));",
        "2.5\n");
}

// fix #5354 A3: FormatException.
BEZEL_TEST(primitive_parse_span_regression_5354, decimal_parse_span_format_exception) {
    expectParseFormatException(
        "var sp := \"xyz\".AsSpan(0, 3); "
        "try "
        "    decimal.Parse(sp); "
        "catch FormatException => "
        "    return 0; "
        "end try;");
}

// ---- long.Parse(ReadOnlyStringSpan) — same family, same fix ----------------

BEZEL_TEST(primitive_parse_span_regression_5354, long_parse_span_success) {
    expectOutChecked(
        "var sp := \"9876543210\".AsSpan(0, 10); "
        "Console.WriteLine(long.Parse(sp));",
        "9876543210\n");
}

// ---- float.Parse(ReadOnlyStringSpan) — same family, same fix ----------------

BEZEL_TEST(primitive_parse_span_regression_5354, float_parse_span_success) {
    expectOutChecked(
        "var sp := \"2.5\".AsSpan(0, 3); "
        "Console.WriteLine(float.Parse(sp));",
        "2.5\n");
}
