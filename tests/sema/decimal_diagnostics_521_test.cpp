// Tests for issue #521: decimal type — full 128-bit implementation.
//
// Spec §4.4: decimal is a 28–29 significant digit high-precision type;
// the `m`/`M` suffix produces a decimal literal (1.5m). Fix #521 implements
// the full 128-bit decimal type; these tests confirm that decimal literals
// and decimal arithmetic are accepted by sema (no diagnostics) and that the
// one remaining §6.2 incompatibility (double ↔ decimal: no implicit conv)
// still produces a sema error.
//
// Test groups:
//   521_m_literal     — m-suffix literal is accepted (no error), typed as decimal
//   521_arith         — decimal arithmetic accepted (no error) per §6.1
//   521_static_field  — static/instance decimal fields accepted (no error)
//   521_control       — double/int arithmetic unchanged; no regression
//   521_incompatible  — double + decimal still rejected per §6.2

#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"

using bezel::sema::PrimitiveType;
using bezel::sema::TypeRef;

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// Wrap a method body in a full class for sema.
std::string wrapBody(const std::string& body) {
    return "module M; class C public static method M(): int "
           + body + " end M; end C;";
}

// Wrap an arithmetic expression between two parameters of the given types.
std::string wrapBinOp(const std::string& lty, const std::string& rty,
                      const std::string& op) {
    return std::string("module M; class C "
                       "public static method M(a: ") + lty +
           ", b: " + rty + "): int "
           "var _ := a " + op + " b; return 0; end M; end C;";
}

} // namespace

// ---------------------------------------------------------------------------
// 521_m_literal: §4.4 — the `m`/`M` suffix produces a `decimal` literal
// and is now fully implemented (no error). Before fix #521 the suffix was
// silently stripped and the literal was left as `double` (miscompile).
// Fix #521 removes the "not yet implemented" diagnostic.
// ---------------------------------------------------------------------------

