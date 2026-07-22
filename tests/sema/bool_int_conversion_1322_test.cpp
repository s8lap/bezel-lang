// fix #1322 / §5.1 / §5.2: bool↔numeric conversion is forbidden.
//
// §5.1 lists "Between integers and `bool`" under "never implicit".
// §5.2's explicit-conversion table does not include `bool` as a target from
// any numeric source, nor as a source for any numeric target.
//
// Before this fix the sema §5.2 block in ExprTypecheck_Calls.cpp accepted
// `bool(n)` and `int(b)` silently, lowering them via a runtime-checked
// integer narrowing in codegen (the narrowing bounds were polished by #1269).
// Both forms must now be rejected at compile time with a §5.1/§5.2 diagnostic;
// codegen should never see the conversion.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

bool hasBoolNumericError(const bezel::test::OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("bool") != std::string::npos &&
            (d.message.find("numeric") != std::string::npos ||
             d.message.find("5.1") != std::string::npos ||
             d.message.find("5.2") != std::string::npos))
            return true;
    return false;
}

const char* kHeader =
    "module M; class C public static method Main(): int ";
const char* kFooter = " end Main; end C;";

bezel::test::OwnershipResult check(const std::string& body) {
    return parseAndCheckOwnership(
        std::string(kHeader) + body + kFooter);
}

}  // namespace

// §5.1 / §5.2 (#1322): `bool(n)` where n is int — rejected.
// The four tests below correspond to the #1269 runtime tests that proved the
// old range: they are now compile-time errors, not runtime overflow checks.
BEZEL_TEST(bool_int_conversion_1322, bool_of_int_one_rejected_spec_5_1) {
    auto r = check("var n := 1; var b := bool(n); return 0;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasBoolNumericError(r));
}

BEZEL_TEST(bool_int_conversion_1322, bool_of_int_zero_rejected_spec_5_1) {
    auto r = check("var n := 0; var b := bool(n); return 0;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasBoolNumericError(r));
}

BEZEL_TEST(bool_int_conversion_1322, bool_of_int_two_rejected_spec_5_1) {
    // Formerly expected to raise OverflowException at runtime; now rejected
    // at compile time — the conversion itself is forbidden (§5.1/§5.2).
    auto r = check("var n := 2; var b := bool(n); return 0;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasBoolNumericError(r));
}

BEZEL_TEST(bool_int_conversion_1322, bool_of_int_negone_rejected_spec_5_1) {
    // Same: formerly a runtime overflow; now a compile-time error.
    auto r = check("var n := -1; var b := bool(n); return 0;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasBoolNumericError(r));
}

// §5.2 (#1322 ruling, v0.158): the directions are deliberately ASYMMETRIC.
// Explicit integer(boolExpr) is ADMITTED (total: false→0, true→1) — it is the
// corpus-wide `return int(r = null);` idiom. bool(n) stays forbidden above.
BEZEL_TEST(bool_int_conversion_1322, int_of_bool_true_admitted_spec_5_2) {
    auto r = check("var b := true; var n := int(b); return n;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(bool_int_conversion_1322, int_of_bool_false_admitted_spec_5_2) {
    auto r = check("var b := false; var n := int(b); return n;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(bool_int_conversion_1322, long_of_bool_admitted_spec_5_2) {
    auto r = check("var b := true; var x := long(b); return 0;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(bool_int_conversion_1322, byte_of_bool_admitted_spec_5_2) {
    auto r = check("var b := false; var x := byte(b); return 0;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §5.2 (#1322 ruling): a bool source is rejected for NON-integer numeric
// targets — float/double/decimal have no blessed 0/1 reading; char is a code
// point, not a number.
BEZEL_TEST(bool_int_conversion_1322, double_of_bool_rejected_spec_5_2) {
    auto r = check("var b := true; var x := double(b); return 0;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(bool_int_conversion_1322, char_of_bool_rejected_spec_5_2) {
    auto r = check("var b := true; var x := char(b); return 0;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §5.1 / §5.2 (#1322): bool(float) is also rejected — §5.2 does not include
// bool as a target from floating-point sources either.
BEZEL_TEST(bool_int_conversion_1322, bool_of_double_rejected_spec_5_2) {
    auto r = check("var d := 1.0; var b := bool(d); return 0;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasBoolNumericError(r));
}

// Sanity: legitimate bool usage (conditions, comparisons, bool literals) is
// unaffected by the rejection.
BEZEL_TEST(bool_int_conversion_1322, bool_literal_and_condition_unaffected) {
    auto r = check(
        "var b := true; var n := 0; "
        "if b then n := 1; end; "
        "return n;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(bool_int_conversion_1322, comparison_to_bool_unaffected_spec_5_1) {
    // `n <> 0` is the spec-recommended idiom — it yields bool without any cast.
    auto r = check("var n := 5; var b := n <> 0; return (if b then 1 else 0);");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(bool_int_conversion_1322, bool_in_conditional_expr_unaffected) {
    auto r = check("var n := 3; return (if n > 0 then 1 else 0);");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
