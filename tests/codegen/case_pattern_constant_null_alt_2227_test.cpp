// Codegen + sema — #2227: a ConstantPattern whose member has a type
// INCOMPATIBLE with the scrutinee must be a clean §19 diagnostic, not an LLVM
// `ICmpInst::AssertOK` abort. The #964 / #1213 / #1202 sema guards cover the
// primitive-vs-primitive comparison-class cases; #1471 covers the nullable
// value-type `null`-member case. Two narrow paths survive those guards:
//   1. A `null` literal folded into a `|`-alternative by the parser over a
//      NON-NULLABLE int/float/bool/enum scrutinee — `null` is allowed by
//      §19.3 as a constant member, but `ctx_.exprType(*c)` is null for a
//      NullLiteralExpr in pattern context, so the existing sema guard's
//      early-return (ExprTypecheck_Exprs.cpp L5818) skips the check.
//   2. A STRING literal ConstantPattern over a NAMED (enum/record) scrutinee
//      — the existing sema guard restricts itself to `scrutTy->kind ==
//      TypeRef::Kind::Primitive`, so an enum/record scrutinee is unguarded.
// Both reach codegen with a `null` or string pointer and a non-pointer
// scrutinee; `reconcile` bridges only int/float widths and `emitEquals` then
// builds `ICmpEQ` on mismatched types → hard abort.
//
// Fix (codegen, LlvmBackend_Exprs2.cpp patternMatches ConstantPattern loop):
// mirror the NamedPattern handler's type-compatibility guard at L6422-6436 —
// detect the fundamental type mismatch between the lowered constant `cv` and
// the scrutinee `scrut` (pointer-vs-non-pointer, i1-vs-wider-int, fp-vs-int
// residuals), emit the §19 diagnostic once per pattern, and OR a never-match
// into the alternative so the rest of the arm's other members still match.
//
// Each test FAILS without the fix (LLVM SIGABRT, rc 134) and PASSES with it.

#include "support/aot_support.hpp"   // runAot — AOT test harness
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
bool compileErrored(const std::string& src) {
    return runAot(src).diagnostics.hasErrors();
}
}  // namespace

// fix #2227 / §19.3 / §19.5 / fix #5189 / §19.6 (v0.311): a `null` literal
// in a `|`-alternative over a non-nullable int scrutinee is ADMITTED
// silently and never selected (§19.6 "admitted and never selected" bullet
// for non-nullable scrutinees). Pre-#5189 the codegen guard emitted a §19
// diagnostic; v0.311 / #5189 reverses it — the `null` member contributes
// `false` to the OR, the next literal matches, the arm selects.
BEZEL_TEST(case_pattern_null_alt_2227, int_scrutinee_null_alt_admitted_spec_19_6) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var n := 5; "
        "  return (case n of null | 5 => 1, _ => 2); "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(r.diagnostics.hasErrors(), false);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// fix #2227 / §19.3 / fix #5189 / §19.6 (v0.311): same shape over float.
// Note: f = 3.14, the alt-list `null | 1.0` does NOT match (1.0 != 3.14),
// so `_` selects → 2. The non-matching exitCode distinguishes "the `null`
// member is admitted silently" from "the alt-list matches".
BEZEL_TEST(case_pattern_null_alt_2227, float_scrutinee_null_alt_admitted_spec_19_6) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var f := 3.14; "
        "  return (case f of null | 1.0 => 1, _ => 2); "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(r.diagnostics.hasErrors(), false);
    BEZEL_EXPECT_EQ(r.exitCode, 2);
}

// fix #2227 / §19.3 / fix #5189 / §19.6 (v0.311): same shape over bool.
BEZEL_TEST(case_pattern_null_alt_2227, bool_scrutinee_null_alt_admitted_spec_19_6) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var b := true; "
        "  return (case b of null | true => 1, _ => 2); "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(r.diagnostics.hasErrors(), false);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// fix #2227 / §19.3: a string-literal ConstantPattern over an enum scrutinee
// is rejected with a §19 diagnostic (the existing sema guard restricts itself
// to `scrutTy->kind == TypeRef::Kind::Primitive` and so misses an enum).
BEZEL_TEST(case_pattern_null_alt_2227, enum_scrutinee_string_literal_rejected) {
    BEZEL_EXPECT_EQ(compileErrored(
        "module M; enum Color Red, Green, Blue; end Color; "
        "class C public static method Main(): int "
        "  var c: Color := Color.Red; "
        "  return (case c of \"Red\" => 1, _ => 2); "
        "end Main; end C;"), true);
}

// Regression: a `null` member in a `|`-alternative over a NULLABLE int
// scrutinee continues to compile and match (the `null` is a real alternative
// for `int?`, handled by the `isNullableVal` branch — distinct from this fix's
// #2227 path).
BEZEL_TEST(case_pattern_null_alt_2227, nullable_int_scrutinee_null_alt_matches) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var n: int? := null; "
        "  return (case n of null | 0 => 1, _ => 2); "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(r.diagnostics.hasErrors(), false);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// Regression: a non-`null` int alternative over an int scrutinee still matches
// (no over-rejection — the codegen guard only fires on a fundamental type
// mismatch, not on a valid int-vs-int compare).
BEZEL_TEST(case_pattern_null_alt_2227, int_alt_unaffected) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var n := 7; "
        "  return (case n of 1 | 5 | 7 => 1, _ => 2); "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(r.diagnostics.hasErrors(), false);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// Regression: a string literal alternative over a string scrutinee continues
// to work (the codegen guard's `isPointerTy()` kind check keeps both sides
// pointer-typed, so it is not flagged as incompatible).
BEZEL_TEST(case_pattern_null_alt_2227, string_alt_unaffected) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var s := \"hi\"; "
        "  return (case s of \"hi\" | \"bye\" => 1, _ => 2); "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(r.diagnostics.hasErrors(), false);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}
