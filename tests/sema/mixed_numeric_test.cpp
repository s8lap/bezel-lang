// Spec-derived tests for §6.2 mixed-type arithmetic — the
// no-implicit-conversion-either-way diagnostic (issue #410, deferred
// from #408).
//
// §6.2: "When operands have different types, the smaller-range operand is
// implicitly promoted to the larger type using §5.1. The result has the
// larger type. If no implicit conversion exists between the operand types
// (e.g. `double` and `decimal`), the operation is a compile error and an
// explicit cast is required."
//
// §5.1 admits a numeric implicit conversion only when no value is lost.
// The conversion table (§5.1) leaves these numeric pairs with NO implicit
// conversion in EITHER direction — each is therefore a §6.2 compile error
// in a mixed binary arithmetic operation:
//   byte/sbyte, sbyte/ushort, sbyte/uint, sbyte/ulong, short/ushort,
//   short/uint, short/ulong, int/uint, int/ulong, long/ulong,
//   float/decimal, double/decimal.
//
// The complementary requirement is that pairs which DO have a §5.1
// conversion (int/long, int/double, float/double, byte/short, …) must
// NOT be over-rejected — they promote to the wider operand per §6.2 as
// before #408.

#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// A method body whose first statement exercises a mixed binary op on two
// typed parameters `a` and `b`. Parameters give genuinely-typed operands
// (decimal/uint literals are not yet front-end-typeable as such).
std::string wrap(const std::string& aTy, const std::string& bTy,
                 const std::string& op) {
    return std::string("module M; class C public static method M(a: ") +
        aTy + ", b: " + bTy + "): int " +
        "var r := a " + op + " b; return 0; end M; end C;";
}

bool anyMessageMentions(const bezel::test::SemaResult& r,
                        const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// §6.2 — the canonical incompatible pair: double and decimal.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_mixed_numeric, double_plus_decimal_is_error_spec_6_2) {
    auto r = parseSemaAll(wrap("double", "decimal", "+"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_mixed_numeric, decimal_plus_double_is_error_spec_6_2) {
    auto r = parseSemaAll(wrap("decimal", "double", "+"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// The diagnostic cites both operand type names (so the programmer knows
// which cast to add).
BEZEL_TEST(sema_mixed_numeric, error_cites_both_types_spec_6_2) {
    auto r = parseSemaAll(wrap("double", "decimal", "+"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyMessageMentions(r, "double"));
    BEZEL_EXPECT_TRUE(anyMessageMentions(r, "decimal"));
}

BEZEL_TEST(sema_mixed_numeric, float_plus_decimal_is_error_spec_6_2) {
    auto r = parseSemaAll(wrap("float", "decimal", "*"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// §6.2 / §5.1 — signed/unsigned pairs of the same or narrower width have no
// implicit conversion either way (§5.1: "Between signed and unsigned types
// of the same or narrower width") → §6.2 compile error.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_mixed_numeric, int_plus_uint_is_error_spec_6_2) {
    auto r = parseSemaAll(wrap("int", "uint", "+"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_mixed_numeric, uint_plus_int_is_error_spec_6_2) {
    auto r = parseSemaAll(wrap("uint", "int", "+"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_mixed_numeric, long_plus_ulong_is_error_spec_6_2) {
    auto r = parseSemaAll(wrap("long", "ulong", "-"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_mixed_numeric, int_plus_ulong_is_error_spec_6_2) {
    auto r = parseSemaAll(wrap("int", "ulong", "*"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_mixed_numeric, short_plus_ushort_is_error_spec_6_2) {
    auto r = parseSemaAll(wrap("short", "ushort", "+"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_mixed_numeric, byte_plus_sbyte_is_error_spec_6_2) {
    auto r = parseSemaAll(wrap("byte", "sbyte", "+"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// The bitwise operators flow through the same §6.2 promotion path; an
// incompatible signed/unsigned pair is rejected there too.
BEZEL_TEST(sema_mixed_numeric, int_bitand_uint_is_error_spec_6_2) {
    auto r = parseSemaAll(wrap("int", "uint", "&"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// NO over-rejection: pairs that DO have a §5.1 implicit conversion in some
// direction must still type-check (they promote to the wider operand).
// These are the regression guards for #408's widening fix.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_mixed_numeric, int_plus_double_ok_spec_6_2) {
    auto r = parseSemaAll(wrap("int", "double", "+"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_mixed_numeric, double_plus_int_ok_spec_6_2) {
    auto r = parseSemaAll(wrap("double", "int", "+"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_mixed_numeric, float_plus_double_ok_spec_6_2) {
    auto r = parseSemaAll(wrap("float", "double", "*"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_mixed_numeric, int_plus_long_ok_spec_6_2) {
    auto r = parseSemaAll(wrap("int", "long", "+"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_mixed_numeric, byte_plus_short_ok_spec_6_2) {
    // §5.1: byte → short is permitted (unsigned widening to a strictly
    // larger signed type loses no value).
    auto r = parseSemaAll(wrap("byte", "short", "+"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_mixed_numeric, uint_plus_long_ok_spec_6_2) {
    // §5.1: uint → long is permitted (fits without loss).
    auto r = parseSemaAll(wrap("uint", "long", "+"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_mixed_numeric, uint_plus_ulong_ok_spec_6_2) {
    // §5.1: uint → ulong is permitted.
    auto r = parseSemaAll(wrap("uint", "ulong", "+"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_mixed_numeric, int_plus_decimal_ok_spec_6_2) {
    // §5.1: int → decimal is permitted (integers widen to decimal).
    auto r = parseSemaAll(wrap("int", "decimal", "+"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Same-type operands are never a mixed-type case.
BEZEL_TEST(sema_mixed_numeric, int_plus_int_ok) {
    auto r = parseSemaAll(wrap("int", "int", "+"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_mixed_numeric, uint_plus_uint_ok) {
    auto r = parseSemaAll(wrap("uint", "uint", "+"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_mixed_numeric, decimal_plus_decimal_ok) {
    auto r = parseSemaAll(wrap("decimal", "decimal", "+"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
