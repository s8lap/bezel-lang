// fix #586: CompareTo on primitive numeric types (§2.8 IComparable<T>).
// stdlib §9.1 (Int32), §9.2 (Double/Float), §9.3 (Decimal), §9.5 (Char)
// — all implement IComparable<T>.  CompareTo(other) must return:
//   negative if self < other, zero if equal, positive if self > other.
// The bug: int.CompareTo returned 0 / garbage instead of the correct sign.
// A second call to CompareTo on the same receiver returned large garbage
// (sign-mismatch symptom of a codegen fall-through to stale IR).

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
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
        "  return (if r > 0 then 2 else (if r = 0 then 0 else 1)); "
        "end Main; end C;"), 2);
}

// §2.8 / stdlib §9.1: int.CompareTo — self < other must be negative.
BEZEL_TEST(codegen_586, int_compareto_less_returns_negative) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a := 3; var b := 10; var r := a.CompareTo(b); "
        "  return (if r > 0 then 2 else (if r = 0 then 0 else 1)); "
        "end Main; end C;"), 1);
}

// §2.8 / stdlib §9.1: int.CompareTo — self = other must be zero.
BEZEL_TEST(codegen_586, int_compareto_equal_returns_zero) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a := 5; var r := a.CompareTo(5); "
        "  return (if r > 0 then 2 else (if r = 0 then 0 else 1)); "
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
        "  return (if (r1 > 0 and r2 > 0) then 2 else 1); "
        "end Main; end C;"), 2);
}

// §2.8: negative numbers — a=-5 < b=0 → negative.
BEZEL_TEST(codegen_586, int_compareto_negative_values) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a := -5; var b := 0; var r := a.CompareTo(b); "
        "  return (if r > 0 then 2 else (if r = 0 then 0 else 1)); "
        "end Main; end C;"), 1);
}

// §2.8: both negative — a=-3 > b=-10 → positive.
BEZEL_TEST(codegen_586, int_compareto_both_negative) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a := -3; var b := -10; var r := a.CompareTo(b); "
        "  return (if r > 0 then 2 else (if r = 0 then 0 else 1)); "
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
        "  return (if r > 0 then 2 else (if r = 0 then 0 else 1)); "
        "end Main; end C;"), 2);
}

// stdlib §9.2: double.CompareTo — self < other → negative.
BEZEL_TEST(codegen_586, double_compareto_less_returns_negative) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a := 1.0; var b := 2.0; var r := a.CompareTo(b); "
        "  return (if r > 0 then 2 else (if r = 0 then 0 else 1)); "
        "end Main; end C;"), 1);
}

// stdlib §9.2: double.CompareTo — self = other → zero.
BEZEL_TEST(codegen_586, double_compareto_equal_returns_zero) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a := 1.5; var r := a.CompareTo(1.5); "
        "  return (if r > 0 then 2 else (if r = 0 then 0 else 1)); "
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
        "  return (if r > 0 then 2 else (if r = 0 then 0 else 1)); "
        "end Main; end C;"), 2);
}

// stdlib §9.5: char.CompareTo — 'a' < 'z' → negative.
BEZEL_TEST(codegen_586, char_compareto_less_returns_negative) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a := 'a'; var b := 'z'; var r := a.CompareTo(b); "
        "  return (if r > 0 then 2 else (if r = 0 then 0 else 1)); "
        "end Main; end C;"), 1);
}

// stdlib §9.5: char.CompareTo — 'x' = 'x' → zero.
BEZEL_TEST(codegen_586, char_compareto_equal_returns_zero) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a := 'x'; var r := a.CompareTo('x'); "
        "  return (if r > 0 then 2 else (if r = 0 then 0 else 1)); "
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
        "  return (if r > 0 then 2 else (if r = 0 then 0 else 1)); "
        "end Main; end C;"), 2);
}

// stdlib §9.6: string.CompareTo — "a" < "z" → negative.
BEZEL_TEST(codegen_586, string_compareto_regression_less) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a := \"a\"; var b := \"z\"; var r := a.CompareTo(b); "
        "  return (if r > 0 then 2 else (if r = 0 then 0 else 1)); "
        "end Main; end C;"), 1);
}

// stdlib §9.6: string.CompareTo — equal strings → zero.
BEZEL_TEST(codegen_586, string_compareto_regression_equal) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a := \"hello\"; var r := a.CompareTo(\"hello\"); "
        "  return (if r > 0 then 2 else (if r = 0 then 0 else 1)); "
        "end Main; end C;"), 0);
}

}  // namespace

// fix #747 R43 / stdlib §9.1–§9.5: primitive types implement IEquatable<T> with
// an auto-generated Equals(other): bool and GetHashCode(): int. The bug:
// `a.Equals(b)` on a primitive receiver fell through codegen and returned the
// receiver's raw value — always-false when branched, and a `br i32` (non-i1)
// IR-verification failure when stored to a `var`.

// int.Equals — equal returns true, unequal returns false.
BEZEL_TEST(codegen_586, int_equals_true_and_false_747) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: int := 5; var b: int := 5; var c: int := 7; "
        "  var ok := 0; "
        "  if a.Equals(b) then ok := ok + 1; end; "
        "  if not a.Equals(c) then ok := ok + 2; end; "
        "  return ok; "  // 3
        "end Main; end C;"), 3);
}

// Storing the result to a `var` and branching on it must compile (i1 result,
// not i32) and behave correctly — the IR_FAIL repro.
BEZEL_TEST(codegen_586, int_equals_stored_to_var_branches_747) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: int := 5; var b: int := 7; "
        "  var r := a.Equals(b); "
        "  if r then return 1; else return 0; end; "  // 0 — a != b
        "end Main; end C;"), 0);
}

// bool / char / long / double all compare by value through Equals.
BEZEL_TEST(codegen_586, equals_across_primitive_types_747) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var ok := 0; "
        "  var b1: bool := true; var b2: bool := true; "
        "  if b1.Equals(b2) then ok := ok + 1; end; "
        "  var c1: char := 'x'; var c2: char := 'y'; "
        "  if not c1.Equals(c2) then ok := ok + 2; end; "
        "  var l1: long := 100L; var l2: long := 100L; "
        "  if l1.Equals(l2) then ok := ok + 4; end; "
        "  var d1: double := 3.5; var d2: double := 3.5; "
        "  if d1.Equals(d2) then ok := ok + 8; end; "
        "  var d3: double := 3.5; var d4: double := 9.0; "
        "  if not d3.Equals(d4) then ok := ok + 16; end; "
        "  return ok; "  // 31
        "end Main; end C;"), 31);
}

// GetHashCode on an int is the value itself (a valid IEquatable hash).
BEZEL_TEST(codegen_586, int_gethashcode_is_value_747) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: int := 42; "
        "  return (if a.GetHashCode() = 42 then 0 else 1); "  // 0
        "end Main; end C;"), 0);
}