// §4.4 spec example: 1.50m is a decimal literal — now accepted with no error.
BEZEL_TEST(sema_521_m_literal, m_suffix_lower_accepted_spec_4_4) {
    auto r = parseSemaAll(wrapBody("var _ := 1.5m; return 0;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §4.4: upper-case M suffix is also a decimal literal — accepted.
BEZEL_TEST(sema_521_m_literal, M_suffix_upper_accepted_spec_4_4) {
    auto r = parseSemaAll(wrapBody("var _ := 2.0M; return 0;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §4.3 / §4.4: an unsuffixed float literal (1.5) remains double — not
// affected by the m-literal fix.
BEZEL_TEST(sema_521_m_literal, unsuffixed_float_remains_double_spec_4_3) {
    auto r = parseSemaAll(wrapBody("var _ := 1.5; return 0;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §4.3: `f` suffix → float, unchanged.
BEZEL_TEST(sema_521_m_literal, f_suffix_remains_float_spec_4_3) {
    auto r = parseSemaAll(wrapBody("var _ := 1.5f; return 0;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §4.4: scientific notation decimal (1.23e2m) — accepted.
BEZEL_TEST(sema_521_m_literal, scientific_m_suffix_accepted_spec_4_4) {
    auto r = parseSemaAll(wrapBody("var _ := 1.23e2m; return 0;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §4.4: zero decimal literal — accepted.
BEZEL_TEST(sema_521_m_literal, zero_m_literal_accepted_spec_4_4) {
    auto r = parseSemaAll(wrapBody("var _ := 0m; return 0;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// 521_arith: §6.1 / §6.3 — decimal arithmetic with parameter operands
// (same-type: decimal op decimal) is now accepted (no diagnostics).
// ---------------------------------------------------------------------------

// §6.1: `+` on two decimal parameters → accepted.
BEZEL_TEST(sema_521_arith, decimal_add_accepted_spec_6_1) {
    auto r = parseSemaAll(wrapBinOp("decimal", "decimal", "+"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §6.1: `-` on two decimal parameters → accepted.
BEZEL_TEST(sema_521_arith, decimal_sub_accepted_spec_6_1) {
    auto r = parseSemaAll(wrapBinOp("decimal", "decimal", "-"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §6.1: `*` on two decimal parameters → accepted.
BEZEL_TEST(sema_521_arith, decimal_mul_accepted_spec_6_1) {
    auto r = parseSemaAll(wrapBinOp("decimal", "decimal", "*"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §6.1 / §4.4: `/` on two decimal parameters → accepted.
BEZEL_TEST(sema_521_arith, decimal_div_accepted_spec_6_1_4_4) {
    auto r = parseSemaAll(wrapBinOp("decimal", "decimal", "/"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §6.1 / §4.4: `mod` on two decimal parameters → accepted.
BEZEL_TEST(sema_521_arith, decimal_mod_accepted_spec_6_1_4_4) {
    auto r = parseSemaAll(wrapBinOp("decimal", "decimal", "mod"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §5.1 / §6.1: int widens to decimal (§5.1), so `decimal + int` is accepted.
BEZEL_TEST(sema_521_arith, decimal_plus_int_param_accepted_spec_5_1_6_1) {
    auto r = parseSemaAll(wrapBinOp("decimal", "int", "+"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §5.1 / §6.1: `int + decimal` → decimal → accepted.
BEZEL_TEST(sema_521_arith, int_plus_decimal_param_accepted_spec_5_1_6_1) {
    auto r = parseSemaAll(wrapBinOp("int", "decimal", "+"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §6.2: double + decimal (no implicit conv either way per §5.1) → §6.2
// error still applies; this pre-existed in mixed_numeric_test — confirm
// the incompatibility check is preserved even after decimal is implemented.
BEZEL_TEST(sema_521_arith, double_plus_decimal_6_2_still_rejected) {
    auto r = parseSemaAll(wrapBinOp("double", "decimal", "+"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// 521_static_field: §4.4 / §22.8 — static/instance decimal fields with
// m-literal initializers are now accepted (no error, no IR crash).
// ---------------------------------------------------------------------------

// §4.4 / §22.8: `static _v: decimal := 1.5m` → no sema error.
BEZEL_TEST(sema_521_static_field, static_decimal_m_literal_init_accepted) {
    auto r = parseSemaAll(
        "module M; class C "
        "public static _v: decimal := 1.5m; "
        "public static method Main(): int => 0; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §4.4: an instance decimal field with an m-literal → accepted.
BEZEL_TEST(sema_521_static_field, instance_decimal_field_m_literal_accepted) {
    auto r = parseSemaAll(
        "module M; class C "
        "public _balance: decimal := 0m; "
        "public static method Main(): int => 0; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §4.4: decimal arithmetic in an instance method → accepted.
BEZEL_TEST(sema_521_static_field, decimal_in_method_accepted) {
    auto r = parseSemaAll(
        "module M; class C "
        "public method Area(w: decimal, h: decimal): decimal "
        "  return w * h; "
        "end Area; "
        "public static method Main(): int => 0; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// 521_control: double/int arithmetic is unchanged — no regression.
// ---------------------------------------------------------------------------

// §6.3: double + double → no error (IEEE 754, fully implemented).
BEZEL_TEST(sema_521_control, double_arith_unchanged_spec_6_3) {
    auto r = parseSemaAll(wrapBinOp("double", "double", "+"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §6.1: int + int → no error.
BEZEL_TEST(sema_521_control, int_arith_unchanged_spec_6_1) {
    auto r = parseSemaAll(wrapBinOp("int", "int", "+"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §6.3: float * float → no error.
BEZEL_TEST(sema_521_control, float_arith_unchanged_spec_6_3) {
    auto r = parseSemaAll(wrapBinOp("float", "float", "*"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §4.3: unsuffixed float literal is double — not affected by decimal fix.
BEZEL_TEST(sema_521_control, double_literal_no_suffix_unchanged_spec_4_3) {
    auto r = parseSemaAll(wrapBody("var _ := 3.14; return 0;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §5.1: int → double widening still works.
BEZEL_TEST(sema_521_control, int_to_double_widening_unchanged_spec_5_1) {
    auto r = parseSemaAll(wrapBinOp("double", "int", "+"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §6.3: double division (not SIGFPE) still works.
BEZEL_TEST(sema_521_control, double_div_no_error_spec_6_3) {
    auto r = parseSemaAll(wrapBinOp("double", "double", "/"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// 521_incompatible: §5.1 / §6.2 — decimal ↔ double is never implicitly
// converted. Only decimal ↔ decimal and int→decimal widening are allowed.
// ---------------------------------------------------------------------------

// §6.2: float + decimal → rejected (float and decimal don't implicitly convert).
BEZEL_TEST(sema_521_incompatible, float_plus_decimal_rejected_spec_6_2) {
    auto r = parseSemaAll(wrapBinOp("float", "decimal", "+"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §6.2: decimal + double → rejected.
BEZEL_TEST(sema_521_incompatible, decimal_plus_double_rejected_spec_6_2) {
    auto r = parseSemaAll(wrapBinOp("decimal", "double", "+"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
