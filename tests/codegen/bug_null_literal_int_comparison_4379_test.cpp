// Codegen — issue #4379: binary equality `null = int` / `null <> int` (null
// literal on the LEFT operand) produced wrong codegen results.
//
// When comparing a null literal to a non-null value type (int, bool, etc.),
// the result should be deterministic: null never equals a non-null value, so
// `null = n` is always FALSE and `null <> n` is always TRUE, regardless of
// which side null appears on.
//
// Before the fix, the type-mismatch guard in lowerBinary (LlvmBackend_Exprs.cpp
// line 9096) handled the case where the LEFT operand is a null pointer and the
// RIGHT is a non-pointer value. It emitted:
//     CreateICmpEQ(null_ptr, null_ptr)  // = TRUE
// and returned `op == "=" ? eq : CreateNot(eq)` = TRUE for `=`.
//
// This is wrong: the comparison is between a null pointer and a non-null value,
// so `null = n` must be FALSE (null is never equal to a non-null int), and
// `null <> n` must be TRUE. The guard was treating the comparison as "is the
// left operand null?" rather than "are the two operands equal?".
//
// The fix mirrors the correct logic already used at line 9101-9104 for the
// symmetric right-is-ptr case: a non-pointer value can never be null, so
// `= null` is always FALSE and `<> null` is always TRUE.
//
// The symmetric cases `int = null` / `int <> null` (null on RIGHT) were already
// correct — the right-is-ptr guard at line 9101-9104 correctly returned
// `getInt1(op == "<>")`.
//
// Spec derivation: §8.4 (null-safe equality), §7.2 (null literal), §7.1
// (nullable types). Fix location: LlvmBackend_Exprs.cpp type-mismatch guard
// for `null = int` (left-is-ptr branch).

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// ── null literal on LEFT operand ──────────────────────────────────────────────

// `null = int` (null on left): null never equals a non-null int → FALSE.
BEZEL_TEST(bug_null_literal_int_comparison_4379, null_eq_int_left_returns_false) {
    // Before fix: returned TRUE (exit 11). After fix: returns FALSE (exit 12).
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C public static method Main(): int "
        "  var n: int := 5; "
        "  if null = n then return 11; else return 12; end; "  // else fires
        "end Main; end C;"), 12);
}

// `null <> int` (null on left): null is always not-equal to a non-null int → TRUE.
BEZEL_TEST(bug_null_literal_int_comparison_4379, null_neq_int_left_returns_true) {
    // Before fix: returned FALSE (exit 12). After fix: returns TRUE (exit 11).
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C public static method Main(): int "
        "  var n: int := 5; "
        "  if null <> n then return 11; else return 12; end; "  // then fires
        "end Main; end C;"), 11);
}

// `null = bool` (null on left, bool operand): same null-vs-nonnull-value rule.
BEZEL_TEST(bug_null_literal_int_comparison_4379, null_eq_bool_left_returns_false) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C public static method Main(): int "
        "  var b: bool := true; "
        "  if null = b then return 11; else return 12; end; "
        "end Main; end C;"), 12);
}

// `null <> bool` (null on left, bool operand): always TRUE.
BEZEL_TEST(bug_null_literal_int_comparison_4379, null_neq_bool_left_returns_true) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C public static method Main(): int "
        "  var b: bool := true; "
        "  if null <> b then return 11; else return 12; end; "
        "end Main; end C;"), 11);
}

// `null = char` (null on left, char operand): null never equals a char → FALSE.
BEZEL_TEST(bug_null_literal_int_comparison_4379, null_eq_char_left_returns_false) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C public static method Main(): int "
        "  var c: char := 'x'; "
        "  if null = c then return 11; else return 12; end; "
        "end Main; end C;"), 12);
}

// `null = double` (null on left, double operand): null never equals a double → FALSE.
BEZEL_TEST(bug_null_literal_int_comparison_4379, null_eq_double_left_returns_false) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C public static method Main(): int "
        "  var d: double := 3.14; "
        "  if null = d then return 11; else return 12; end; "
        "end Main; end C;"), 12);
}

// `null <> double` (null on left): always TRUE.
BEZEL_TEST(bug_null_literal_int_comparison_4379, null_neq_double_left_returns_true) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C public static method Main(): int "
        "  var d: double := 3.14; "
        "  if null <> d then return 11; else return 12; end; "
        "end Main; end C;"), 11);
}

// ── null literal on RIGHT operand (regression control) ────────────────────────
// These should have been correct before the fix; they verify the right-is-ptr
// path (line 9101-9104) is undisturbed.

// `int = null` (null on right): always FALSE.
BEZEL_TEST(bug_null_literal_int_comparison_4379, int_eq_null_right_returns_false) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C public static method Main(): int "
        "  var n: int := 5; "
        "  if n = null then return 11; else return 12; end; "
        "end Main; end C;"), 12);
}

// `int <> null` (null on right): always TRUE.
BEZEL_TEST(bug_null_literal_int_comparison_4379, int_neq_null_right_returns_true) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C public static method Main(): int "
        "  var n: int := 5; "
        "  if n <> null then return 11; else return 12; end; "
        "end Main; end C;"), 11);
}
