// Spec-derived tests for Sema slice T1 — primitive assignability at a
// typed local initializer (§5.1 implicit conversions). Only
// primitive-to-primitive cases are checked; reference / named / null
// cases are deferred to the full assignability lattice.

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

std::string wrap(const std::string& decl) {
    return std::string("module M; class C public method M() ") + decl +
        " end M; end C;";
}

} // namespace

BEZEL_TEST(sema_assign, int_to_int_ok) {
    auto r = parseSemaAll(wrap("var x: int := 42;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_assign, int_to_long_widens_ok) {
    auto r = parseSemaAll(wrap("var x: long := 42;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_assign, int_to_double_widens_ok) {
    auto r = parseSemaAll(wrap("var x: double := 42;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_assign, bool_to_int_diagnoses) {
    auto r = parseSemaAll(wrap("var x: int := true;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_assign, int_to_bool_diagnoses) {
    auto r = parseSemaAll(wrap("var x: bool := 5;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_assign, string_to_int_diagnoses) {
    auto r = parseSemaAll(wrap("var x: int := \"hi\";"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_assign, double_to_int_narrowing_diagnoses) {
    // double does not implicitly convert to int (narrowing).
    auto r = parseSemaAll(wrap("var x: int := 3.14;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_assign, char_to_int_widens_ok) {
    auto r = parseSemaAll(wrap("var x: int := 'a';"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_assign, bool_to_bool_ok) {
    auto r = parseSemaAll(wrap("var x: bool := true;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_assign, inferred_local_no_ascription_no_check) {
    // No ascription → inference; no assignability check applies.
    auto r = parseSemaAll(wrap("var x := 42;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// #738 / §5.1: an integer LITERAL/constant narrows implicitly (representable);
// a non-constant int VALUE does not — it needs an explicit cast. Previously the
// retype-to-narrower applied to ANY int initializer, so `var y: sbyte := x`
// (x an int variable) was silently accepted and truncated.
BEZEL_TEST(sema_assign, int_var_to_sbyte_narrowing_rejected_738) {
    auto r = parseSemaAll(wrap(
        "var x: int := 300; var y: sbyte := x;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
BEZEL_TEST(sema_assign, int_var_to_byte_assign_narrowing_rejected_738) {
    auto r = parseSemaAll(wrap(
        "var x: int := 300; var b: byte := 1; b := x;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
BEZEL_TEST(sema_assign, int_literal_to_sbyte_still_ok_738) {
    auto r = parseSemaAll(wrap("var y: sbyte := 100;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
BEZEL_TEST(sema_assign, neg_int_literal_to_sbyte_still_ok_738) {
    auto r = parseSemaAll(wrap("var y: sbyte := -5;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
BEZEL_TEST(sema_assign, int_const_arith_to_byte_still_ok_738) {
    auto r = parseSemaAll(wrap("var b: byte := 10 + 20;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// #738 / §5.1: an int constant that does NOT fit the narrower target is rejected
// (no silent truncation). `300` does not fit `sbyte` (-128..127) or `byte`
// (0..255); a negative literal does not fit an unsigned target.
BEZEL_TEST(sema_assign, int_literal_out_of_range_sbyte_rejected_738) {
    auto r = parseSemaAll(wrap("var y: sbyte := 300;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
BEZEL_TEST(sema_assign, int_literal_out_of_range_byte_rejected_738) {
    auto r = parseSemaAll(wrap("var b: byte := 300;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
BEZEL_TEST(sema_assign, neg_int_literal_to_unsigned_byte_rejected_738) {
    auto r = parseSemaAll(wrap("var b: byte := -5;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
BEZEL_TEST(sema_assign, int_literal_out_of_range_byte_assign_rejected_738) {
    auto r = parseSemaAll(wrap("var b: byte := 1; b := 300;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
// In-range narrowing (incl. negative within sbyte) stays valid.
BEZEL_TEST(sema_assign, int_literal_in_range_sbyte_ok_738) {
    auto r = parseSemaAll(wrap("var y: sbyte := -100; var z: byte := 200;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
