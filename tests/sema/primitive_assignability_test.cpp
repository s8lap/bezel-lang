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
