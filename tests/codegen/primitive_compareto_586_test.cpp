// fix #586: CompareTo on primitive numeric types (§2.8 IComparable<T>).
// stdlib §9.1 (Int32), §9.2 (Double/Float), §9.3 (Decimal), §9.5 (Char)
// — all implement IComparable<T>.  CompareTo(other) must return:
//   negative if self < other, zero if equal, positive if self > other.
// The bug: int.CompareTo returned 0 / garbage instead of the correct sign.
// A second call to CompareTo on the same receiver returned large garbage
// (sign-mismatch symptom of a codegen fall-through to stale IR).

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}

// Helper: sign-normalise to -1/0/1 for clean exit-code testing.
// We use return-value clamping in the Bezel source itself.
// Exit codes are: 2 = positive, 0 = zero, 1 = negative  (encoded)

// ---------------------------------------------------------------------------
// §9.1 int.CompareTo — the primary bug report (#586)
// ---------------------------------------------------------------------------

// §2.8 / stdlib §9.1: int.CompareTo — self > other must be positive.
BEZEL_TEST(codegen_586, int_compareto_greater_returns_positive) {
    // a=10 > b=3 → positive; we return 2 iff positive, 0 iff zero, 1 iff negative.
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a := 10; var b := 3; var r := a.CompareTo(b); "
        "  return (2 if r > 0 else (0 if r = 0 else 1)); "
        "end Main; end C;"), 2);
}

// §2.8 / stdlib §9.1: int.CompareTo — self < other must be negative.
BEZEL_TEST(codegen_586, int_compareto_less_returns_negative) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a := 3; var b := 10; var r := a.CompareTo(b); "
        "  return (2 if r > 0 else (0 if r = 0 else 1)); "
        "end Main; end C;"), 1);
}

// §2.8 / stdlib §9.1: int.CompareTo — self = other must be zero.
BEZEL_TEST(codegen_586, int_compareto_equal_returns_zero) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a := 5; var r := a.CompareTo(5); "
        "  return (2 if r > 0 else (0 if r = 0 else 1)); "
        "end Main; end C;"), 0);
}

// §2.8 / stdlib §9.1: second call on same receiver must not produce garbage
// (the original bug: first call → 0, second call → large garbage value).
BEZEL_TEST(codegen_586, int_compareto_two_calls_consistent) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a := 10; var b := 3; "
        "  var r1 := a.CompareTo(b); "   // should be positive
        "  var r2 := a.CompareTo(b); "   // second call must also be positive
        "  return (2 if (r1 > 0 and r2 > 0) else 1); "
        "end Main; end C;"), 2);
}

// §2.8: negative numbers — a=-5 < b=0 → negative.
BEZEL_TEST(codegen_586, int_compareto_negative_values) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a := -5; var b := 0; var r := a.CompareTo(b); "
        "  return (2 if r > 0 else (0 if r = 0 else 1)); "
        "end Main; end C;"), 1);
}

// §2.8: both negative — a=-3 > b=-10 → positive.
BEZEL_TEST(codegen_586, int_compareto_both_negative) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a := -3; var b := -10; var r := a.CompareTo(b); "
        "  return (2 if r > 0 else (0 if r = 0 else 1)); "
        "end Main; end C;"), 2);
}

// ---------------------------------------------------------------------------
// §9.2 double.CompareTo
// ---------------------------------------------------------------------------

// stdlib §9.2: double.CompareTo — self > other → positive.
BEZEL_TEST(codegen_586, double_compareto_greater_returns_positive) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a := 3.14; var b := 2.71; var r := a.CompareTo(b); "
        "  return (2 if r > 0 else (0 if r = 0 else 1)); "
        "end Main; end C;"), 2);
}

// stdlib §9.2: double.CompareTo — self < other → negative.
BEZEL_TEST(codegen_586, double_compareto_less_returns_negative) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a := 1.0; var b := 2.0; var r := a.CompareTo(b); "
        "  return (2 if r > 0 else (0 if r = 0 else 1)); "
        "end Main; end C;"), 1);
}

// stdlib §9.2: double.CompareTo — self = other → zero.
BEZEL_TEST(codegen_586, double_compareto_equal_returns_zero) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a := 1.5; var r := a.CompareTo(1.5); "
        "  return (2 if r > 0 else (0 if r = 0 else 1)); "
        "end Main; end C;"), 0);
}

// ---------------------------------------------------------------------------
// §9.5 char.CompareTo
// ---------------------------------------------------------------------------

// stdlib §9.5: char.CompareTo — 'z' > 'a' → positive.
BEZEL_TEST(codegen_586, char_compareto_greater_returns_positive) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a := 'z'; var b := 'a'; var r := a.CompareTo(b); "
        "  return (2 if r > 0 else (0 if r = 0 else 1)); "
        "end Main; end C;"), 2);
}

// stdlib §9.5: char.CompareTo — 'a' < 'z' → negative.
BEZEL_TEST(codegen_586, char_compareto_less_returns_negative) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a := 'a'; var b := 'z'; var r := a.CompareTo(b); "
        "  return (2 if r > 0 else (0 if r = 0 else 1)); "
        "end Main; end C;"), 1);
}

// stdlib §9.5: char.CompareTo — 'x' = 'x' → zero.
BEZEL_TEST(codegen_586, char_compareto_equal_returns_zero) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a := 'x'; var r := a.CompareTo('x'); "
        "  return (2 if r > 0 else (0 if r = 0 else 1)); "
        "end Main; end C;"), 0);
}

// ---------------------------------------------------------------------------
// Regression: string.CompareTo must still work (was working before)
// ---------------------------------------------------------------------------

// stdlib §9.6: string.CompareTo — "z" > "a" → positive.
BEZEL_TEST(codegen_586, string_compareto_regression_greater) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a := \"z\"; var b := \"a\"; var r := a.CompareTo(b); "
        "  return (2 if r > 0 else (0 if r = 0 else 1)); "
        "end Main; end C;"), 2);
}

// stdlib §9.6: string.CompareTo — "a" < "z" → negative.
BEZEL_TEST(codegen_586, string_compareto_regression_less) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a := \"a\"; var b := \"z\"; var r := a.CompareTo(b); "
        "  return (2 if r > 0 else (0 if r = 0 else 1)); "
        "end Main; end C;"), 1);
}

// stdlib §9.6: string.CompareTo — equal strings → zero.
BEZEL_TEST(codegen_586, string_compareto_regression_equal) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a := \"hello\"; var r := a.CompareTo(\"hello\"); "
        "  return (2 if r > 0 else (0 if r = 0 else 1)); "
        "end Main; end C;"), 0);
}

}  // namespace
