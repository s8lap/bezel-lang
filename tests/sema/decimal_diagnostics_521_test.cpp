// Tests for issue #521: decimal type — eliminate silent miscompiles and
// IR crashes; produce clean compile-time diagnostics for unsupported
// decimal operations while full 128-bit decimal is deferred.
//
// Spec §4.4: decimal is a 28–29 significant digit high-precision type;
// the `m` suffix produces a decimal literal (1.5m). Full implementation
// is deferred; these tests confirm that unsupported decimal use is
// diagnosed cleanly rather than silently miscompiled or crashing.
//
// Test groups:
//   521_m_literal     — m-suffix literal types as decimal (not double)
//   521_arith         — decimal arithmetic emits §4.4 diagnostic
//   521_static_field  — static decimal field with m-literal: clean diag
//   521_control       — double/int arithmetic unchanged; no regression

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

bool anyMessageMentions(const bezel::test::SemaResult& r,
                        const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
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
// 521_m_literal: §4.4 — the `m` suffix produces a `decimal` literal, not
// `double`. Before fix #521 the suffix was silently stripped and the literal
// was left as `double` (ExprTypecheck_Exprs.cpp ~L327).
// ---------------------------------------------------------------------------

// §4.4 spec example: 1.50m is a decimal literal; decimal is not yet fully
// implemented → clean "not yet implemented" diagnostic.
BEZEL_TEST(sema_521_m_literal, m_suffix_lower_is_diagnosed_spec_4_4) {
    auto r = parseSemaAll(wrapBody("var _ := 1.5m; return 0;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyMessageMentions(r, "decimal"));
}

// §4.4: upper-case M suffix is also a decimal literal.
BEZEL_TEST(sema_521_m_literal, M_suffix_upper_is_diagnosed_spec_4_4) {
    auto r = parseSemaAll(wrapBody("var _ := 2.0M; return 0;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyMessageMentions(r, "decimal"));
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

// §4.4 spec example: 1.23e2m (scientific notation decimal) → diagnosed.
BEZEL_TEST(sema_521_m_literal, scientific_m_suffix_diagnosed_spec_4_4) {
    auto r = parseSemaAll(wrapBody("var _ := 1.23e2m; return 0;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyMessageMentions(r, "decimal"));
}

// §4.4: diagnostic message cites §4.4 so the programmer knows it is a
// deferred feature, not a permanent restriction.
BEZEL_TEST(sema_521_m_literal, diagnostic_cites_spec_section_4_4) {
    auto r = parseSemaAll(wrapBody("var _ := 1.5m; return 0;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyMessageMentions(r, "4.4") ||
                      anyMessageMentions(r, "§4.4"));
}

// ---------------------------------------------------------------------------
// 521_arith: §6.1 / §6.3 — decimal arithmetic with parameter operands
// (same-type: decimal op decimal) must produce a clean diagnostic, not
// silently miscompile as i32 arithmetic.
// ---------------------------------------------------------------------------

// §6.1: `+` on two decimal parameters → diagnosed, not silently miscompiled.
BEZEL_TEST(sema_521_arith, decimal_add_is_diagnosed_spec_6_1) {
    auto r = parseSemaAll(wrapBinOp("decimal", "decimal", "+"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyMessageMentions(r, "decimal"));
}

// §6.1: `-` on two decimal parameters → diagnosed.
BEZEL_TEST(sema_521_arith, decimal_sub_is_diagnosed_spec_6_1) {
    auto r = parseSemaAll(wrapBinOp("decimal", "decimal", "-"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §6.1: `*` on two decimal parameters → diagnosed.
BEZEL_TEST(sema_521_arith, decimal_mul_is_diagnosed_spec_6_1) {
    auto r = parseSemaAll(wrapBinOp("decimal", "decimal", "*"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §6.1 / §4.4: `/` on two decimal parameters → diagnosed (not SIGFPE).
// Before fix #521 this reached codegen as i32/i32, raising SIGFPE.
BEZEL_TEST(sema_521_arith, decimal_div_is_diagnosed_spec_6_1_4_4) {
    auto r = parseSemaAll(wrapBinOp("decimal", "decimal", "/"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyMessageMentions(r, "decimal"));
}

// §6.1 / §4.4: `mod` on two decimal parameters → diagnosed.
// (§6.1 states mod is defined on decimal; DivideByZeroException on zero.)
BEZEL_TEST(sema_521_arith, decimal_mod_is_diagnosed_spec_6_1_4_4) {
    auto r = parseSemaAll(wrapBinOp("decimal", "decimal", "mod"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §5.1 / §6.1: int widens to decimal (§5.1), so `decimal + int` is a
// decimal operation → also diagnosed.
BEZEL_TEST(sema_521_arith, decimal_plus_int_param_is_diagnosed_spec_5_1_6_1) {
    auto r = parseSemaAll(wrapBinOp("decimal", "int", "+"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §5.1 / §6.1: `int + decimal` → decimal → diagnosed.
BEZEL_TEST(sema_521_arith, int_plus_decimal_param_is_diagnosed_spec_5_1_6_1) {
    auto r = parseSemaAll(wrapBinOp("int", "decimal", "+"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §6.2: double + decimal (no implicit conv either way per §5.1) → §6.2
// error; this pre-existed in mixed_numeric_test — confirm no regression.
BEZEL_TEST(sema_521_arith, double_plus_decimal_6_2_no_regression) {
    auto r = parseSemaAll(wrapBinOp("double", "decimal", "+"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// 521_static_field: §4.4 / §22.8 — a static decimal field with an m-literal
// initializer must produce a clean diagnostic, NOT an IR verification crash
// (double ConstantFP stored into an i32 global: #521 sub-bug 4).
// ---------------------------------------------------------------------------

// §4.4 / §22.8: `static _v: decimal := 1.5m` → clean sema diagnostic.
// Before fix #521 this reached codegen and crashed the IR verifier.
BEZEL_TEST(sema_521_static_field, static_decimal_m_literal_init_diagnosed) {
    auto r = parseSemaAll(
        "module M; class C "
        "public static _v: decimal := 1.5m; "
        "public static method Main(): int => 0; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyMessageMentions(r, "decimal"));
}

// §4.4: an instance decimal field with an m-literal → same clean diagnostic.
BEZEL_TEST(sema_521_static_field, instance_decimal_field_m_literal_diagnosed) {
    auto r = parseSemaAll(
        "module M; class C "
        "public _balance: decimal := 0m; "
        "public static method Main(): int => 0; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyMessageMentions(r, "decimal"));
}

// §4.4: decimal arithmetic in an instance method → diagnosed.
BEZEL_TEST(sema_521_static_field, decimal_in_method_is_diagnosed) {
    auto r = parseSemaAll(
        "module M; class C "
        "public method Area(w: decimal, h: decimal): decimal "
        "  return w * h; "
        "end Area; "
        "public static method Main(): int => 0; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
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
